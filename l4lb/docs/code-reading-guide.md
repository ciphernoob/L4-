# L4 负载均衡器代码阅读指南

这份文档面向第一次阅读本项目的人。目标不是逐行解释所有代码，而是先建立正确的框架，再沿着一条 TCP 连接的生命周期阅读关键实现。

> 当前状态：网络基础层、调度算法、单会话代理、Multi-Reactor 服务组装、JSON 配置、管理监听和主动健康检查已经实现。单会话从 `tests/unit/ProxySessionTest.cpp` 开始读，整体进程从 `src/app/L4ProxyServer.cpp` 和 `src/app/main.cpp` 开始读。

## 1. 先理解它是什么

本项目是用户态四层 TCP full proxy。客户端不会直接连接后端，而是建立两条独立 TCP 连接：

```text
client                  l4lb                         backend
  |                       |                            |
  |---- frontend TCP ---->|                            |
  |                       |----- backend TCP --------->|
  |                       |                            |
  |<==== opaque bytes ===>|<===== opaque bytes ======>|
```

代理只处理字节流，不理解 HTTP 方法、URL、JSON 或其他应用层协议。因此 HTTP、Redis 协议或自定义 TCP 协议都只是“不透明字节”。

它不是 LVS：LVS 通常工作在内核网络路径中；本项目在用户态终止客户端 TCP 连接，再主动建立后端 TCP 连接。

## 2. 阅读时使用的四层心智模型

```text
app     进程启动、配置、数据/管理监听和优雅退出
  |
lb      后端模型、选择算法、Connector、ProxySession
  |
net     fd、Socket、Buffer、epoll、Reactor、TcpConnection
  |
Linux   socket / epoll / eventfd / timerfd / pthread
```

依赖只应由上向下：`lb` 可以使用 `net`，但 `net` 不应知道负载均衡算法或 `ProxySession`。

### 目录职责

| 目录 | 职责 | 代表文件 |
|---|---|---|
| `src/net` | 协议无关的网络基础设施 | `EventLoop`、`Channel`、`TcpConnection` |
| `src/lb` | 四层负载均衡领域逻辑 | `BackendPool`、`LoadBalancer`、`ProxySession` |
| `src/app` | 最终服务进程的组装入口 | `main.cpp`、`L4ProxyServer.cpp` |
| `config` | 配置示例 | `example.json` |
| `tests/unit` | 组件测试与当前端到端行为示例 | `ProxySessionTest.cpp` |

## 3. 推荐阅读顺序

如果目标是学习网络库，不要一开始就陷入 `main.cpp` 的组装细节；建议按以下六个阶段阅读，最后再读 `L4ProxyServer`。

### 阶段一：构建图和资源封装

1. `CMakeLists.txt`
2. `src/net/UniqueFd.h/.cpp`
3. `src/net/Socket.h/.cpp`
4. `src/net/InetAddress.h/.cpp`
5. `src/net/Buffer.h/.cpp`

先回答这些问题：

- 哪些类可以复制，哪些类只能移动？
- fd 在什么对象析构时关闭？
- `Socket::Release()` 与移动构造的用途有什么差别？
- `Buffer` 如何区分可读区、可写区和前置空间？

对应测试：`UniqueFdTest.cpp`、`SocketTest.cpp`、`BufferTest.cpp`。

### 阶段二：Reactor 主干

1. `src/net/Channel.h/.cpp`
2. `src/net/Epoller.h/.cpp`
3. `src/net/EventLoop.h/.cpp`
4. `tests/unit/EventLoopTest.cpp`

这三类的关系是：

```text
EventLoop
  owns Epoller
  owns wakeup eventfd + wakeup Channel
  polls ready Channel objects

Channel
  observes one fd
  stores interested events and callbacks
  does not own or close the fd

Epoller
  owns epoll fd
  translates Channel state into EPOLL_CTL_ADD/MOD/DEL
```

一次事件分发的调用链：

```text
EventLoop::Loop
  -> Epoller::Poll
  -> epoll_wait
  -> Channel::HandleEvent
  -> read/write/error/close callback
```

阅读重点是 `Channel::Tie()`。Channel 保存 `weak_ptr`，事件到达时先临时提升为强引用，避免 owner 已销毁后继续执行回调。

