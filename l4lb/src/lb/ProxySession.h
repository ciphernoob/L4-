#pragma once

#include "net/InetAddress.h"
#include "net/TcpConnection.h"

#include <functional>
#include <memory>

namespace l4lb {
namespace lb {

class Connector;

// 一条客户端连接 + 一条后端连接。所有函数在同一个 EventLoop 中执行。
class ProxySession : public std::enable_shared_from_this<ProxySession> {
public:
    using Ptr = std::shared_ptr<ProxySession>;

    ProxySession(net::EventLoop* loop, net::Socket client, net::InetAddress backend);
    ~ProxySession();

    void SetCloseCallback(std::function<void()> cb) { on_close_ = std::move(cb); }
    void Start();
    void Close();
    bool IsClosed() const { return closed_; }

    // 便于在测试/调试器中观察是否暂停读取及缓冲大小。
    const net::TcpConnection::Ptr& Client() const { return client_; }
    const net::TcpConnection::Ptr& Backend() const { return backend_; }

private:
    void OnBackendConnected(net::Socket socket);
    void OnClientMessage(net::Buffer* input);
    void OnBackendMessage(net::Buffer* input);
    void OnClientEof();
    void OnBackendEof();
    void CheckFinished();

    net::EventLoop* loop_;
    net::InetAddress backend_address_;
    net::TcpConnection::Ptr client_;
    net::TcpConnection::Ptr backend_;
    std::shared_ptr<Connector> connector_;
    std::function<void()> on_close_;
    bool closed_{false};
};

}  // namespace lb
}  // namespace l4lb
