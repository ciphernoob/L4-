# L4 负载均衡器：架构、业务逻辑与面试学习指南

这份文档的目标不是逐行翻译代码，而是帮你建立三层理解：

1. **架构设计**：系统为什么这样分层，线程和对象如何归属。
2. **业务逻辑**：一条 TCP 连接从接入、选择后端、双向转发到关闭的完整过程。
3. **关键技术**：Reactor、非阻塞 connect、背压、半关闭、RAII、健康快照与优雅退出如何落地。

阅读时建议同时打开 `src/app/L4ProxyServer.cpp`、`src/lb/ProxySession.cpp` 和 `tests/unit/ProxySessionTest.cpp`。

## 1. 项目定位

本项目是 Linux 用户态四层 TCP full proxy。对每个会话，负载均衡器维护两条独立的 TCP 连接：

```text
client              L4 load balancer               backend
   |                       |                           |
   |--- frontend TCP ----->|                           |
   |                       |---- backend TCP -------->|
   |<========= opaque bidirectional byte stream =====>|
```

“L4”的核心含义是数据面不理解 HTTP、Redis 或自定义协议，只转发字节。管理端的 HTTP 接口是另一条控制链路，不会让数据面变成 L7 代理。

### 面试问答

**Q：这个项目与 LVS 或 Nginx 有什么区别？**

A：它是用户态 TCP full proxy，会终止客户端 TCP，再建立后端 TCP，因此可以在用户态做调度、背压、健康检查和指标。LVS 主要在内核转发路径工作，性能与透明性更强；Nginx 既能做 L4 stream 也能做 L7 HTTP，功能和工程成熟度远高于本学习项目。

**Q：既然不解析 HTTP，为什么仍然能代理 HTTP？**

A：HTTP 最终也是 TCP 字节流。只要字节顺序、内容和 TCP 半关闭语义不被破坏，上层协议就能正常工作。

## 2. 总体架构

```text
                           base/control EventLoop
                 +------------------------------------+
                 | data Acceptor   admin Acceptor     |
                 | HealthChecker   graceful shutdown  |
                 +-----------------+------------------+
                                   |
                         round-robin fd dispatch
                  +----------------+----------------+
                  v                v                v
             worker loop 0    worker loop 1    worker loop N
             SessionMap       SessionMap       SessionMap
                  |                |                |
             ProxySession     ProxySession     ProxySession
             /          \     /          \     /          \
       frontend TcpConn  backend TcpConn (same worker thread)
```

代码分层：

| 层 | 职责 | 核心对象 |
|---|---|---|
| `net` | 通用异步网络基础 | `Socket`、`Channel`、`Epoller`、`EventLoop`、`TcpConnection` |
| `lb` | 负载均衡领域逻辑 | `BackendPool`、`LoadBalancer`、`Connector`、`ProxySession` |
| `config` | JSON 配置及启动前校验 | `ServerConfig` |
| `app` | 组装数据面、管理面和生命周期 | `L4ProxyServer`、`main` |
| `base` | 跨模块基础能力 | `AsyncLogger` |

依赖方向是 `app -> lb -> net -> Linux`。`TcpConnection` 不知道什么是后端或调度算法，从而保持网络库的通用性。

### 面试问答

**Q：为什么不让前端和后端连接分属不同 worker？**

A：同一会话的状态、Buffer 和 Channel 都由一个 EventLoop 串行化访问，转发路径无需锁和跨线程拷贝。代价是一个会话只能使用一个 worker 的执行能力，但 TCP 会话本身要求有序，这是合理取舍。

**Q：主 Reactor 会不会成为瓶颈？**

A：主 Reactor 只承担 accept、管理端和健康检查，大量字节搬运在 worker 中完成。高新建连接率下仍可能成为瓶颈，可进一步考虑 `SO_REUSEPORT` 多 acceptor、批量投递或更高效的连接分发。

## 3. 模块一：RAII 与资源所有权

### 架构意图

网络程序的难点不只是成功路径，而是超时、reset、重试和停机交叉时能否只释放一次。项目把资源与对象生命周期绑定：

