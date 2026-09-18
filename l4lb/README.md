# 教学版 L4 TCP 负载均衡器

当前默认版本专注于最基本的业务：**接收连接 → 轮询选后端 → 非阻塞连接 → 双向转发 → 释放会话**。

使用 C++14、Linux epoll 和 RAII；只有一个 EventLoop、一个服务线程。它是用户态 TCP full proxy，客户端与后端分别建立一条 TCP 连接，数据面不解析 HTTP。

## 构建

在 Ubuntu/WSL2 中，从仓库根目录执行：

```sh
sudo apt install build-essential cmake python3
cmake -S l4lb -B l4lb/build-teaching -DCMAKE_BUILD_TYPE=Debug
cmake --build l4lb/build-teaching -j2
ctest --test-dir l4lb/build-teaching --output-on-failure
```

不再需要 jsoncpp。请使用新的 build-teaching 目录，避免误运行旧 build-linux 中的完整版本二进制。

## 运行演示

三个终端分别启动 echo 后端：

```sh
python3 l4lb/examples/echo_backend.py 9101
python3 l4lb/examples/echo_backend.py 9102
python3 l4lb/examples/echo_backend.py 9103
```

另一个终端启动代理：

```sh
./l4lb/build-teaching/l4lb_server
```

测试三次新连接，观察三个后端终端各接收一次：

```sh
python3 -c 'import socket; s=socket.create_connection(("127.0.0.1",9000)); s.sendall(b"hello"); s.shutdown(socket.SHUT_WR); print(s.recv(1024)); s.close()'
```

监听地址和后端列表直接写在 [main.cpp](src/app/main.cpp)，默认监听 127.0.0.1:9000，后端是 9101、9102、9103。修改后重新编译。程序不接受 JSON 参数。

Ctrl-C/SIGTERM 会停止循环并关闭剩余连接，没有等待会话完成的宽限期。

## 从业务逻辑读代码

1. [main.cpp](src/app/main.cpp)：定义地址并启动服务。
2. [LoadBalancer.cpp](src/lb/LoadBalancer.cpp)：接入、轮询、保存会话。
3. [ProxySession.cpp](src/lb/ProxySession.cpp)：连接后端、两个方向的转发、半关闭。
4. [Connector.cpp](src/lb/Connector.cpp)：非阻塞 connect 和三秒超时。
5. [TcpConnection.cpp](src/net/TcpConnection.cpp)：读取一批、部分写、排空通知。
6. 最后阅读 EventLoop、Channel、Epoller、Socket、Buffer。

详细材料：

- [代码阅读指南](docs/code-reading-guide.md)
- [架构与面试问答](docs/architecture-business-interview-guide.md)

## 为了简洁做出的选择

- **单线程 LT Reactor**：可以并发维护多条连接，但不会并行执行多个业务回调。
- **轮询**：每条新 TCP 连接选一次后端，不做逐消息调度。
- **连接前不读客户端**：早到数据暂存在内核 TCP 接收缓冲区。
- **逐批背压**：每次读取最多 16 KiB，目标输出未排空时暂停来源，排空后恢复。每方向用户态待发送数据最多一个读取批次；这不等于整个进程或内核缓冲只占 16 KiB。
- **保留 TCP 正确性**：部分写、EAGAIN、双向半关闭、reset 和幂等清理。
- **无自动故障转移**：选中的后端失败或三秒连接超时就关闭当前客户端；后续连接继续轮询。
- **无空闲超时**：空闲长连接保留至 EOF、错误或服务停止。
- **RAII**：Socket 管 fd，TimerId 管连接定时器；map 持有会话，回调弱引用会话。

本版不包含多 worker、Least Connections、健康检查、重试、管理端、指标、异步日志、JSON、权重和宽限期退出。旧版本的性能数字不适用于教学版。

## 验证

CTest 包含网络基础单元测试和真实进程测试，后者使用 9000、9101、9102、9103 端口，请先停止手工演示进程。

```sh
cmake -S l4lb -B l4lb/build-teaching-asan -DCMAKE_BUILD_TYPE=Debug \
  -DL4LB_ENABLE_ASAN=ON -DL4LB_ENABLE_UBSAN=ON
cmake --build l4lb/build-teaching-asan -j2
ctest --test-dir l4lb/build-teaching-asan --output-on-failure
```

## 完整旧版本

精简前的源码、文档和测试保存在 [legacy/full-version.tar.gz](legacy/full-version.tar.gz)，恢复方法见 [legacy/README.md](legacy/README.md)。默认工程只编译教学版；历史 code/dayNN 教程未改动。
