# Design

## Context

参见 `proposal.md` 的动机。当前 `code/day29` 已包含 epoll、Reactor、线程池、TCP 连接、缓冲区、timerfd 和异步日志，但它是按天保存的教学快照，并存在几个会阻碍代理实现的结构问题：`TcpConnection` 持有 `HttpContext`；`Channel` 缺少读写事件关闭和完整错误回调；连接注册可能跨越 EventLoop 线程；缓冲区边界、weak guard 和连接关闭仍需加固；系统也没有主动非阻塞连接后端的组件。

新实现必须保留历史快照，运行于 Linux，使用 C++14、CMake、pthreads 和已有 jsoncpp 依赖。行为契约见 `specs/l4-load-balancing/spec.md`。

## Goals / Non-Goals

**Goals:**

- 建立职责清楚、可单元测试的 `base -> net -> lb -> app` 分层。
- 让每个代理会话的两个连接归属同一 EventLoop，使数据路径无需跨线程锁。
- 正确处理非阻塞连接、双向转发、背压、半关闭、超时和幂等销毁。
- 将后端调度与健康状态从网络细节中分离，便于独立测试和扩展算法。
- 提供足够的指标、故障测试和基准数据，使性能与可靠性结论可复现。

**Non-Goals:**

- 不把项目描述为内核态 LVS，也不实现 NAT、DR 或 TUN 转发模式。
- 不在数据面终止 TLS 或解析 HTTP；管理端 HTTP 只承载只读指标。
- 不在首版实现 UDP、IPv6、动态服务发现、配置热更新、跨进程状态共享、零拷贝或内核旁路。
- 不追求修改每个历史 day 目录；历史代码只作为提炼依据。

## Decisions

### 1. 在 `l4lb/` 中创建独立工程

目录采用：

```text
l4lb/
  CMakeLists.txt
  config/example.json
  src/base/
  src/net/
  src/lb/
  src/config/
  src/app/
  tests/unit/
  tests/integration/
  tests/benchmark/
```

`base`、`net` 和 `lb` 分别构建为库，最终由 `l4lb` 可执行文件组合。这样既能展示从现有项目演进的关系，又避免在教程快照之间复制修改。

替代方案是在 `code/day30` 上继续开发。该方案会把生产化重构与教程时间线混合，并使测试、文档和构建入口继续分散，因此不采用。

### 2. 使用单 Acceptor 加 N 个 worker EventLoop

主 EventLoop 负责监听和 accept，然后轮询选择 worker，并把新连接的创建与 Channel 注册投递到该 worker。一个 `ProxySession` 的前端连接、后端 Connector 和后端连接全部在同一 worker 中运行；所有会话状态只能在所属 EventLoop 线程读写。

```text
main loop: accept fd
       |
       v queueInLoop
worker loop: frontend + connector + backend + session
```

这消除了双向转发中的跨线程消息队列和锁。替代方案是让前后端连接各自落在不同 worker 上，但每个数据块都需要跨线程转交，生命周期也更难证明，因此不采用。每连接一线程同样因线程数和上下文切换成本而不采用。

### 3. 先把网络库变成协议无关层

`TcpConnection` 不再持有 `HttpContext`，只暴露连接、消息、写完成、高水位、半关闭和关闭回调。`Channel` 增加 Enable/Disable Read/Write、Remove，以及 Read/Write/Close/Error 回调，并在 weak guard 失效时跳过事件处理。所有 Channel 更新必须发生在所属 EventLoop；EventLoop 提供 loop-thread 断言、QueueInLoop、Quit 和安全唤醒。

`Buffer` 使用 `size_t` 索引，允许恰好消费全部数据，并提供最大缓存约束所需的可读/可写操作。Socket 选项和地址转换收敛到 `Socket`、`InetAddress`，避免裸 fd 操作散落在业务层。

### 4. 用 Connector 完成非阻塞后端连接

`Connector` 创建 nonblocking socket，调用 `connect()`；立即成功直接完成，`EINPROGRESS` 时监听可写事件，并通过 `getsockopt(SO_ERROR)` 判断最终结果。连接超时由所属 EventLoop 的定时器触发。Connector 只负责产生一个成功连接的 fd 或失败原因，不负责选择后端和重试。

将重试留给 `ProxySession` 可以确保尝试过的节点、暂存数据、重试上限和客户端生命周期由一个对象统一管理。

### 5. ProxySession 是双连接生命周期边界

worker 中的 SessionMap 持有 `shared_ptr<ProxySession>`；ProxySession 独占两个 TcpConnection，连接回调只弱引用 Session，避免引用环。会话经历：

