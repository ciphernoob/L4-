#include "app/L4ProxyServer.h"

#include "base/AsyncLogger.h"
#include "lb/BackendPool.h"
#include "lb/HealthChecker.h"
#include "lb/LoadBalancer.h"
#include "lb/SessionMap.h"
#include "net/Acceptor.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"

#include <future>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace l4lb {
namespace app {

struct L4ProxyServer::WorkerState {
    WorkerState(net::EventLoop* owner,
                std::shared_ptr<std::atomic<lb::ProxySession::Id>> next_id)
        : loop(owner), sessions(owner, std::move(next_id)) {}
    net::EventLoop* loop;
    lb::SessionMap sessions;
};

L4ProxyServer::L4ProxyServer(net::EventLoop* base_loop, config::ServerConfig config)
    : base_loop_(base_loop),
      config_(std::move(config)),
      backend_pool_(new lb::BackendPool(config_.backends)),
      logger_(new base::AsyncLogger()),
      thread_pool_(base_loop) {
    if (base_loop_ == nullptr) {
        throw std::invalid_argument("L4ProxyServer requires a base EventLoop");
    }
    if (config_.algorithm == config::Algorithm::kRoundRobin) {
        load_balancer_.reset(new lb::RoundRobinLoadBalancer());
    } else {
        load_balancer_.reset(new lb::LeastConnectionsLoadBalancer());
    }
}

L4ProxyServer::~L4ProxyServer() {
    Stop();
}

void L4ProxyServer::Start() {
    base_loop_->AssertInLoopThread();
    if (started_) {
        throw std::logic_error("L4ProxyServer cannot be started twice");
    }
    thread_pool_.Start(config_.workers);
    try {
        CreateWorkers();
        data_acceptor_.reset(new net::Acceptor(base_loop_, config_.listen, true));
        admin_acceptor_.reset(
            new net::Acceptor(base_loop_, config_.admin_listen, true));
        data_acceptor_->SetNewConnectionCallback(
            [this](net::Socket socket, const net::InetAddress&) {
                HandleClient(std::move(socket));
            });
        admin_acceptor_->SetNewConnectionCallback(
            [this](net::Socket socket, const net::InetAddress&) {
                HandleAdminClient(std::move(socket));
            });
        data_acceptor_->Start();
        admin_acceptor_->Start();
        health_checker_.reset(new lb::HealthChecker(
            base_loop_, backend_pool_,
            lb::HealthChecker::Options{config_.health_check.interval,
                                       config_.health_check.timeout,
                                       config_.health_check.failure_threshold,
                                       config_.health_check.success_threshold}));
        health_checker_->Start();
        started_ = true;
    } catch (...) {
        if (health_checker_) {
            health_checker_->Stop();
            health_checker_.reset();
        }
        CloseWorkers();
        thread_pool_.Stop();
        workers_.clear();
        data_acceptor_.reset();
        admin_acceptor_.reset();
        throw;
    }
}

void L4ProxyServer::Stop() {
    if (!started_ && workers_.empty()) {
        return;
    }
    base_loop_->AssertInLoopThread();
    shutdown_poll_timer_.Cancel();
    shutdown_deadline_timer_.Cancel();
    if (data_acceptor_) {
        data_acceptor_->Stop();
    }
    if (admin_acceptor_) {
        admin_acceptor_->Stop();
    }
    if (health_checker_) {
        health_checker_->Stop();
        health_checker_.reset();
    }
    std::vector<std::shared_ptr<net::TcpConnection>> admin_connections;
    admin_connections.reserve(admin_connections_.size());
    for (const auto& entry : admin_connections_) {
        admin_connections.push_back(entry.second);
    }
    for (const std::shared_ptr<net::TcpConnection>& connection : admin_connections) {
        connection->ForceClose();
    }
    admin_connections_.clear();
    admin_timers_.clear();
    CloseWorkers();
    thread_pool_.Stop();
    workers_.clear();
    logger_->Flush();
    logger_->Stop();
    data_acceptor_.reset();
    admin_acceptor_.reset();
    started_ = false;
    shutting_down_ = false;
}

void L4ProxyServer::BeginShutdown() {
    base_loop_->AssertInLoopThread();
    if (!started_ || shutting_down_) {
        return;
    }
    shutting_down_ = true;
    data_acceptor_->Stop();
    admin_acceptor_->Stop();
    if (health_checker_) {
        health_checker_->Stop();
        health_checker_.reset();
    }
    if (counters_->current_sessions.load(std::memory_order_relaxed) == 0 ||
        config_.shutdown_grace <= std::chrono::milliseconds::zero()) {
        Stop();
        base_loop_->Quit();
        return;
    }
    shutdown_poll_timer_ = base_loop_->RunEvery(std::chrono::milliseconds(10), [this] {
        if (counters_->current_sessions.load(std::memory_order_relaxed) == 0) {
            Stop();
            base_loop_->Quit();
        }
    });
    shutdown_deadline_timer_ = base_loop_->RunAfter(config_.shutdown_grace, [this] {
        Stop();
        base_loop_->Quit();
    });
}

net::InetAddress L4ProxyServer::ListenAddress() const {
    if (!data_acceptor_) {
        throw std::logic_error("data listener is not started");
    }
    return data_acceptor_->ListenAddress();
}

net::InetAddress L4ProxyServer::AdminAddress() const {
    if (!admin_acceptor_) {
        throw std::logic_error("admin listener is not started");
    }
    return admin_acceptor_->ListenAddress();
}

void L4ProxyServer::CreateWorkers() {
    for (net::EventLoop* loop : thread_pool_.Loops()) {
        std::shared_ptr<std::promise<std::shared_ptr<WorkerState>>> promise(
            new std::promise<std::shared_ptr<WorkerState>>());
        std::future<std::shared_ptr<WorkerState>> future = promise->get_future();
        const auto next_id = next_session_id_;
        loop->QueueInLoop([loop, promise, next_id] {
            promise->set_value(std::make_shared<WorkerState>(loop, next_id));
        });
        workers_.push_back(future.get());
    }
}

void L4ProxyServer::HandleClient(net::Socket socket) {
    net::EventLoop* worker_loop = thread_pool_.NextLoop();
    std::shared_ptr<WorkerState> state;
    for (const std::shared_ptr<WorkerState>& candidate : workers_) {
        if (candidate->loop == worker_loop) {
            state = candidate;
            break;
        }
    }
    if (!state) {
        throw std::logic_error("selected worker has no state");
    }

    std::shared_ptr<net::Socket> accepted(new net::Socket(std::move(socket)));
    worker_loop->QueueInLoop(
        [this, state, accepted]() mutable {
            std::shared_ptr<net::TcpConnection> frontend(new net::TcpConnection(
                state->loop, std::move(*accepted), "frontend"));
            frontend->Establish();
            lb::ProxySession::Options options(net::InetAddress(
                                                  config_.backends.front().ip,
                                                  config_.backends.front().port),
                                              config_.connect_timeout,
                                              config_.low_watermark,
                                              config_.high_watermark);
            options.backend_pool = backend_pool_;
            options.load_balancer = load_balancer_;
            options.counters = counters_;
            options.logger = logger_;
            options.idle_timeout = config_.idle_timeout;
            options.max_connect_retries = config_.max_connect_retries;
            std::shared_ptr<lb::ProxySession> session =
                state->sessions.Add(std::move(frontend), std::move(options));
            session->Start();
        });
}

void L4ProxyServer::HandleAdminClient(net::Socket socket) {
    const std::uint64_t id = next_admin_id_++;
    std::shared_ptr<net::TcpConnection> connection(
        new net::TcpConnection(base_loop_, std::move(socket), "admin"));
    connection->SetMaxInputBufferBytes(4096);
    connection->SetMessageCallback(
        [this, id](const std::shared_ptr<net::TcpConnection>&, net::Buffer* input) {
            HandleAdminRequest(id, input);
        });
    connection->SetCloseCallback(
        [this, id](const std::shared_ptr<net::TcpConnection>&) {
            admin_timers_.erase(id);
            admin_connections_.erase(id);
        });
    connection->SetErrorCallback(
        [this, id](const std::shared_ptr<net::TcpConnection>&, const std::error_code&) {
            admin_timers_.erase(id);
            admin_connections_.erase(id);
        });
    admin_connections_.emplace(id, connection);
    const std::chrono::milliseconds admin_idle =
        std::min(config_.idle_timeout, std::chrono::milliseconds(5000));
    admin_timers_.emplace(
        id, base_loop_->RunAfter(admin_idle, [this, id] {
            const auto found = admin_connections_.find(id);
            if (found != admin_connections_.end()) {
                found->second->ForceClose();
            }
        }));
    connection->Establish();
}

void L4ProxyServer::HandleAdminRequest(std::uint64_t id, net::Buffer* input) {
    const auto found = admin_connections_.find(id);
    if (found == admin_connections_.end()) {
        return;
    }
    const std::string request(input->Peek(), input->ReadableBytes());
    if (request.find("\r\n\r\n") == std::string::npos &&
        input->ReadableBytes() < 4096) {
        return;
    }
    admin_timers_.erase(id);
    input->RetrieveAll();
    std::string status;
    std::string body;
    if (request.compare(0, 13, "GET /metrics ") == 0) {
        status = "200 OK";
        body = RenderMetrics();
    } else if (request.compare(0, 13, "GET /healthz ") == 0) {
        status = "200 OK";
        body = "ok\n";
    } else if (request.compare(0, 4, "GET ") != 0) {
        status = "405 Method Not Allowed";
        body = "method not allowed\n";
    } else {
        status = "404 Not Found";
        body = "not found\n";
    }
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\nContent-Type: text/plain\r\n"
             << "Content-Length: " << body.size() << "\r\nConnection: close\r\n\r\n"
             << body;
    found->second->Send(response.str());
    found->second->ShutdownWrite();
}

std::string L4ProxyServer::RenderMetrics() const {
    std::ostringstream output;
    output << "l4lb_sessions_current " << counters_->current_sessions.load() << '\n'
           << "l4lb_sessions_total " << counters_->total_sessions.load() << '\n'
           << "l4lb_bytes_client_to_backend "
           << counters_->client_to_backend_bytes.load() << '\n'
           << "l4lb_bytes_backend_to_client "
           << counters_->backend_to_client_bytes.load() << '\n'
           << "l4lb_connect_failures " << counters_->connect_failures.load() << '\n'
           << "l4lb_connect_timeouts " << counters_->connect_timeouts.load() << '\n'
           << "l4lb_idle_timeouts " << counters_->idle_timeouts.load() << '\n'
           << "l4lb_backpressure_events " << counters_->backpressure_events.load()
           << '\n'
           << "l4lb_no_available_backend "
           << backend_pool_->NoAvailableBackendCount() << '\n';
    for (const std::shared_ptr<lb::Backend>& backend : backend_pool_->Backends()) {
        output << "l4lb_backend_healthy{id=\"" << backend->Id() << "\"} "
               << (backend->IsHealthy() ? 1 : 0) << '\n'
               << "l4lb_backend_active_sessions{id=\"" << backend->Id() << "\"} "
               << backend->ActiveSessions() << '\n';
    }
    return output.str();
}

void L4ProxyServer::CloseWorkers() {
    std::vector<std::future<void>> completions;
    completions.reserve(workers_.size());
    for (const std::shared_ptr<WorkerState>& state : workers_) {
        std::shared_ptr<std::promise<void>> promise(new std::promise<void>());
        completions.push_back(promise->get_future());
        state->loop->QueueInLoop([state, promise] {
            state->sessions.CloseAll();
            promise->set_value();
        });
    }
    for (std::future<void>& completion : completions) {
        completion.get();
    }
}

}  // namespace app
}  // namespace l4lb
