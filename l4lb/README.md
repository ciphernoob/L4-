# L4 TCP 负载均衡器

这是一个面向学习与秋招展示的 C++14 用户态 TCP full-proxy。它运行于 Linux，使用 IPv4/TCP，不解析数据面协议，也不是 LVS、NAT、DR 或内核旁路方案。

## 架构与数据流

```text
                         control EventLoop
                    +-------------------------+
client --> Acceptor | health checks / admin   | --> /healthz, /metrics
                    +------------+------------+
                                 |
                       round-robin dispatch
             +-------------------+-------------------+
             v                   v                   v
       worker EventLoop 0  worker EventLoop 1  worker EventLoop N
       SessionMap          SessionMap          SessionMap
       ProxySession        ProxySession        ProxySession
       frontend <------ opaque bytes ------> backend
```

主 Reactor 接收连接后把 fd 轮询投递给 worker；前后端 `TcpConnection` 与 `ProxySession` 始终归同一 worker 所有，因此数据面不需要跨线程操作 Channel。`ProxySession` 通过 Round Robin 或 Least Connections 选择健康后端，连接失败时仅重试尚未尝试的节点。双向高低水位背压限制用户态缓存，EOF 会在缓存排空后传播为 `shutdown(SHUT_WR)`。

资源使用 RAII 管理：`UniqueFd`/`Socket` 管理 fd，`TimerId` 管理定时器，`BackendLease` 管理活跃连接计数，`EventLoopThread` 析构时 join。`ProxySession::Close()` 是会话的唯一逻辑释放入口，可重复调用。

学习文档：

- [代码阅读与学习指南](docs/code-reading-guide.md)：偏重文件顺序与调用链。
- [架构、业务逻辑与面试指南](docs/architecture-business-interview-guide.md)：偏重系统设计、技术取舍与模块问答。

## 构建与测试

Ubuntu/WSL2 安装依赖：

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config libjsoncpp-dev python3
```

构建并运行全部测试：

```sh
cmake -S l4lb -B l4lb/build -DCMAKE_BUILD_TYPE=Debug -DL4LB_REQUIRE_JSONCPP=ON
cmake --build l4lb/build -j
ctest --test-dir l4lb/build --output-on-failure
```

CTest 会运行单元/集成测试以及真实进程 smoke test，覆盖二进制和 HTTP 字节流、两种调度算法、背压、半关闭、reset、超时、健康摘除/恢复、管理端和 SIGTERM 优雅退出。

Sanitizer：

```sh
cmake -S l4lb -B l4lb/build-asan -DL4LB_REQUIRE_JSONCPP=ON \
  -DL4LB_ENABLE_ASAN=ON -DL4LB_ENABLE_UBSAN=ON
cmake --build l4lb/build-asan -j
ctest --test-dir l4lb/build-asan --output-on-failure

cmake -S l4lb -B l4lb/build-tsan -DL4LB_REQUIRE_JSONCPP=ON \
  -DL4LB_ENABLE_TSAN=ON
cmake --build l4lb/build-tsan -j
ctest --test-dir l4lb/build-tsan --output-on-failure
```

## 配置与运行

先在 `127.0.0.1:9101` 启动 TCP 后端，再运行：

```sh
./l4lb/build/l4lb_server l4lb/config/example.json
```

完整字段见 [示例配置](config/example.json)，包括数据/管理监听地址、worker 数、算法、后端、连接与空闲超时、水位、健康检查、重试次数和退出宽限期。

```sh
curl http://127.0.0.1:9001/healthz
curl http://127.0.0.1:9001/metrics
```

服务输出逐行 JSON 会话日志，包含 `session_id`、`backend_id`、`state` 和 `close_reason`。按 `Ctrl-C` 或发送 SIGTERM 后停止接收新连接，已有连接可在宽限期内排空，随后所有线程 join 并完成日志 flush。

## 调度算法

- `round_robin`：在当前健康快照中按配置顺序轮转，适合后端能力接近的场景。
- `least_connections`：选择活跃会话最少的健康后端，并按配置顺序稳定打破平局；计数由 `BackendLease` 自动归还。

健康状态采用不可变快照发布。节点 DOWN 后新连接会避开它，已有连接不迁移、不被健康检查打断。

## 性能基准

```sh
python3 l4lb/bench/benchmark.py \
  --server l4lb/build/l4lb_server \
  --config l4lb/config/example.json \
  --requests 1000 --concurrency 32 --payload-bytes 16384 \
  --output l4lb/bench/report.json
```

脚本记录吞吐、P50/P99、错误率以及代理 CPU/RSS。仓库中的 [最近一次原始结果](bench/latest.json) 来自 WSL2 loopback、500 请求、并发 16、16 KiB payload：直连 60.58 MiB/s，代理 58.69 MiB/s，两者错误率均为 0。该结果只用于复现实验流程，不代表生产容量。

## 限制与排障

- 当前仅支持 Linux epoll、IPv4 和 TCP；没有 TLS 终止、配置热加载、权重调度、UDP、splice 或 io_uring。
- 这是用户态双连接代理，每条客户端连接对应一条后端连接；容量受 fd 上限、内存水位和 CPU 限制。
- 启动即失败通常是 JSON 字段错误、端口占用或 jsoncpp 缺失；错误会在监听前返回非零状态。
- 后端不可用时先查询 `/metrics` 中的健康状态、连接失败和超时计数，再检查后端地址、防火墙与探测阈值。
- TSan 在部分 WSL 地址布局下需用 `setarch "$(uname -m)" -R` 运行测试程序。