- `UniqueFd` move-only，析构时 `close`。
- `Socket` 拥有 `UniqueFd`，`Channel` 只观察 fd，不关闭 fd。
- `TimerId` move-only，析构或 `Cancel()` 时幂等取消。
- `BackendLease` 构造时增加活跃数，析构时减少。
- `EventLoopThread` 停止后 join，不留 detached 线程。
- `ProxySession::Close()` 统一释放两端连接、Connector、定时器和 lease。

### 关键实现

`Channel` 必须先从 EventLoop 中 remove，`Socket` 才能关闭 fd，否则 epoll 可能返回指向已销毁 Channel 的事件。`TcpConnection::ForceClose()` 将这个顺序集中起来。

### 面试问答

**Q：为什么 `Channel` 不应该拥有 fd？**

A：`Channel` 是事件兴趣和回调的适配器，fd 的业务生命周期属于 `Socket/TcpConnection`。分离所有权可避免 Channel 与 Socket 同时 close 造成 double-close，也使 epoll 注册关系更清晰。

**Q：RAII 能解决所有异步生命周期问题吗？**

A：不能。RAII 解决“对象销毁时如何释放资源”，但异步回调还要解决“对象是否仍存活”。项目使用 `shared_ptr/weak_ptr`、`Channel::Tie()` 和 loop-thread 约束防止悬空回调。

## 4. 模块二：Reactor 网络核心

### 业务逻辑

```text
EventLoop::Loop
  -> Epoller::Poll
  -> epoll_wait
  -> active Channel
  -> Channel::HandleEvent
  -> TcpConnection::HandleRead/HandleWrite
  -> ProxySession callback
```

`Epoller` 只做 ADD/MOD/DEL 和就绪事件返回；`Channel` 保存某个 fd 的 interest 与 callback；`EventLoop` 提供线程亲和的执行环境。

跨线程任务通过 `QueueInLoop()` 放入队列，然后写 `eventfd` 唤醒阻塞在 `epoll_wait` 的 loop。定时器使用 `timerfd`，因此 socket、唤醒和定时器都可由同一个 epoll 事件模型处理。

### 面试问答

**Q：`RunInLoop` 和 `QueueInLoop` 有什么区别？**

A：在 loop 所属线程调用 `RunInLoop` 可立即执行，否则入队；`QueueInLoop` 始终延后到 pending functor 阶段。后者适合避免在当前回调栈中重入修改容器或 Channel。

**Q：为什么使用 eventfd，不用条件变量？**

A：条件变量无法直接进入 epoll 等待集。eventfd 是 fd，跨线程写它就能让 `epoll_wait` 立即返回，使唤醒机制与 Reactor 统一。

**Q：ET 模式最容易犯什么错？**

A：没有循环 read/accept 到 `EAGAIN`。ET 只在状态变化时通知，本次不排空内核队列可能永久等不到下一次通知。

## 5. 模块三：Multi-Reactor 线程模型

`EventLoopThreadPool` 为每个 worker 创建独立 EventLoop。主线程 accept 后轮询选择 worker，将 move-only `Socket` 投递过去，到 worker 线程后才创建 `TcpConnection` 和注册 Channel。

关键不变式：

- Channel 只能在所属 EventLoop 线程注册、修改和删除。
- `SessionMap` 只在对应 worker 访问。
- 会话内部对象不依赖互斥锁；跨 worker 共享的指标和后端计数使用原子变量。

### 面试问答

**Q：one loop per thread 的优点是什么？**

A：用线程归属代替大量锁，同一连接的事件按顺序处理，同时通过多个 loop 利用多核。它也要求所有连接操作遵守线程亲和性，跨线程只能投递任务。

**Q：这里的 worker 调度和后端调度是一回事吗？**

A：不是。worker 调度决定本机哪个 EventLoop 处理客户会话；后端调度决定该会话连接哪台业务服务器。

## 6. 模块四：TcpConnection、Buffer 与字节流

### 读路径

```text
EPOLLIN -> HandleRead -> read until EAGAIN
        -> input_buffer.Append
        -> message_callback(ProxySession)
```

### 写路径

```text
Send -> SendInLoop -> try send directly
                    -> unsent bytes enter output_buffer
                    -> enable EPOLLOUT
EPOLLOUT -> HandleWrite -> drain output_buffer
                         -> disable EPOLLOUT when empty
```

