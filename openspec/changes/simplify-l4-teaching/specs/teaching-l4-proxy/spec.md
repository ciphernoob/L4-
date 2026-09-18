## Purpose

提供便于学习的最小 Linux IPv4 TCP 负载均衡服务，让读者沿客户端接入、轮询选择后端、建立连接、双向转发及资源释放理解完整业务，同时保留必要的传输正确性和有界缓存行为。

## ADDED Requirements

### Requirement: Connection-level round robin
服务 SHALL 使用启动时固定的非空后端列表按新 TCP 连接轮询选择，每条连接保持后端绑定，不根据应用层内容路由。

#### Scenario: Distribution and affinity
- **WHEN** 三个正常后端接收六条客户端连接，每条连接多次发送数据
- **THEN** 后端分布为 2/2/2，单条连接的所有数据始终发给同一后端

### Requirement: Transparent bounded forwarding
服务 SHALL 保持双向二进制字节的内容和顺序，连接期间不主动读取客户端，并在目标发送积压时暂停来源读取，排空后恢复。

#### Scenario: Early and slow traffic
- **WHEN** 客户端连接后立即发送大于单次读取上限的数据，目标先暂停读取再恢复
- **THEN** 数据完整到达，用户态待转发缓冲不会随暂停时间无限增长，恢复后继续传输

### Requirement: Half close and failure cleanup
服务 MUST 在 EOF 后发送完对应方向缓存再传播写半关闭，允许反方向继续发送；错误或三秒连接超时关闭整个会话，不自动重试。

#### Scenario: Response after request EOF
- **WHEN** 客户端发送请求并半关闭写方向
- **THEN** 后端收到完整请求及 EOF，客户端仍收到完整响应

#### Scenario: Failed backend
- **WHEN** 后端拒绝连接、连接超时或已连接后 reset
- **THEN** 会话两端被关闭，资源回收，代理继续处理其他连接

### Requirement: Minimal executable and learning material
默认程序 SHALL 使用源代码中的固定监听和后端配置，不依赖 jsoncpp，不启动 worker、管理端或日志线程；文档 MUST 描述教学版边界和完整旧版恢复方式。

#### Scenario: Build and run
- **WHEN** 在 Linux 上构建并启动默认目标
- **THEN** 单线程服务开放数据监听，SIGINT/SIGTERM 后关闭连接并退出，读者可按文档运行 echo 演示