```text
Accepted -> Connecting -> Established -> Draining -> Closed
                |              |
                +-- Failed ----+
```

Connecting 状态允许在上限内暂存客户端数据。Established 状态把一侧读缓冲移动到另一侧写缓冲。任一方向读取 EOF 后标记该方向结束；对应缓存发送完成后对目的 socket 执行 `shutdown(SHUT_WR)`。只有两个方向都结束且缓存清空，或出现 terminal error，才从 SessionMap 幂等移除。

连接失败最多按配置重试尚未尝试的健康后端。已经成功向某个后端转发数据后不再自动切换节点，避免在不同后端重复或分裂应用层会话。

### 6. 使用高低水位实现背压

每个 TcpConnection 的 output buffer 设高、低水位。向后端的缓冲达到高水位时关闭前端读事件；向客户端的缓冲达到高水位时关闭后端读事件。写完成使缓存下降到低水位后，再恢复对端读取。

高低两个阈值避免在单一阈值附近频繁切换 epoll interest。首版使用用户态 Buffer，`splice()` 或 io_uring 作为基准完成后的可选优化，避免过早增加生命周期复杂度。

### 7. 调度策略与后端状态解耦

`BackendPool` 保存稳定 backend id、地址、配置权重、健康状态以及原子活跃会话计数。`LoadBalancer` 接口接收一次只读候选快照：

- Round Robin 使用单调序号对健康候选取模。
- Least Connections 选择活跃会话最少的节点，并按配置顺序稳定打破平局。

选择成功后立即增加活跃计数，Session 结束时通过只执行一次的 lease/deleter 扣减，避免错误路径漏计数。健康检查线程发布不可变候选快照；数据线程读取快照时不持有健康检查锁。C++14 下使用 `atomic_load`/`atomic_store` 操作 `shared_ptr<const BackendSnapshot>`。

首版不实现权重调度，配置中的权重先验证并保留为未来扩展字段；避免“支持权重”但算法语义不完整。Round Robin 与 Least Connections 的首版验收均按等权节点进行。

### 8. 健康检查属于控制面

独立控制 EventLoop 按周期对每个后端执行 TCP connect 探测。每个节点维护连续成功和失败计数，仅在达到阈值时发布 UP/DOWN 转换。状态变化只重建候选快照并记录日志，不访问或终止已有 ProxySession。

被动连接失败会增加指标并可作为日志信号，但首版不直接改变健康状态，防止业务流量中的瞬时失败与主动探测竞争。

### 9. 启动配置固定，指标使用独立管理端口

启动时读取一次 JSON 配置，先解析到临时对象并完成全量校验，再构造运行组件，避免半初始化监听。配置包含 `listen`、`admin_listen`、`workers`、`algorithm`、`backends`、`timeouts`、`buffer_watermarks`、`health_check`、`max_connect_retries` 和 `shutdown_grace_ms`。

管理端使用独立 listener，仅实现 `GET /metrics` 和健康响应。指标内部使用原子计数和按请求聚合的后端快照，数据线程不等待管理端。管理协议不是数据面 HTTP 路由，不改变四层代理定位。

### 10. 分两阶段完成优雅退出

信号处理器只写入 eventfd/设置异步信号安全标记。主循环收到通知后关闭 Acceptor、停止新健康探测并通知所有 worker 进入 draining。现有 Session 正常运行至全部结束或宽限期到达；随后每个 worker 在自己的线程关闭剩余连接、退出 EventLoop，最后刷新日志并 join 线程。

### 11. 以确定性测试先行验证边界

单元测试覆盖 Buffer、Channel 事件位、调度算法、后端 lease、配置校验和 Session 状态转换。集成测试启动多个可控 echo/HTTP 后端，验证二进制完整性、调度分布、连接失败重试、健康摘除恢复、慢读背压、半关闭、reset、空闲超时和优雅退出。

基准测试在同机固定 CPU/连接数/负载下分别测量直连与代理，报告吞吐、P50/P99、CPU、RSS 和错误率，而不是只给出单个 QPS 数字。HTTP 基准可使用 wrk 验证 TCP 上的常见工作负载，原始 TCP 使用独立客户端工具或仓库内压测程序。

### 12. 用 RAII 明确资源所有权与线程归属

所有内核资源和生命周期计数都由一个明确的 C++ 对象拥有，资源释放绑定到对象析构，不允许业务层散落裸 `close()`、`delete` 或手工配对的计数增减。拥有资源的类型不可复制；需要转移所有权时只允许显式移动。

```cpp
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept;
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    int get() const noexcept;
    int release() noexcept;
    void reset(int fd = -1) noexcept;
};
```

资源关系固定如下：