`TcpConnection` 不在跨线程 `Send()` 中捕获原始指针，而是先复制成 `std::string` 再投递，避免回调执行时原数据已失效。

### 面试问答

**Q：为什么要有 output buffer？**

A：非阻塞 socket 的 `send` 可能只写一部分或返回 `EAGAIN`。剩余数据必须保存，等 EPOLLOUT 再续写，否则会丢数据。

**Q：TCP 是消息协议吗？一次 send 是否对应一次 read？**

A：不是。TCP 是有序字节流，可能拆包或合并。L4 代理不需要恢复上层消息边界，但必须保证所有字节有序、不丢失、不重复。

**Q：为什么读到 0 不能立即销毁会话？**

A：读到 0 只表示对端关闭了它的写方向，本端仍可能需要把缓存发完或继续向对端写响应，这是 TCP half-close。

## 7. 模块五：后端模型与调度算法

`Backend` 包含稳定 id、地址、权重、健康状态与原子活跃会话数。`BackendPool` 对外发布不可变的健康候选快照，选择路径只读快照，不遍历正在修改的容器。

- Round Robin：在健康候选中按顺序轮转。
- Least Connections：选择活跃数最少的节点，平局按配置顺序。
- 选择成功返回 move-only `BackendLease`，使计数与会话生命周期绑定。

### 面试问答

**Q：Least Connections 一定比 Round Robin 好吗？**

A：不一定。LC 适合会话时长差异较大的场景，但“连接数”不等于真实负载，而且需要可靠的全局计数。RR 简单、确定、开销低，后端同质且短连接时往往足够。

**Q：为什么使用不可变快照？**

A：健康更新少、选择读取多。copy-on-write 快照把同步成本移到低频更新路径，读者获得一个生命周期稳定的视图，避免遍历时容器被修改。

**Q：节点变为 DOWN 时为什么不关闭已有连接？**

A：健康检查只代表新建连接的选择决策。强制中断已有会话会破坏业务，而且已有 TCP 可能仍正常。因此新快照排除 DOWN 节点，旧 lease 继续持有原 Backend。

## 8. 模块六：Connector 与主动健康检查

### 非阻塞 connect

```text
socket(nonblocking)
  -> connect
     -> 0: immediate success
     -> EINPROGRESS: watch EPOLLOUT
  -> EPOLLOUT
  -> getsockopt(SO_ERROR)
     -> 0: transfer Socket ownership
     -> error: report failure
```

EPOLLOUT 不等于 connect 成功，必须读 `SO_ERROR`。Connector 同时注册可取消的超时定时器，成功、失败、超时和用户取消最终走幂等清理。

`HealthChecker` 为每个后端周期性创建 Connector，连续失败达阈值才标记 DOWN，连续成功达阈值才恢复 UP，避免短暂抖动导致频繁摘除/恢复。

### 面试问答

**Q：健康检查为什么不与业务会话共用连接？**

A：探测是控制面行为，应当与业务数据和生命周期隔离。独立短连接可测试新建 TCP 能力，也不会污染已有会话。

**Q：TCP connect 探测有什么局限？**

A：它只能证明端口能建立 TCP，不能证明应用逻辑正常。生产中可以增加协议级健康检查，但那会引入 L7 知识和更复杂的超时/响应校验。

## 9. 模块七：ProxySession 核心业务状态机

### 状态转换

```text
Accepted -> Connecting -> Established -> Draining -> Closed
                 |             |             |
                 +-------------+-------------+--> Closed on error/timeout
```

- `Accepted`：前端已接入，会话尚未启动。
- `Connecting`：已选后端，客户早到数据留在 frontend input buffer。
- `Established`：两端已建立，双向转发。
- `Draining`：至少一个方向 EOF，继续排空缓存并传播半关闭。
- `Closed`：终态，释放全部会话资源。

### 双向转发

```text
frontend.input_buffer  --HandleFrontendData--> backend.output_buffer
backend.input_buffer   --HandleBackendData---> frontend.output_buffer
```

后端连接失败时，会话会释放当前 lease，过滤已尝试 id，再在未尝试的健康后端中选择。一旦进入 Established，就不再中途切换后端，否则新后端无法理解旧连接已交互的协议状态。

