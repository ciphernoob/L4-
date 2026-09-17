# Tasks

## 1. 工程骨架与验证基线

- [x] 1.1 创建 `l4lb/` 独立 CMake 工程及 `base`、`net`、`lb`、`config`、`app`、`tests` 目录，验证在 Linux 上执行 `cmake -S l4lb -B l4lb/build && cmake --build l4lb/build` 成功且不修改 `code/dayNN`。
- [x] 1.2 配置 C++14、`-Wall -Wextra -Werror`、pthread 和 jsoncpp 目标依赖，验证干净构建不会依赖历史 day 目录中的源文件或生成物。
- [x] 1.3 建立 CTest 单元测试入口以及可选 ASan/UBSan 构建开关，验证一个最小测试能被 `ctest --test-dir l4lb/build --output-on-failure` 和 sanitizer 构建运行。
- [x] 1.4 编写 `l4lb/README.md` 初始说明，明确用户态 full-proxy、Linux/IPv4/TCP 范围和非 LVS 定位，验证文档包含构建、运行和测试命令。

## 2. 协议无关网络基础层

- [x] 2.1 从 day29 提炼并修正 `Buffer`，使用安全索引并支持恰好消费全部数据，验证扩容、压缩、部分消费、全部消费和二进制零字节单元测试通过。
- [x] 2.2 实现 move-only `UniqueFd`、`InetAddress` 与 RAII `Socket`，封装 nonblocking、close-on-exec、reuse-address、bind、listen、accept、shutdown-write 和 socket error 查询，验证地址转换、移动移交、`release/reset`、作用域退出和失败路径中的 fd 恰好释放一次。
- [x] 2.3 重构非 fd 所有者 `Channel` 的读、写、关闭、错误回调及 Enable/Disable/Remove 操作，并修复 weak guard 失效行为，验证事件位切换、失效 owner 不执行回调，以及 Channel 在 Socket 关闭前从所属 EventLoop 移除。
- [x] 2.4 重构 `Epoller`，覆盖 ADD、MOD、DEL、EINTR 和错误返回，验证使用 eventfd 触发的注册、修改与删除测试通过。
- [x] 2.5 重构 `EventLoop`，增加 loop-thread 断言、RunInLoop、QueueInLoop、eventfd 唤醒和 Quit，验证跨线程排队任务会被唤醒执行且循环能够确定退出。
- [x] 2.6 修正 timerfd/TimerQueue 的一次性、重复、取消和销毁行为，提供 move-only、取消幂等的定时器句柄，验证连接超时与空闲超时所需的确定性定时器及作用域退出自动取消测试通过。
- [x] 2.7 重构 `Acceptor` 使其在 ET/LT 场景均 accept 到 EAGAIN，并处理 EMFILE 等错误，验证突发建立多连接时不会在 backlog 中遗留可接受连接。
- [x] 2.8 实现与 HTTP 解耦的 `TcpConnection`，提供消息、写完成、高水位、EOF、错误和关闭回调，验证源文件不依赖 `HttpContext` 且 echo 单元/集成测试通过。
- [x] 2.9 实现 `EventLoopThread` 与线程池的安全启动、轮询分配、停止和 RAII join，验证重复启动被拒绝、所有任务在所属线程执行，且正常退出和提前返回后均无 joinable/detached 悬挂线程。

## 3. 主动连接能力

- [x] 3.1 实现非阻塞 `Connector` 的立即成功、EINPROGRESS、EPOLLOUT 和 `SO_ERROR` 状态处理，验证成功连接与 connection-refused 测试均返回准确结果。
- [x] 3.2 为 Connector 增加可取消的连接超时和幂等清理，成功时以移动语义移交 Socket，验证连接超时、用户取消和事件同时到达时 fd/Channel 仅释放一次且不存在悬空所有权。
- [x] 3.3 验证 Connector 成功产生的后端 `TcpConnection` 与调用方处于同一个 EventLoop，使用线程断言测试确保不存在跨线程 Channel 注册。

## 4. 后端模型与调度策略

- [x] 4.1 实现包含稳定 id、IPv4 地址、配置权重、健康状态和原子活跃会话数的 Backend/BackendPool，验证重复 id、无效地址和无效权重会被拒绝。
- [x] 4.2 实现不可变健康候选快照的原子发布与读取，验证并发更新健康状态和执行选择时 ThreadSanitizer 不报告数据竞争。
- [x] 4.3 实现 Round Robin，仅在健康后端间按配置顺序轮转，验证三节点六连接分布为 2/2/2 且不健康节点不被选择。
- [x] 4.4 实现 Least Connections 及按配置顺序稳定打破平局，并以 move-only RAII `BackendLease` 管理活跃计数，验证移动、正常销毁和各种错误路径均不会漏减或重复扣减。
- [x] 4.5 实现无健康后端的显式选择失败，验证调用方获得可识别错误并更新 `no_available_backend` 指标。

## 5. 单线程四层代理会话

