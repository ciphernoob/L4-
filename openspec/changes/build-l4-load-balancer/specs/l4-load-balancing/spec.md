# Spec Delta

## Purpose

提供一个可配置、可观测且能够在后端故障和网络背压下保持行为确定的四层 TCP 负载均衡服务，并以透明字节流代理方式支持任意基于 TCP 的上层协议。

## ADDED Requirements

### Requirement: Validated startup configuration
系统 MUST 从 JSON 配置中读取监听地址、工作线程数、调度算法、后端列表、连接与空闲超时、缓冲区水位、健康检查阈值、管理端监听地址和退出宽限期。系统 MUST 在启动监听前拒绝端口无效、后端为空、水位关系无效、算法未知或数值超出有效范围的配置，并输出可定位字段的错误信息。

#### Scenario: Start with a valid configuration
- **WHEN** 操作者提供包含至少一个有效后端且所有字段合法的配置
- **THEN** 系统启动数据端和管理端监听，并记录生效的非敏感配置摘要

#### Scenario: Reject an invalid configuration
- **WHEN** 配置包含未知调度算法或低水位不小于高水位
- **THEN** 系统以非零状态退出，且不开放任何监听端口

### Requirement: Layer-four connection routing
系统 SHALL 对每个新客户端 TCP 连接从当前健康后端中选择一个节点，并建立独立的后端 TCP 连接。后端选择 MUST 以连接为粒度完成，系统 MUST NOT 解析、修改或基于应用层报文重新选择后端。

#### Scenario: Route a new TCP connection
- **WHEN** 客户端连接数据端端口且存在健康后端
- **THEN** 系统选择一个健康后端、建立后端连接，并在该客户端连接的整个生命周期内保持绑定

#### Scenario: No healthy backend is available
- **WHEN** 客户端连接到达时所有后端均处于不健康状态
- **THEN** 系统关闭该客户端连接、增加无可用后端计数，并保持服务进程可用

### Requirement: Supported balancing algorithms
系统 MUST 支持 Round Robin 和 Least Connections 两种可配置算法。Round Robin SHALL 在健康后端之间按确定顺序轮转；Least Connections SHALL 选择活跃代理会话数最少的健康后端，并以稳定规则处理并列节点。关闭会话后，相关后端的活跃会话数 MUST 被准确扣减。

#### Scenario: Round-robin distribution
- **WHEN** 三个健康后端具有相同权重且连续建立六个客户端连接
- **THEN** Round Robin 按后端配置顺序循环选择，每个后端被选择两次

#### Scenario: Least-connections distribution
- **WHEN** 一个健康后端的活跃会话数低于其他健康后端
- **THEN** Least Connections 为新连接选择该后端

#### Scenario: Ignore unhealthy backends
- **WHEN** 任一调度算法执行选择且部分后端不健康
- **THEN** 不健康后端不参与候选集合

### Requirement: Transparent bidirectional forwarding
后端连接建立后，系统 SHALL 按原始顺序双向转发客户端与后端之间的全部字节，不改变字节内容。后端连接建立期间收到的客户端数据 MUST 在配置的内存限制内保留，并在连接成功后按顺序发送。

#### Scenario: Forward arbitrary binary payloads
- **WHEN** 客户端与后端在同一会话中双向发送包含零字节的二进制数据
- **THEN** 双方接收的数据在内容、长度和顺序上均与发送数据一致

#### Scenario: Buffer data while connecting upstream
- **WHEN** 客户端在后端非阻塞连接完成前发送数据且未超过高水位
- **THEN** 数据在后端连接成功后完整且按序转发

### Requirement: Bounded buffering and backpressure
系统 MUST 为每个转发方向应用可配置的高、低水位。当目标方向待发送数据达到高水位时，系统 MUST 暂停读取来源连接；当积压下降到低水位或以下时，系统 MUST 恢复读取。单个会话的用户态待发送数据 MUST NOT 无限制增长。

#### Scenario: Slow backend applies backpressure
- **WHEN** 后端停止读取且客户端持续发送数据，使后端方向缓冲达到高水位
- **THEN** 系统暂停读取客户端，且该方向缓冲不会因继续读取而越过配置上限

#### Scenario: Reading resumes after drain
- **WHEN** 已暂停的目标方向缓冲被发送至低水位或以下
- **THEN** 系统恢复读取来源连接并继续透明转发