### 高低水位背压

```text
backend output >= high -> frontend.StopRead()
backend output <= low  -> frontend.StartRead()

frontend output >= high -> backend.StopRead()
frontend output <= low  -> backend.StartRead()
```

高、低两个阈值构成滞回区间，避免缓冲在单一阈值附近波动时频繁 MOD epoll interest。暂停的始终是产生数据的来源方。

### 半关闭

客户 EOF 后不能立即 close：代理需要先把客户数据发完，再对后端 `shutdown(SHUT_WR)`，同时仍允许后端返回完整响应。只有双方 EOF 且两个 output buffer 都排空才完成关闭。

### 面试问答

**Q：为什么后端连接期间要缓存客户数据？**

A：非阻塞 connect 有时间窗，客户可能在后端成功前已发数据。不缓存就只能丢弃或过早拒绝读取。本项目保留在 frontend input buffer，达到高水位后停读，连接成功后再有序发送。

**Q：背压为什么不直接丢包？**

A：TCP 应用期待可靠有序字节流，用户态代理丢字节会静默破坏协议。正确做法是暂停 EPOLLIN，让压力继续传导到内核接收缓冲和发送端 TCP 窗口。

**Q：为什么 established 后不能故障转移到新后端？**

A：L4 代理不理解上层会话状态，不知道已发数据的语义，也无法让新后端恢复旧状态。只能在 connect 成功前安全重试。

**Q：如何保证多个终止事件同时到达时不 double free？**

A：所有终止路径收敛到 `ProxySession::Close()`，它先检查并设置 `Closed`，后续调用立即返回。再结合 RAII、weak callback 和 loop-thread 串行化，保证 fd、Channel、Timer 和 lease 只释放一次。

## 10. 模块八：L4ProxyServer、配置与生命周期

### 启动流程

```text
parse and validate JSON
  -> create base EventLoop
  -> block SIGINT/SIGTERM before any child thread
  -> construct L4ProxyServer
  -> start worker pool
  -> create worker-owned SessionMap
  -> start data/admin Acceptor
  -> start HealthChecker
  -> EventLoop::Loop
```

配置在 bind/listen 之前完成全量校验，避免进程已部分启动后才发现水位、算法或后端地址无效。

### 优雅退出

Linux `signalfd` 把 SIGINT/SIGTERM 转为 EventLoop 事件。信号必须在创建日志和 worker 线程前屏蔽，使新线程继承 mask，否则信号可能命中其他线程并直接终止进程。

```text
signal
  -> stop data/admin acceptors
  -> stop health checker
  -> wait existing sessions to drain
  -> grace deadline: force-close remaining sessions
  -> stop loops, join threads, flush logger
```

### 面试问答

**Q：为什么不在信号处理函数中直接 Stop？**

A：传统 signal handler 中只能调用 async-signal-safe 函数，锁、容器、`shared_ptr` 和大部分 C++ 逻辑都不安全。signalfd 让停机逻辑回到普通 EventLoop 上下文执行。

**Q：优雅退出为什么需要 deadline？**

A：只等待会话自然结束可能因长连接永不退出。宽限期在业务完整性和运维可控性之间做取舍，超时后必须强制收敛。

## 11. 模块九：管理面、指标与异步日志

管理 listener 默认绑定 loopback，提供：

- `GET /healthz`：进程健康响应。
- `GET /metrics`：当前/累计会话、双向字节、后端状态与活跃数、连接失败、超时和背压次数。

指标在 worker 中通过原子变量更新，避免数据面加全局锁。`AsyncLogger` 只在前台组装 JSON 记录并入队，单独线程串行输出，停机时 flush 后 join。

### 面试问答

**Q：原子指标一定没有性能问题吗？**

A：不一定。多 worker 频繁写同一 cache line 会产生缓存一致性开销和 false sharing。高性能版本可使用 per-worker 分片计数，查询时再聚合。

**Q：为什么日志要异步？**

A：磁盘或终端 I/O 延迟不可控，同步写会阻塞 EventLoop。异步日志把格式化后的记录交给后台线程，但要处理队列增长、停机 flush 和日志线程生命周期。