### 阶段三：定时器与线程

1. `TimerQueue` 和 `TimerId`
2. `EventLoopThread`
3. `EventLoopThreadPool`
4. 相应的测试文件

`TimerId` 是 move-only RAII 句柄。它的析构会取消定时器，因此下面的写法会立即取消任务：

```cpp
loop.RunAfter(std::chrono::seconds(1), callback); // 返回值立即析构
```

正确方式是持有句柄：

```cpp
TimerId timer = loop.RunAfter(std::chrono::seconds(1), callback);
```

`EventLoopThread` 的重要不变量是：线程对象析构前必须让事件循环退出并 `join()`，不能留下 detached 或 joinable 线程。

### 阶段四：TCP 连接抽象

1. `src/net/Acceptor.h/.cpp`
2. `src/net/TcpConnection.h/.cpp`
3. `tests/unit/TcpConnectionTest.cpp`

`Acceptor` 负责被动建立连接；`TcpConnection` 负责一个已经建立或正在建立的连接。

`TcpConnection` 拥有：

- 一个 RAII `Socket`；
- 一个不拥有 fd 的 `Channel`；
- 输入和输出 `Buffer`；
- 消息、写完成、水位、EOF、错误和关闭回调。

读路径：

```text
EPOLLIN
  -> TcpConnection::HandleRead
  -> read until EAGAIN / EOF / error
  -> append input_buffer
  -> message_callback
```

写路径：

```text
Send
  -> try send immediately
  -> remaining bytes enter output_buffer
  -> enable EPOLLOUT
  -> HandleWrite drains until EAGAIN
  -> disable EPOLLOUT when empty
```

需要特别区分：

- `StopRead()` 只是取消 EPOLLIN，不会关闭连接；
- `ShutdownWrite()` 是 TCP 写半关闭，并允许继续读取；
- `ForceClose()` 才是终止整个 `TcpConnection`。

### 阶段五：主动连接与调度

1. `src/lb/Connector.h/.cpp`
2. `src/lb/Backend.h/.cpp`
3. `src/lb/BackendPool.h/.cpp`
4. `src/lb/LoadBalancer.h/.cpp`
5. `ConnectorTest.cpp` 和 `LoadBalancerTest.cpp`

`Connector` 封装非阻塞 `connect()`：

```text
connect
  +-- success --------------------------> transfer Socket
  +-- EINPROGRESS -> wait EPOLLOUT
                        -> SO_ERROR == 0 -> transfer Socket
                        -> error         -> failure callback
  +-- timeout/cancel -------------------> cleanup
```

成功时，`Connector` 用移动语义把 `Socket` 交给 `TcpConnection`。同一个 fd 在任意时刻只有一个所有者。

调度层由三个概念组成：

- `Backend`：后端地址、健康状态、权重和活跃会话数；
- `BackendPool`：保存后端，并原子发布不可变健康候选快照；
- `LoadBalancer`：在快照上执行 Round Robin 或 Least Connections。

`BackendLease` 是调度层最关键的 RAII 对象。获得 lease 时活跃数加一，lease 析构时减一；移动后源对象失效，因此错误路径不需要手工回滚计数。

### 阶段六：完整代理会话

最后阅读：

1. `src/lb/SessionMap.h/.cpp`
2. `src/lb/ProxySession.h/.cpp`
3. `tests/unit/ProxySessionTest.cpp`

`SessionMap` 是会话的强引用根。`ProxySession` 强引用前端连接、正在连接的 `Connector` 和后端连接；这些对象的回调只弱引用会话，从而避免引用环。

```text
SessionMap
  |
  +-- shared_ptr<ProxySession>
          |
          +-- shared_ptr<TcpConnection> frontend
          +-- shared_ptr<Connector> while connecting
          +-- shared_ptr<TcpConnection> backend

callbacks: weak_ptr<ProxySession>
```

会话状态：

```text
Accepted -> Connecting -> Established -> Draining -> Closed
                 |                           |
                 +------ failure ---------->+
```

