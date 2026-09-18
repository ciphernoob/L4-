## Why

当前完整版本的控制面和多线程协调掩盖了教学主线。用户已确认以简洁、可读的最小 L4 转发服务替换默认工程。

## What Changes

- **BREAKING**: 默认服务改为单线程 LT Reactor、轮询调度、固定 main 配置，不再接受 JSON 配置。
- 业务层仅保留 LoadBalancer、Connector、ProxySession；暂停前端读取直到后端连接完成。
- 采用固定读取批次和输出排空后恢复读取的简单背压，保留半关闭、部分写、连接超时和 RAII。
- 移除默认工程的健康检查、LC、重试、管理端、指标、异步日志、worker 和宽限期退出。
- 保存完整版本的可恢复快照，同步重写教学文档和基础测试。

## Capabilities

### New Capabilities
- `teaching-l4-proxy`: 教学版单线程 TCP 轮询代理；替代尚未归档的 build-l4-load-balancer 对默认工程的完整功能要求。

### Modified Capabilities
无已发布主规格；原变更作为历史设计保留。

## Impact

修改 l4lb 的源码、构建、测试和文档；不修改 code/dayNN。取消 jsoncpp 依赖，保留 Linux/C++14/CMake。旧功能只能在完整快照中恢复，不作为教学版验收要求。