- [x] 5.1 实现 worker 所有的 SessionMap 和 ProxySession 状态机，先接入单个固定后端，验证 Accepted、Connecting、Established、Draining、Closed 转换及重复关闭测试通过。
- [x] 5.2 实现后端连接前的客户端数据暂存和连接成功后的按序发送，验证延迟建立后端时收到的数据不丢失、不重复且不越过配置上限。
- [x] 5.3 实现前端到后端、后端到前端的透明字节流转发，验证包含零字节和大于单次 read buffer 的双向 payload 哈希完全一致。
- [x] 5.4 接入调度器并实现有界后端连接重试，验证首选节点失败时只尝试未使用的健康节点、成功转发后不再切换后端、超过上限关闭客户端。
- [x] 5.5 实现双向高低水位背压，验证慢后端和慢客户端场景会暂停来源 EPOLLIN、缓冲保持有界并在下降至低水位后恢复。
- [x] 5.6 实现双向 EOF、缓存排空和 `shutdown(SHUT_WR)` 半关闭传播，验证客户端写半关闭后仍能完整接收后端响应。
- [x] 5.7 实现 reset、EPOLLERR/HUP、写错误与重复事件下的幂等会话销毁，以 `ProxySession::Close()` 作为唯一逻辑释放入口，验证最后一个强引用在所属 EventLoop 释放，且两端 fd、Channel、定时器、缓冲和 backend lease 均只释放一次。
- [x] 5.8 实现连接建立超时和双向空闲超时，验证超时会关闭会话、记录原因并增加对应指标，而活跃传输不会被误关闭。

## 6. Multi-Reactor 与健康检查

- [x] 6.1 实现主 Acceptor 将新 fd 投递到轮询 worker 后再创建连接，验证并发连接均在各自 worker 内注册且同一 ProxySession 的前后端始终属于同一 EventLoop。
- [x] 6.2 在多 worker 下接入 Round Robin 和 Least Connections，验证并发建立/关闭连接后分布满足算法预期且所有后端活跃计数最终归零。
- [x] 6.3 实现控制 EventLoop 中带超时的周期 TCP 健康探测，验证达到连续失败阈值后摘除节点、达到连续成功阈值后恢复节点。
- [x] 6.4 将健康状态转换发布为新快照且不触碰已有会话，验证节点变为 DOWN 时既有长连接继续传输而新连接避开该节点。

## 7. 配置、可观测性与进程生命周期

- [x] 7.1 实现 JSON 配置数据结构、全量解析和字段级校验，覆盖监听、线程、算法、后端、超时、水位、健康检查、重试和退出宽限期，验证有效示例可加载且每类无效输入在监听前失败。
- [x] 7.2 组装 `L4ProxyServer` 和 CLI 入口，支持指定配置路径并输出非敏感启动摘要，验证有效配置同时开放数据端与管理端、无效配置以非零状态退出且不监听端口。
- [x] 7.3 实现原子 MetricsRegistry，记录当前/累计会话、各后端状态与连接数、双向字节、连接失败、无可用后端、超时和背压次数，验证生命周期和流量测试中的计数单调性与归零规则。
- [x] 7.4 实现默认绑定 loopback 的只读管理 listener、`GET /metrics` 和健康响应，并限制请求大小和空闲时间，验证查询返回完整指标且未知方法/路径不会影响数据面。
- [x] 7.5 接入结构化异步日志，统一记录 session id、backend id、状态变化和关闭原因，验证高并发测试中日志线程无数据竞争且退出前完成 flush。
- [x] 7.6 实现基于信号通知的 draining 与宽限期退出，验证 SIGTERM 后拒绝新连接、允许已有连接完成，并在宽限期到达后关闭剩余连接和 join 全部线程。

## 8. 端到端质量与秋招交付

- [x] 8.1 建立可控 echo/HTTP 测试后端与集成测试编排，验证单命令可运行正常转发、二进制完整性、Round Robin 和 Least Connections 场景。
- [x] 8.2 增加后端拒绝连接、探测失败/恢复、连接 reset、客户端与后端半关闭、空闲超时的故障矩阵，验证所有场景无崩溃、fd 泄漏或计数泄漏。
- [x] 8.3 增加持续慢读和大流量背压测试，验证单会话缓冲不超过约定上限，并在恢复读取后完成剩余数据传输。
- [x] 8.4 在 Debug、Release、ASan/UBSan 和可用时的 TSan 配置下运行完整测试，并检查进程前后 fd 数量，验证 CTest 全部通过、sanitizer 无报告且正常/失败/超时/取消路径无资源泄漏。
- [x] 8.5 编写可复现基准脚本，分别测量直连和代理的吞吐、P50/P99、CPU、RSS 与错误率，验证固定环境下可生成带命令、参数和原始结果的报告。
- [x] 8.6 完善根 README 与 `l4lb/README.md` 的架构图、配置示例、运行演示、算法说明、测试方法、性能结果、限制和故障处理，验证全新 Linux 环境可按文档完成构建与演示。