- `Accepted`：前端连接已接收，会话尚未启动；
- `Connecting`：正在非阻塞连接后端，客户端数据可以有界暂存；
- `Established`：前后端连接都存在，进行双向转发；
- `Draining`：至少一侧收到 EOF，继续排空缓存和反向流量；
- `Closed`：所有逻辑资源已经通过唯一关闭入口处理。

## 4. 跟踪一条连接的完整调用链

当前可执行的完整示例在 `ProxySessionForwardsLargeOpaqueStreamsAndPreservesHalfClose` 测试中。

### 4.1 创建前端连接

测试通过 `socketpair()` 模拟客户端与代理前端。代理一侧的 fd 被移动进 `TcpConnection`，然后调用 `Establish()` 注册读事件。

### 4.2 SessionMap 建立所有权根

`SessionMap::Add()` 分配 session id，创建 `ProxySession`，并保存强引用。删除回调捕获 SessionMap，使 `ProxySession::Close()` 能把自己从 map 中移除。

### 4.3 主动连接后端

`ProxySession::Start()` 安装前端回调、设置水位和输入上限，然后创建 `Connector`。连接成功后：

1. Connector 移交 `Socket`；
2. ProxySession 创建后端 `TcpConnection`；
3. 安装后端回调；
4. 建立后端连接；
5. 冲刷连接建立期间暂存的客户端字节。

### 4.4 双向转发

方向 A：客户端到后端。

```text
frontend input_buffer
  -> ProxySession::HandleFrontendData
  -> backend Send
  -> backend output_buffer / kernel send buffer
```

方向 B 完全对称，由 `HandleBackendData()` 把后端输入发送到前端。

这里没有解析协议，也不会按报文重新选择后端。一旦会话绑定某个后端，就一直使用该后端直到关闭。

## 5. 高低水位背压怎么读

假设后端读取很慢：

1. 前端不断收到客户端数据；
2. 数据进入后端连接的 `output_buffer`；
3. 达到高水位时，后端连接触发 high-watermark callback；
4. ProxySession 对前端调用 `StopRead()`；
5. 后端恢复读取后，EPOLLOUT 将缓存发送出去；
6. 缓存降到低水位时触发 low-watermark callback；
7. 先转交已经读入但尚未发送的数据，再对前端调用 `StartRead()`。

```text
backend output bytes

high  ----------- pause frontend EPOLLIN
        hysteresis region
low   ----------- resume frontend EPOLLIN
```

高、低两个阈值形成滞回区，防止缓冲大小在单一阈值附近变化时频繁修改 epoll interest。

项目还会计算目标输出缓冲剩余容量，每次只转交不超过该容量的字节。因此高水位不仅是通知阈值，也是用户态输出缓存的硬边界。

反方向的慢客户端背压逻辑完全对称。阅读 `ProxySessionAppliesAndReleasesBackpressureInBothDirections` 测试可以同时看到两种场景。

## 6. TCP 半关闭怎么读

TCP 是全双工流。一端发送 EOF，只说明它以后不再发送数据，不代表它也不接收数据。

客户端执行 `shutdown(SHUT_WR)` 后：

1. 前端 `TcpConnection` 读到 EOF；
2. ProxySession 标记 `frontend_eof_`；
3. 已缓存的请求继续发送给后端；
4. 后端输出缓存排空后执行 `shutdown(SHUT_WR)`；
5. 后端仍可以发送响应；
6. 响应继续转发给客户端；
7. 两个方向都 EOF 且输出缓存为空时，会话才进入 `Closed`。

不要把“收到 EOF”直接实现为“关闭整个会话”，否则客户端半关闭写端后将收不到后端响应。

## 7. RAII 所有权地图

| 资源 | 所有者 | 是否可复制 | 释放方式 |
|---|---|---:|---|
| 普通 fd | `UniqueFd` | 否 | 析构或 `Reset()` |
| socket fd | `Socket` 内的 `UniqueFd` | 否 | `Socket` 析构或 `Close()` |
| epoll fd | `Epoller` 内的 `UniqueFd` | 否 | `Epoller` 析构 |
| Channel | Acceptor/Connector/TcpConnection 等 | 否 | 先 `Remove()`，再析构 |
| timer 注册 | `TimerId` | 否 | `Cancel()` 或句柄析构 |
| 工作线程 | `EventLoopThread` | 否 | Quit 后 join |
| 后端活跃计数 | `BackendLease` | 否 | lease 析构自动减一 |
| 代理会话 | `SessionMap` | shared ownership | `ProxySession::Close()` 从 map 移除 |

