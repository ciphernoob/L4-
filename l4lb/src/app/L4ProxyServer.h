#pragma once

#include "config/Config.h"
#include "lb/ProxySession.h"
#include "net/EventLoopThreadPool.h"
#include "net/TimerId.h"

#include <cstdint>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

namespace l4lb {
namespace base {
class AsyncLogger;
}
namespace lb {
class BackendPool;
class HealthChecker;
class LoadBalancer;
class SessionMap;
}  // namespace lb
namespace net {
class Acceptor;
class Buffer;
class EventLoop;
class Socket;
class TcpConnection;
}  // namespace net

namespace app {

class L4ProxyServer {
public:
    L4ProxyServer(net::EventLoop* base_loop, config::ServerConfig config);
    ~L4ProxyServer();

    L4ProxyServer(const L4ProxyServer&) = delete;
    L4ProxyServer& operator=(const L4ProxyServer&) = delete;

    void Start();
    void Stop();
    void BeginShutdown();

    bool IsStarted() const noexcept { return started_; }
    net::InetAddress ListenAddress() const;
    net::InetAddress AdminAddress() const;
    std::shared_ptr<lb::BackendPool> Backends() const noexcept { return backend_pool_; }
    std::shared_ptr<lb::SessionCounters> Counters() const noexcept { return counters_; }

private:
    struct WorkerState;
    void CreateWorkers();
    void HandleClient(net::Socket socket);
    void HandleAdminClient(net::Socket socket);
    void HandleAdminRequest(std::uint64_t id, net::Buffer* input);
    std::string RenderMetrics() const;
    void CloseWorkers();

    net::EventLoop* base_loop_;
    config::ServerConfig config_;
    std::shared_ptr<lb::BackendPool> backend_pool_;
    std::shared_ptr<lb::LoadBalancer> load_balancer_;
    std::shared_ptr<lb::SessionCounters> counters_{new lb::SessionCounters()};
    std::shared_ptr<std::atomic<lb::ProxySession::Id>> next_session_id_{
        new std::atomic<lb::ProxySession::Id>(1)};
    std::shared_ptr<base::AsyncLogger> logger_;
    std::unique_ptr<lb::HealthChecker> health_checker_;
    net::EventLoopThreadPool thread_pool_;
    std::vector<std::shared_ptr<WorkerState>> workers_;
    std::unique_ptr<net::Acceptor> data_acceptor_;
    std::unique_ptr<net::Acceptor> admin_acceptor_;
    std::uint64_t next_admin_id_{1};
    std::unordered_map<std::uint64_t, std::shared_ptr<net::TcpConnection>>
        admin_connections_;
    std::unordered_map<std::uint64_t, net::TimerId> admin_timers_;
    net::TimerId shutdown_poll_timer_;
    net::TimerId shutdown_deadline_timer_;
    bool started_{false};
    bool shutting_down_{false};
};

}  // namespace app
}  // namespace l4lb
