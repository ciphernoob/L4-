#include "lb/LoadBalancer.h"

#include "lb/ProxySession.h"
#include "net/EventLoop.h"

#include <stdexcept>

namespace l4lb {
namespace lb {

LoadBalancer::LoadBalancer(net::EventLoop* loop, const net::InetAddress& listen,
                           std::vector<net::InetAddress> backends)
    : loop_(loop), acceptor_(loop, listen, false), backends_(std::move(backends)) {
    if (backends_.empty()) {
        throw std::invalid_argument("at least one backend is required");
    }
    acceptor_.SetNewConnectionCallback([this](net::Socket socket, const net::InetAddress&) {
        OnNewConnection(std::move(socket));
    });
}

LoadBalancer::~LoadBalancer() {
    Stop();
}

void LoadBalancer::Start() {
    acceptor_.Start();
}

void LoadBalancer::OnNewConnection(net::Socket socket) {
    // 轮询以 TCP 连接为粒度；后续消息始终交给这个 session。
    const auto address = backends_[next_backend_];
    next_backend_ = (next_backend_ + 1) % backends_.size();
    const int fd = socket.Fd();
    auto session = std::make_shared<ProxySession>(loop_, std::move(socket), address);
    session->SetCloseCallback([this, fd] { sessions_.erase(fd); });
    sessions_.emplace(fd, session);  // 必须先持有，再启动可能立即失败的 connect。
    session->Start();
}

void LoadBalancer::Stop() {
    loop_->AssertInLoopThread();
    acceptor_.Stop();
    while (!sessions_.empty()) {
        auto session = sessions_.begin()->second;
        session->Close();  // 回调从 map 中删除自身。
    }
}

}  // namespace lb
}  // namespace l4lb
