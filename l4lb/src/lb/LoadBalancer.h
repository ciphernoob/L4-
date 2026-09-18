#pragma once

#include "net/Acceptor.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace l4lb {
namespace lb {

class ProxySession;

// 服务入口：接入客户端、轮询选后端、拥有全部会话。
class LoadBalancer {
public:
    LoadBalancer(net::EventLoop* loop, const net::InetAddress& listen,
                 std::vector<net::InetAddress> backends);
    ~LoadBalancer();

    void Start();
    void Stop();
    net::InetAddress ListenAddress() const { return acceptor_.ListenAddress(); }
    std::size_t SessionCount() const { return sessions_.size(); }

private:
    void OnNewConnection(net::Socket socket);

    net::EventLoop* loop_;
    net::Acceptor acceptor_;
    std::vector<net::InetAddress> backends_;
    std::size_t next_backend_{0};
    std::unordered_map<int, std::shared_ptr<ProxySession>> sessions_;
};

}  // namespace lb
}  // namespace l4lb
