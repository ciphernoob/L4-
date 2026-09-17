#pragma once

#include "lb/Connector.h"
#include "lb/Backend.h"
#include "net/InetAddress.h"
#include "net/TimerId.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace l4lb {
namespace base {
class AsyncLogger;
}
namespace net {
class Buffer;
class EventLoop;
class TcpConnection;
}  // namespace net

namespace lb {

class BackendPool;
class LoadBalancer;

struct SessionCounters {
    std::atomic<std::uint64_t> current_sessions{0};
    std::atomic<std::uint64_t> total_sessions{0};
    std::atomic<std::uint64_t> client_to_backend_bytes{0};
    std::atomic<std::uint64_t> backend_to_client_bytes{0};
    std::atomic<std::uint64_t> backpressure_events{0};
    std::atomic<std::uint64_t> connect_failures{0};
    std::atomic<std::uint64_t> connect_timeouts{0};
    std::atomic<std::uint64_t> idle_timeouts{0};
};

class ProxySession : public std::enable_shared_from_this<ProxySession> {
public:
    using Id = std::uint64_t;

    enum class State {
        kAccepted,
        kConnecting,
        kEstablished,
        kDraining,
        kClosed,
    };

    enum class CloseReason {
        kNone,
        kCompleted,
        kConnectFailed,
        kConnectTimeout,
        kIdleTimeout,
        kNoAvailableBackend,
        kPeerError,
        kExplicit,
    };

    struct Options {
        Options(net::InetAddress backend_address,
                std::chrono::milliseconds connection_timeout,
                std::size_t low, std::size_t high)
            : backend(std::move(backend_address)),
              connect_timeout(connection_timeout),
              low_watermark(low),
              high_watermark(high) {}

        net::InetAddress backend;
        std::chrono::milliseconds connect_timeout{1000};
        std::size_t low_watermark{64 * 1024};
        std::size_t high_watermark{256 * 1024};
        std::shared_ptr<BackendPool> backend_pool;
        std::shared_ptr<LoadBalancer> load_balancer;
        std::shared_ptr<SessionCounters> counters;
        std::shared_ptr<base::AsyncLogger> logger;
        std::chrono::milliseconds idle_timeout{0};
        std::size_t max_connect_retries{0};
    };

    using RemoveCallback = std::function<void(Id)>;

    ProxySession(net::EventLoop* loop, Id id,
                 std::shared_ptr<net::TcpConnection> frontend, Options options,
                 RemoveCallback remove_callback);
    ~ProxySession();

    ProxySession(const ProxySession&) = delete;
    ProxySession& operator=(const ProxySession&) = delete;

    void Start();
    void Close(CloseReason reason = CloseReason::kExplicit);

    Id SessionId() const noexcept { return id_; }
    State GetState() const noexcept { return state_; }
    CloseReason Reason() const noexcept { return close_reason_; }
    std::size_t PendingFrontendBytes() const noexcept;
    std::size_t PeakPendingFrontendBytes() const noexcept {
        return peak_pending_frontend_bytes_;
    }
    const std::shared_ptr<net::TcpConnection>& Frontend() const noexcept {
        return frontend_;
    }
    const std::shared_ptr<net::TcpConnection>& BackendConnection() const noexcept {
        return backend_;
    }
    const Backend* SelectedBackend() const noexcept { return backend_lease_.Get(); }
    std::size_t ConnectAttempts() const noexcept { return connect_attempts_; }

private:
    void InstallFrontendCallbacks();
    void InstallBackendCallbacks();
    bool SelectAndConnectBackend();
    void StartConnector(const net::InetAddress& address);
    void HandleConnected(net::Socket socket);
    void HandleConnectError(const std::error_code& error);
    void HandleFrontendData(net::Buffer* input);
    void HandleBackendData(net::Buffer* input);
    void HandleFrontendEof();
    void HandleBackendEof();
    void HandleTerminalError();
    void EnterDraining();
    void CheckForCompletedDrain();
    void TouchActivity();
    void HandleIdleTimeout();

    net::EventLoop* loop_;
    const Id id_;
    Options options_;
    RemoveCallback remove_callback_;
    State state_{State::kAccepted};
    CloseReason close_reason_{CloseReason::kNone};
    bool frontend_eof_{false};
    bool backend_eof_{false};
    bool counted_session_{false};
    std::size_t peak_pending_frontend_bytes_{0};
    std::size_t connect_attempts_{0};
    std::unordered_set<std::string> attempted_backends_;
    BackendLease backend_lease_;
    net::TimerId idle_timer_;
    std::shared_ptr<net::TcpConnection> frontend_;
    std::shared_ptr<Connector> connector_;
    std::shared_ptr<net::TcpConnection> backend_;
};

}  // namespace lb
}  // namespace l4lb