必须牢记两个规则：

1. `Channel` 不拥有 fd，拥有者必须先把 Channel 从 EventLoop 移除，再关闭 fd；
2. RAII 不等于“可以在任意线程析构”。涉及 epoll 注册的逻辑关闭必须发生在所属 EventLoop 线程。

`ProxySession::Close()` 是唯一逻辑释放入口。它先把状态设为 Closed，因此由错误、关闭和重复事件造成的重入会直接返回；随后取消 Connector、关闭两端连接并从 SessionMap 移除。

## 8. 线程模型与并发边界

设计目标是 main Reactor 接收连接，再轮询投递给 worker Reactor。一个会话的以下对象必须属于同一个 worker EventLoop：

- 前端 `TcpConnection`；
- 后端 `Connector`；
- 后端 `TcpConnection`；
- `ProxySession` 的可变状态。

这样数据面状态只在一个线程访问，不需要为每个会话加锁。

允许跨线程的操作通过 `EventLoop::QueueInLoop()` 排队，并用 eventfd 唤醒 loop。后端健康快照和统计计数属于跨线程共享状态，使用原子操作或原子发布的不可变快照。

`L4ProxyServer` 已将主 Acceptor 与 worker 线程池组装：主 loop accept 后轮询选择 worker，把 Socket 移动投递到 worker，再在 worker 内创建 TcpConnection 和 ProxySession。

## 9. 如何通过测试学习，而不是只看实现

推荐采用“测试提出问题，实现给出答案”的方式：

| 想理解的行为 | 先读的测试 | 再读的实现 |
|---|---|---|
| fd 如何只关闭一次 | `UniqueFdTest.cpp` | `UniqueFd.cpp` |
| Channel 如何防悬空回调 | `EventLoopTest.cpp` | `Channel.cpp` |
| 定时器为什么要持有句柄 | `TimerQueueTest.cpp` | `TimerId.cpp`、`TimerQueue.cpp` |
| 非阻塞 connect 如何完成 | `ConnectorTest.cpp` | `Connector.cpp` |
| 两种调度算法 | `LoadBalancerTest.cpp` | `LoadBalancer.cpp` |
| 二进制双向转发与半关闭 | `ProxySessionTest.cpp` 第二个测试 | `ProxySession.cpp` |
| 双向背压 | `ProxySessionTest.cpp` 第三个测试 | `ProxySession.cpp`、`TcpConnection.cpp` |

构建和运行：

```bash
cmake -S l4lb -B l4lb/build-linux \
  -DCMAKE_BUILD_TYPE=Debug \
  -DL4LB_REQUIRE_JSONCPP=ON
cmake --build l4lb/build-linux -j2
ctest --test-dir l4lb/build-linux --output-on-failure
```

在本项目配置下所有单元测试位于同一个二进制中，也可以直接运行以看到每个测试名：

```bash
./l4lb/build-linux/l4lb_unit_tests
```

内存与未定义行为检查：

```bash
cmake -S l4lb -B l4lb/build-linux-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DL4LB_ENABLE_ASAN=ON \
  -DL4LB_ENABLE_UBSAN=ON
cmake --build l4lb/build-linux-asan -j2
ctest --test-dir l4lb/build-linux-asan --output-on-failure
```

TSan 必须使用独立构建，不能与 ASan/UBSan 同时开启。部分 WSL 环境的高地址随机化会让 TSan 在程序启动前报告 `unexpected memory mapping`；这种情况下可仅对测试进程使用：

```bash
setarch "$(uname -m)" -R ./l4lb/build-linux-tsan/l4lb_unit_tests
```

## 10. 建议设置的调试断点

第一次用 gdb 跟踪代理测试时，按这个顺序设置断点：

```gdb
break l4lb::lb::ProxySession::Start
break l4lb::lb::Connector::CompleteSuccess
break l4lb::lb::ProxySession::HandleConnected
break l4lb::lb::ProxySession::HandleFrontendData
break l4lb::net::TcpConnection::SendInLoop
break l4lb::lb::ProxySession::HandleFrontendEof
break l4lb::lb::ProxySession::Close
```