- `Socket` 独占一个 `UniqueFd`；`Acceptor`、`Connector` 和 `TcpConnection` 通过拥有 `Socket` 间接拥有各自 fd。
- `Connector` 在连接建立期间拥有 pending socket；成功后以移动语义把 `Socket` 移交给 `TcpConnection`，失败、取消或超时时由 `Connector` 析构路径自动关闭。
- `Channel` 不拥有 fd，只表示该 fd 在某个 `EventLoop` 中的事件注册；拥有 Channel 的对象必须先在所属 loop 线程执行 `Remove()`，再销毁 Channel 和 Socket。
- `TcpConnection` 拥有其 `Socket`、`Channel`、输入 Buffer 和输出 Buffer；关闭操作负责在 loop 线程解除注册，析构函数只回收已经脱离 epoll 的资源。
- `SessionMap` 是 `ProxySession` 的强引用根；`ProxySession` 强引用前后端连接和 Connector，连接回调只捕获 `weak_ptr<ProxySession>`，Channel 的 tie/guard 只弱引用连接，避免引用环。
- 定时器注册返回可取消的 `TimerId`；需要自动随作用域撤销的连接建立超时、空闲超时和健康探测使用 move-only `ScopedTimer`，其 `Cancel()` 和析构均幂等。
- 后端选择返回 move-only `BackendLease`，构造成功时增加活跃会话数，移动后源对象失效，析构时只扣减一次；任何失败、超时或重复关闭路径都不手工修改计数。
- `EventLoopThread` 拥有 `std::thread` 和子线程中的 EventLoop 生命周期；析构前必须请求 `Quit()` 并 `join()`，不允许遗留 joinable thread 或 detached thread。
- `ProxySession::Close()` 是会话资源释放的唯一入口并且幂等；它取消定时器、关闭两端连接并从 SessionMap 移除自身，最终由 RAII 回收 Buffer、Channel、Socket 和 BackendLease。

析构函数不得从任意线程直接执行 `epoll_ctl`。若最后一个外部引用可能在非所属线程释放，必须先通过 `QueueInLoop` 把逻辑关闭与最后一个强引用的释放投递到所属 EventLoop。这样 RAII 负责资源最终回收，EventLoop 线程规则负责回收发生在正确位置，两者共同保证安全。

测试需要验证默认构造、移动构造、移动赋值、`release/reset`、正常关闭、连接失败、超时、取消、重复关闭及异常提前返回；每条路径均要求 fd、Channel、Timer、线程和 backend lease 恰好释放一次。

## Risks / Trade-offs

- [从教学代码提炼时携带隐藏生命周期缺陷] -> 先完成网络层测试和 sanitizers，再接入代理逻辑；禁止直接整目录复制后开始功能开发。
- [慢端导致单会话或全局内存压力] -> 使用双向水位、配置上限和相关指标，并在集成测试中持续制造慢读。
- [ET 模式漏读或漏写造成连接停滞] -> 所有 read/accept/write/connect 处理遵循 drain-until-EAGAIN，并为边缘触发编写压力测试。
- [健康检查与真实连接结果短暂不一致] -> 使用阈值和不可变快照；连接失败仍走有界重试，不假设健康状态绝对实时。
- [Least Connections 的原子计数只能近似同时到达的全局最小值] -> 接受短暂并列误差，用稳定 tie-break 保证可测试性；避免在数据路径加全局互斥锁。
- [管理端扩大攻击面] -> 默认绑定 loopback，只实现只读固定端点，限制请求大小和空闲时间。
- [C++14 限制部分现代所有权工具] -> 明确使用 shared_ptr 原子自由函数和 RAII lease，不引入依赖编译器扩展的实现。

## Migration Plan

1. 在不修改 `code/dayNN` 的情况下建立独立工程、测试入口和示例配置。
2. 分组件提炼 base/net 代码，每移入一个组件就增加测试并运行 AddressSanitizer/UndefinedBehaviorSanitizer。
3. 先用固定单后端完成单线程代理，再开启多 worker、调度、健康检查和管理面。
4. 通过端到端故障矩阵和基准门槛后，将根 README 增加新工程入口；原 WebServer 构建与教程链接保持可用。
5. 若新工程构建或稳定性不达标，回滚仅需移除/禁用 `l4lb` 构建入口，不影响历史快照。

## Open Questions

- 项目对外名称可在实现或 README 阶段从 `MiniLB`、`FluxGate` 等候选中确定，不影响目录、协议或任务拆分。
- 基准测试采用 tcpkali、iperf 的代理模式还是仓库内自建 TCP 压测器，可根据目标 Linux 环境已有工具决定；报告指标集合保持不变。
