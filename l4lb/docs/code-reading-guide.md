# 教学版 L4：从流量转发读代码

本文描述当前单线程教学版。完整版本资料在 [legacy](../legacy/README.md)。

## 1. 先记住三个业务类

| 类 | 职责 |
|---|---|
| [LoadBalancer](../src/lb/LoadBalancer.cpp) | accept 后轮询选一个地址，创建并保存会话 |
| [Connector](../src/lb/Connector.cpp) | 连接给定后端，报告成功/失败/超时 |
| [ProxySession](../src/lb/ProxySession.cpp) | 保存两端连接，转发字节，结束会话 |

底层 TcpConnection 不知道负载均衡算法；它只知道自己的 socket、输入/输出 Buffer 和回调。

## 2. 从入口开始

打开 [main.cpp](../src/app/main.cpp)，先看三个步骤：

```cpp
EventLoop loop;
LoadBalancer server(&loop, listen, backends);
server.Start();
loop.Loop();
```

实际 main 还包含 signalfd，用于 Ctrl-C 后安全退出。第一遍可以跳过信号相关代码。

这里只有一个线程：客户端和后端事件均由同一个 EventLoop 处理。没有 worker 分配，也没有跨线程转发。

## 3. 新连接如何选择后端

在 LoadBalancer::OnNewConnection 中：

1. 根据 next_backend_ 取地址。
2. 下标递增并对后端数量取模。
3. 创建 ProxySession，移交客户端 Socket。
4. sessions_ map 持有会话。
5. 调用 session->Start()。

先放入 map 再 Start，是因为 connect 可能立即失败并触发 Close。map 必须已经有对应条目。

## 4. 后端连接前为什么不读客户端

ProxySession::Start 安装客户端回调，调用：

```cpp
client_->Establish(false);
connector_->Start();
```

false 表示连接对象已经就绪，但尚不监听读事件。客户端早到的数据保留在代理的内核接收缓冲区。

Connector 成功时调用 OnBackendConnected，创建 backend TcpConnection、安装回调，然后开启双方读取。不需要单独的用户态“连接前缓存”。

Connector 的三条路径是：

- connect 返回 0：立即成功。
- 返回 EINPROGRESS：等 EPOLLOUT，再检查 SO_ERROR。
- 失败或三秒超时：关闭本次会话，不换其他后端重试。

## 5. 客户端到后端的主线

```text
client sends bytes
  -> kernel receive buffer
  -> epoll readable event
  -> TcpConnection::HandleRead (at most 16 KiB)
  -> client input Buffer
  -> ProxySession::OnClientMessage
  -> backend_->Send
  -> send immediately, or save remainder and watch EPOLLOUT
```

OnClientMessage 的三个关键动作：

```cpp
backend_->Send(input->Peek(), input->ReadableBytes());
input->RetrieveAll();
if (backend_->OutputBufferBytes() != 0) {
    client_->StopRead();
}
```

实际函数还会检查 closed_，因为 Send 内部可能因写错误关闭会话。

OnBackendMessage 完全对称。请把两个函数放在一起比较，看清“来源读缓冲”和“目标写缓冲”。

## 6. 写不完时怎么办

Send 优先直接调用系统 send。若只写了一部分或返回 EAGAIN，剩余字节进入 output_ 并开启 EPOLLOUT。

HandleWrite 继续发送，缓冲全部排空时关闭 EPOLLOUT，然后调用写完成回调：

- backend 写完：恢复 client 读取。
- client 写完：恢复 backend 读取。

这是简单的逐批背压。不要把旧版本的高低水位逻辑套到这里；当前只有“有积压就停读，排空再恢复”。

TcpConnection 使用 LT，因此每次只读一批是允许的：内核中仍有数据时，下次 epoll 会再次通知。

## 7. EOF 和 Close 分开理解

EOF 表示对端不再发送，但仍可接收反向数据。

- client EOF：backend 调用 ShutdownWrite，输出排空后才向后端发送 FIN。
- backend EOF：client 同样处理。
- 双方 EOF 且输出都为空：Close。
- reset、连接失败、写错误：直接 Close。

ShutdownWrite 只结束写方向；ForceClose 才移除 Channel 并关闭 fd。不要把两者混淆。

## 8. 谁持有谁

```text
LoadBalancer
  -> sessions_ map
      -> shared_ptr<ProxySession>
          -> client TcpConnection
          -> backend TcpConnection
          -> Connector (only while connecting)

connection callbacks --weak_ptr--> ProxySession
```

Close 先设置 closed_，再取消 Connector、关闭两个连接，最后通过回调从 map 删除会话。重复 Close 立即返回。

还有一个细节：epoll_wait 一次可能返回两端 Channel。处理第一端时关闭整条会话，第二端的 Channel 指针仍在当前事件数组中。因此对象需要保留到当前批次结束：

```cpp
loop_->QueueInLoop([self] {});
```

这行不是转发业务，只是在 pending functor 执行完时释放临时强引用，保证当前批次不会访问悬空对象。

## 9. 看哪些测试

- [ProxySessionTest.cpp](../tests/unit/ProxySessionTest.cpp)：双向各 2 MiB 数据、先停读后恢复、16 KiB 输出上限、双向 EOF、RST。
- [LoadBalancerTest.cpp](../tests/unit/LoadBalancerTest.cpp)：空后端列表拒绝、连接失败后会话清零。
- [ConnectorTest.cpp](../tests/unit/ConnectorTest.cpp)：成功、拒绝、取消、超时；用本机满 backlog 制造超时。
- [smoke.py](../tests/integration/smoke.py)：三个后端六条连接 2/2/2、同连接多次消息、HTTP、二进制、并发短连接、fd 检查和 SIGTERM。

推荐断点：OnNewConnection、OnBackendConnected、OnClientMessage、HandleWrite、OnClientEof、Close。

完成阅读后，尝试回答：这一批字节现在在哪个 Buffer？目标能不能写？来源是否在读？谁持有这条会话？这四个问题足以串起主线。