### Requirement: TCP half-close and terminal error handling
系统 SHALL 保留 TCP 半关闭语义。一侧收到有序 EOF 后，系统 MUST 先发送完该方向已缓存的数据，再关闭另一侧对应的写方向，同时允许反方向继续传输。发生不可恢复的连接错误或两侧传输均结束后，系统 MUST 幂等地释放整个会话及其资源。

#### Scenario: Client half-closes its write side
- **WHEN** 客户端发送完请求后执行写半关闭且后端仍会返回响应
- **THEN** 系统将请求全部转发并向后端传播写半关闭，同时继续把后端响应转发给客户端

#### Scenario: Peer resets a connection
- **WHEN** 任一端发生连接重置或不可恢复的套接字错误
- **THEN** 系统关闭会话两端、释放缓冲和计数，且重复到达的关闭事件不会造成崩溃或重复扣减

### Requirement: Upstream connection retry and timeouts
系统 MUST 对后端连接建立和已建立会话应用可配置超时。后端连接失败或超时时，系统 SHALL 在尚未向后端成功转发数据且仍有未尝试健康节点时，最多按照配置的重试次数选择其他节点；超过重试次数后 MUST 关闭客户端会话。空闲时间达到配置阈值的会话 MUST 被关闭。

#### Scenario: Retry an alternate backend
- **WHEN** 首选后端连接失败、配置允许一次重试且存在另一未尝试健康后端
- **THEN** 系统连接该备选后端，并在成功后转发暂存的客户端数据

#### Scenario: Close an idle session
- **WHEN** 会话两个方向在空闲超时时间内均无数据活动
- **THEN** 系统关闭两端连接并记录空闲超时原因

### Requirement: Active backend health checking
系统 SHALL 定期对每个后端执行带超时的 TCP 连接探测。后端连续失败达到失败阈值后 MUST 标记为不健康；不健康后端连续成功达到恢复阈值后 MUST 恢复为健康。健康状态改变只影响新连接，现有会话 MUST NOT 因健康检查状态改变而被强制断开。

#### Scenario: Remove a failing backend
- **WHEN** 后端连续探测失败次数达到配置的失败阈值
- **THEN** 后端状态变为不健康，后续新连接不再选择该节点

#### Scenario: Restore a recovered backend
- **WHEN** 不健康后端连续探测成功次数达到恢复阈值
- **THEN** 后端状态变为健康并重新参与新连接调度

### Requirement: Runtime observability
系统 MUST 通过独立管理监听地址提供只读指标，至少包括当前和累计会话数、各后端健康状态与活跃会话数、双向字节数、后端连接失败数、无可用后端数、超时数和缓冲区背压触发数。指标读取 MUST NOT 解析或阻塞数据面流量。

#### Scenario: Query metrics during traffic
- **WHEN** 操作者在存在活跃代理会话时查询管理指标端点
- **THEN** 系统返回当前进程和各后端指标，且数据面连接继续转发

#### Scenario: Metrics reflect session completion
- **WHEN** 一个代理会话正常结束
- **THEN** 当前会话数减少、累计会话数保持递增且传输字节累计值不回退

### Requirement: Graceful process shutdown
系统 MUST 在收到受支持的终止信号后停止接受新连接，并允许现有会话在配置的宽限期内完成。宽限期结束后，系统 MUST 关闭剩余会话、刷新日志并以确定状态退出。

#### Scenario: Existing session drains during shutdown
- **WHEN** 系统收到终止信号且现有会话在宽限期内完成
- **THEN** 系统不再接受新连接，等待该会话结束后正常退出

#### Scenario: Shutdown grace period expires
- **WHEN** 宽限期结束时仍存在未完成会话
- **THEN** 系统关闭剩余会话、释放资源并退出，而不会无限等待

### Requirement: Linux TCP compatibility
首版系统 MUST 在 Linux 上支持 IPv4 TCP 字节流，并能够透明代理 HTTP、echo 或其他不依赖源地址透传的 TCP 应用。系统 MUST 明确报告配置中不受支持的 UDP、IPv6 或应用层路由选项。

#### Scenario: Proxy an HTTP exchange without HTTP awareness
- **WHEN** HTTP 客户端通过数据端端口连接到 HTTP 后端
- **THEN** 请求与响应能够完成，且调度和转发行为不依赖 HTTP 方法、路径或头部