每次停下后观察：

- 当前线程是否是 `loop_` 所属线程；
- `ProxySession::state_`；
- 前后端 input/output buffer 大小；
- `frontend_eof_` 与 `backend_eof_`；
- 谁还持有当前对象的强引用。

## 11. 三组动手练习

### 练习一：画出一个 fd 的所有权转移

从 `Connector::Start()` 创建 socket 开始，跟踪到 `CompleteSuccess()`，再到 `ProxySession::HandleConnected()` 和 `TcpConnection` 构造。确认每一步是移动而不是复制，并记录失败路径由谁关闭 fd。

### 练习二：观察背压

在 `ProxySessionAppliesAndReleasesBackpressureInBothDirections` 中，将高低水位改成更小的值，打印：

- `OutputBufferBytes()`；
- `IsReading()`；
- 当前测试阶段。

观察高水位暂停和低水位恢复。练习结束后撤销临时打印。

### 练习三：故意破坏半关闭

临时把 `HandleFrontendEof()` 改成直接 `Close()`，运行代理测试，观察为什么响应丢失。然后恢复代码。这个实验能直观看出 EOF 与完整关闭的区别。

## 12. 常见误读与陷阱

- `shared_ptr` 不是“哪里都能安全销毁”；epoll 对象仍有线程归属。
- `Channel` 不是 fd owner，不能依赖其析构关闭 fd。
- ET 模式下不能只读一次，必须处理到 EAGAIN。
- EPOLLOUT 不应永久监听，只在存在待发送缓存时开启。
- `TimerId` 临时对象会立即取消定时器。
- TCP 的一次 `read()` 不对应一次应用层消息。
- 收到 EOF 不等于反方向也结束。
- 高水位负责暂停来源读取，低水位负责恢复；不能只用一个阈值。
- Least Connections 的计数生命周期由 `BackendLease` 管理，不应手工加减。
- `ProxySession` 既保留固定后端测试模式，也支持生产模式的调度、`BackendLease` 和有界重试。

## 13. 当前实现边界

已完成：

- Linux epoll Reactor、跨线程唤醒和定时器；
- RAII fd、Socket、线程、Timer 和 BackendLease；
- Acceptor、TcpConnection、非阻塞 Connector；
- BackendPool、健康候选快照、Round Robin、Least Connections；
- 接入调度器、BackendLease 和有界重试的 ProxySession；
- 连接前缓存、透明双向转发、背压、半关闭和空闲超时；
- Multi-Reactor 服务、JSON 配置、指标管理端、主动健康检查和宽限期退出；
- reset/EPOLLERR/HUP 异常销毁、多 worker 调度验收和 fd 计数检查；
- 结构化异步日志、真实进程端到端测试与可复现性能基准；
- Debug、Release、ASan/UBSan 和 TSan 验证。

当前是可运行的学习型 L4 服务，但仍不是生产级负载均衡器：它暂不支持 IPv6、UDP、TLS 终止、配置热加载、权重调度和内核旁路。

## 14. 面试时如何概括当前架构

可以用下面这段话作为基础，再结合代码细节展开：

> 这是一个基于 C++14、epoll 和 Reactor 的用户态 L4 TCP full proxy。网络层用 EventLoop、Channel 和 TcpConnection 封装非阻塞 I/O；一个 ProxySession 把客户端和后端连接固定在同一个 EventLoop，通过弱回调避免悬空访问，通过高低水位控制双向背压，并保留 TCP 半关闭语义。fd、Socket、定时器、线程和后端活跃计数都采用 move-only RAII 管理。后端池通过不可变健康快照支持 Round Robin 和 Least Connections，服务层已组装 Multi-Reactor、主动健康检查、指标、异步日志和优雅退出。

读完这份指南后，建议把 `ProxySessionTest.cpp` 中的正常转发、背压、重试和 reset 测试分别画成时序图。只要能够解释每次强引用、fd 所有权、EPOLLIN/EPOLLOUT 切换和状态变化，就已经掌握了当前项目的主体框架。