**Q：`/healthz` 返回 200 是否代表所有后端都健康？**

A：不一定。当前它是进程存活端点。后端健康需要查询 metrics。生产系统通常还会区分 liveness 和 readiness。

## 12. 一条完整会话的业务时序

```text
1. Acceptor accept 客户 fd
2. L4ProxyServer 选择 worker，投递 Socket
3. worker 创建 frontend TcpConnection
4. SessionMap 创建并持有 ProxySession
5. ProxySession 安装回调，进入 Connecting
6. LoadBalancer 从健康快照选择 BackendLease
7. Connector 异步连接后端
8. 后端成功，创建 backend TcpConnection
9. 转发连接期间累积的客户数据
10. 双向 message callback 透明转发
11. 高水位停止来源读，低水位恢复
12. EOF 后进入 Draining，排空后传播 shutdown-write
13. 双向完成或错误/超时进入 Close
14. 从 SessionMap 移除，RAII 释放所有资源
```

## 13. 如何用测试学习实现

| 想学的内容 | 先读的测试 |
|---|---|
| Buffer/RAII | `BufferTest.cpp`、`SocketTest.cpp`、`UniqueFdTest.cpp` |
| Reactor 与跨线程唤醒 | `EventLoopTest.cpp`、`EventLoopThreadTest.cpp` |
| 非阻塞 connect | `ConnectorTest.cpp` |
| RR/LC 与 lease | `LoadBalancerTest.cpp` |
| 双向转发和半关闭 | `ProxySessionForwardsLargeOpaqueStreamsAndPreservesHalfClose` |
| 背压 | `ProxySessionAppliesAndReleasesBackpressureInBothDirections` |
| 重试 | `ProxySessionRetriesOnlyUnusedBackendsAndKeepsBufferedData` |
| reset 与幂等关闭 | `ProxySessionHandlesBackendResetAndReleasesOwnershipExactlyOnce` |
| 健康摘除/恢复 | `HealthCheckerTest.cpp` |
| Multi-Reactor 与优雅停机 | `L4ProxyServerTest.cpp` |
| 真实进程链路 | `tests/integration/smoke.py` |

阅读每个测试时，先写下“初始状态—事件—预期状态—资源结果”，再去对照实现。

## 14. 面试时如何介绍项目

可以用下面的 90 秒版本：

> 我在一个 C++ Reactor 网络库的基础上实现了 Linux 用户态 L4 TCP 负载均衡器。架构采用主从 Reactor，主线程 accept 后将 fd 轮询投递给 worker，每个会话的前后端连接都归同一 EventLoop 所有，数据面无需加锁。我实现了 Round Robin 和 Least Connections，用 RAII BackendLease 管理活跃数，用不可变快照发布健康后端。ProxySession 负责非阻塞连接、有界重试、双向透明转发、高低水位背压和 TCP 半关闭。fd、定时器、线程和计数都使用 RAII 管理，所有异常路径收敛到幂等 Close。此外还有主动健康检查、metrics、异步结构化日志和 SIGTERM 优雅退出。项目在 Debug、Release、ASan/UBSan 和 TSan 下通过了故障与端到端测试。

面试时不要只列功能，要主动说出三个设计取舍：

1. 选择用户态 Buffer，放弃当前就上 `splice/io_uring`，优先保证生命周期正确。
2. 同一会话固定在一个 worker，用线程归属换取无锁数据面。
3. 健康变化只影响新连接，不中断已有会话，保护业务完整性。

## 15. 可继续深挖的问题

当你能讲清当前实现后，可以继续思考：

- 如何将全局原子 metrics 改为 per-worker 分片？
- 如何支持加权 RR 或 EWMA latency 调度？
- 如何设计配置热加载，又不影响已有会话？
- 如何限制全局连接数、单 IP 连接数和新建速率？
- 如何处理 EMFILE 时的 idle-fd 技巧？
- `splice` 如何与背压、半关闭和跨 fd 错误处理结合？
- 如何将 TCP 健康检查升级为可插拔的 L7 探测？
- 如何用一致性哈希增加会话亲和性？

这些问题没有唯一答案。面试官更关心你能否说清需求、不变式、失败路径和取舍。
