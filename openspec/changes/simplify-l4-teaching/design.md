## Context

参见 proposal.md。现有网络资源封装可复用，复杂度主要在 app/lb 的控制面和并发协调。教学版直接替换默认工程。

## Goals / Non-Goals

Goals: main -> LoadBalancer -> ProxySession -> TcpConnection 调用链直接可读，保留正确的非阻塞 TCP 语义。
Non-Goals: 多 worker、LC、健康检查、JSON、metrics、异步日志、重试和宽限期停机。

## Decisions

- 使用单线程 LT epoll；复用底层 RAII Socket、Buffer、EventLoop 和连接超时定时器。
- LoadBalancer 直接保存地址数组、轮询下标和会话 map，去掉策略接口及 SessionMap 包装。
- ProxySession 在后端连接成功前不读取客户端，早到数据保留在内核。Connector 连接超时固定为三秒。
- TcpConnection 每次 LT 读事件最多读取 16 KiB，立即调用消息回调。目标有剩余 output 时暂停来源，排空时恢复；单方向用户态积压至多一个批次。
- 保留 EOF 标记、延迟 shutdown-write、Closed 标记和弱回调。会话关闭后保留到当前 epoll 批次结束再释放，避免同一批次另一 Channel 的裸指针悬空。
- main 中列出固定地址；SIGINT/SIGTERM 通过 signalfd 安全退出并立即关闭剩余会话，没有宽限期。
- 先将干净工作区的完整版本保存成 Git 源码归档，移除无关源码/测试并重写文档。历史功能和性能数据只属于归档。

## Risks / Trade-offs

- 单线程和逐批背压降低性能上限 -> 明确学习定位，不沿用旧性能结论。
- 后端失败不重试、不摘除 -> 当前客户端断开，后续连接继续按配置轮询。
- 不再提供空闲超时 -> 空闲连接持续存在至 EOF、错误或服务停止，文档明确。

## Migration Plan

保存 l4lb 完整源码归档及提交号；修改默认源码、CMake、测试和文档；运行 Debug、Release 与 ASan/UBSan。需完整版本时解压至另一目录，不覆盖教学版。
