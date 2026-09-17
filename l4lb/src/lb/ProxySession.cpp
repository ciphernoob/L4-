#include "lb/ProxySession.h"

#include "base/AsyncLogger.h"
#include "lb/BackendPool.h"
#include "lb/LoadBalancer.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace l4lb {
namespace lb {
namespace {

const char* StateName(ProxySession::State state) {
    switch (state) {
    case ProxySession::State::kAccepted: return "accepted";
    case ProxySession::State::kConnecting: return "connecting";
    case ProxySession::State::kEstablished: return "established";
    case ProxySession::State::kDraining: return "draining";
    case ProxySession::State::kClosed: return "closed";
    }
    return "unknown";
}

const char* ReasonName(ProxySession::CloseReason reason) {
    switch (reason) {
    case ProxySession::CloseReason::kNone: return "none";
    case ProxySession::CloseReason::kCompleted: return "completed";
    case ProxySession::CloseReason::kConnectFailed: return "connect_failed";
    case ProxySession::CloseReason::kConnectTimeout: return "connect_timeout";
    case ProxySession::CloseReason::kIdleTimeout: return "idle_timeout";
    case ProxySession::CloseReason::kNoAvailableBackend: return "no_backend";
    case ProxySession::CloseReason::kPeerError: return "peer_error";
    case ProxySession::CloseReason::kExplicit: return "explicit";
    }
    return "unknown";
}

}  // namespace

ProxySession::ProxySession(net::EventLoop* loop, Id id,
                           std::shared_ptr<net::TcpConnection> frontend,
                           Options options, RemoveCallback remove_callback)
    : loop_(loop),
      id_(id),
      options_(std::move(options)),
      remove_callback_(std::move(remove_callback)),
      frontend_(std::move(frontend)) {
    if (loop_ == nullptr || !frontend_ || frontend_->Loop() != loop_) {
        throw std::invalid_argument("ProxySession requires a frontend on its EventLoop");
    }
    if (options_.low_watermark >= options_.high_watermark) {
        throw std::invalid_argument("ProxySession watermarks are invalid");
    }
    if (options_.idle_timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("ProxySession idle timeout must not be negative");
    }
    if (static_cast<bool>(options_.backend_pool) !=
        static_cast<bool>(options_.load_balancer)) {
        throw std::invalid_argument(
            "ProxySession backend pool and load balancer must be configured together");
    }
}

ProxySession::~ProxySession() {
    assert(state_ == State::kClosed && "ProxySession must close before destruction");
}

void ProxySession::Start() {
    loop_->AssertInLoopThread();
    if (state_ != State::kAccepted) {
        throw std::logic_error("ProxySession::Start called in invalid state");
    }
    if (frontend_->GetState() != net::TcpConnection::State::kConnected) {
        throw std::logic_error("ProxySession frontend must already be established");
    }

    InstallFrontendCallbacks();
    frontend_->SetWatermarks(options_.low_watermark, options_.high_watermark);
    frontend_->SetMaxInputBufferBytes(options_.high_watermark);
    state_ = State::kConnecting;
    if (options_.logger) {
        options_.logger->LogSession(id_, "", StateName(state_), "none");
    }
    if (options_.counters) {
        options_.counters->current_sessions.fetch_add(1, std::memory_order_relaxed);
        options_.counters->total_sessions.fetch_add(1, std::memory_order_relaxed);
        counted_session_ = true;
    }

    if (options_.backend_pool) {
        if (!SelectAndConnectBackend()) {
            Close(CloseReason::kNoAvailableBackend);
        }
        return;
    }
    StartConnector(options_.backend);
}

bool ProxySession::SelectAndConnectBackend() {
    std::shared_ptr<const BackendSnapshot> source = options_.backend_pool->Snapshot();
    std::shared_ptr<BackendSnapshot> filtered(new BackendSnapshot());
    if (source) {
        filtered->counters = source->counters;
        for (const std::shared_ptr<Backend>& backend : source->candidates) {
            if (attempted_backends_.count(backend->Id()) == 0) {
                filtered->candidates.push_back(backend);
            }
        }
    }
    std::shared_ptr<const BackendSnapshot> candidates = filtered;
    SelectionResult result = options_.load_balancer->Select(candidates);
    if (!result) {
        return false;
    }
    backend_lease_ = std::move(result.lease);
    if (options_.logger) {
        options_.logger->LogSession(id_, backend_lease_.Get()->Id(),
                                    StateName(state_), "none");
    }
    attempted_backends_.insert(backend_lease_.Get()->Id());
    StartConnector(backend_lease_.Get()->Address());
    return true;
}

void ProxySession::StartConnector(const net::InetAddress& address) {
    ++connect_attempts_;
    connector_.reset(new Connector(loop_, address, options_.connect_timeout));
    const std::weak_ptr<ProxySession> weak = shared_from_this();
    connector_->SetSuccessCallback(
        [weak](const std::shared_ptr<Connector>&, net::Socket socket) mutable {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                session->HandleConnected(std::move(socket));
            }
        });
    connector_->SetErrorCallback(
        [weak](const std::shared_ptr<Connector>&, const std::error_code& error) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                session->HandleConnectError(error);
            }
        });
    loop_->QueueInLoop([weak] {
        if (const std::shared_ptr<ProxySession> session = weak.lock()) {
            if (session->state_ == State::kConnecting && session->connector_) {
                session->connector_->Start();
            }
        }
    });
}

void ProxySession::Close(CloseReason reason) {
    loop_->AssertInLoopThread();
    if (state_ == State::kClosed) {
        return;
    }
    const std::shared_ptr<ProxySession> self = shared_from_this();
    state_ = State::kClosed;
    close_reason_ = reason;
    if (options_.logger) {
        options_.logger->LogSession(
            id_, backend_lease_ ? backend_lease_.Get()->Id() : "",
            StateName(state_), ReasonName(close_reason_));
    }
    if (counted_session_) {
        options_.counters->current_sessions.fetch_sub(1, std::memory_order_relaxed);
        counted_session_ = false;
    }

    if (connector_) {
        connector_->Cancel();
        connector_.reset();
    }
    idle_timer_.Cancel();
    if (frontend_) {
        frontend_->ForceClose();
    }
    if (backend_) {
        backend_->ForceClose();
    }
    frontend_.reset();
    backend_.reset();
    backend_lease_.Reset();
    if (remove_callback_) {
        remove_callback_(id_);
    }
}

std::size_t ProxySession::PendingFrontendBytes() const noexcept {
    return frontend_ ? frontend_->InputBufferBytes() : 0;
}

void ProxySession::InstallFrontendCallbacks() {
    const std::weak_ptr<ProxySession> weak = shared_from_this();
    frontend_->SetMessageCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&, net::Buffer* input) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                session->HandleFrontendData(input);
            }
        });
    frontend_->SetEofCallback([weak](const std::shared_ptr<net::TcpConnection>&) {
        if (const std::shared_ptr<ProxySession> session = weak.lock()) {
            session->HandleFrontendEof();
        }
    });
    frontend_->SetErrorCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&, const std::error_code&) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                session->HandleTerminalError();
            }
        });
    frontend_->SetCloseCallback([weak](const std::shared_ptr<net::TcpConnection>&) {
        if (const std::shared_ptr<ProxySession> session = weak.lock()) {
            session->HandleTerminalError();
        }
    });
    frontend_->SetHighWatermarkCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&, std::size_t) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                if (session->backend_) {
                    if (session->options_.counters) {
                        session->options_.counters->backpressure_events.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    session->backend_->StopRead();
                }
            }
        });
    frontend_->SetLowWatermarkCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&, std::size_t) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                if (session->backend_ && session->backend_->InputBufferBytes() > 0) {
                    session->HandleBackendData(session->backend_->InputBuffer());
                }
                if (session->backend_ && !session->backend_eof_ &&
                    session->backend_->InputBufferBytes() == 0) {
                    session->backend_->StartRead();
                }
                session->CheckForCompletedDrain();
            }
        });
    frontend_->SetWriteCompleteCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                session->CheckForCompletedDrain();
            }
        });
}

void ProxySession::InstallBackendCallbacks() {
    const std::weak_ptr<ProxySession> weak = shared_from_this();
    backend_->SetMessageCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&, net::Buffer* input) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                session->HandleBackendData(input);
            }
        });
    backend_->SetEofCallback([weak](const std::shared_ptr<net::TcpConnection>&) {
        if (const std::shared_ptr<ProxySession> session = weak.lock()) {
            session->HandleBackendEof();
        }
    });
    backend_->SetErrorCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&, const std::error_code&) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                session->HandleTerminalError();
            }
        });
    backend_->SetCloseCallback([weak](const std::shared_ptr<net::TcpConnection>&) {
        if (const std::shared_ptr<ProxySession> session = weak.lock()) {
            session->HandleTerminalError();
        }
    });
    backend_->SetHighWatermarkCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&, std::size_t) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                if (session->frontend_) {
                    if (session->options_.counters) {
                        session->options_.counters->backpressure_events.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    session->frontend_->StopRead();
                }
            }
        });
    backend_->SetLowWatermarkCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&, std::size_t) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                if (session->frontend_ && session->frontend_->InputBufferBytes() > 0) {
                    session->HandleFrontendData(session->frontend_->InputBuffer());
                }
                if (session->frontend_ && !session->frontend_eof_ &&
                    session->frontend_->InputBufferBytes() == 0) {
                    session->frontend_->StartRead();
                }
                session->CheckForCompletedDrain();
            }
        });
    backend_->SetWriteCompleteCallback(
        [weak](const std::shared_ptr<net::TcpConnection>&) {
            if (const std::shared_ptr<ProxySession> session = weak.lock()) {
                session->CheckForCompletedDrain();
            }
        });
}

void ProxySession::HandleConnected(net::Socket socket) {
    loop_->AssertInLoopThread();
    if (state_ != State::kConnecting) {
        return;
    }
    connector_.reset();
    backend_.reset(new net::TcpConnection(
        loop_, std::move(socket), "backend-" + std::to_string(id_)));
    backend_->SetWatermarks(options_.low_watermark, options_.high_watermark);
    backend_->SetMaxInputBufferBytes(options_.high_watermark);
    InstallBackendCallbacks();
    backend_->Establish();
    state_ = frontend_eof_ ? State::kDraining : State::kEstablished;
    if (options_.logger) {
        options_.logger->LogSession(
            id_, backend_lease_ ? backend_lease_.Get()->Id() : "fixed",
            StateName(state_), "none");
    }
    TouchActivity();

    HandleFrontendData(frontend_->InputBuffer());
    if (state_ == State::kClosed) {
        return;
    }
    if (frontend_eof_) {
        backend_->ShutdownWrite();
    } else if (backend_->OutputBufferBytes() < options_.high_watermark) {
        frontend_->StartRead();
    }
    CheckForCompletedDrain();
}

void ProxySession::HandleConnectError(const std::error_code& error) {
    if (state_ == State::kConnecting) {
        connector_.reset();
        backend_lease_.Reset();
        if (options_.counters) {
            options_.counters->connect_failures.fetch_add(1, std::memory_order_relaxed);
            if (error == std::errc::timed_out) {
                options_.counters->connect_timeouts.fetch_add(1,
                                                              std::memory_order_relaxed);
            }
        }
        const bool may_retry = options_.backend_pool &&
                               connect_attempts_ <= options_.max_connect_retries;
        if (may_retry && SelectAndConnectBackend()) {
            return;
        }
        Close(error == std::errc::timed_out ? CloseReason::kConnectTimeout
                                            : CloseReason::kConnectFailed);
    }
}

void ProxySession::HandleFrontendData(net::Buffer* input) {
    TouchActivity();
    if (state_ == State::kConnecting) {
        if (input->ReadableBytes() > peak_pending_frontend_bytes_) {
            peak_pending_frontend_bytes_ = input->ReadableBytes();
        }
        if (input->ReadableBytes() >= options_.high_watermark) {
            frontend_->StopRead();
        }
        return;
    }
    if ((state_ == State::kEstablished || state_ == State::kDraining) && backend_) {
        const std::shared_ptr<net::TcpConnection> destination = backend_;
        const std::size_t buffered = destination->OutputBufferBytes();
        const std::size_t available =
            buffered < options_.high_watermark ? options_.high_watermark - buffered : 0;
        const std::size_t transfer = std::min(input->ReadableBytes(), available);
        if (transfer > 0) {
            destination->Send(input->Peek(), transfer);
            input->Retrieve(transfer);
            if (options_.counters) {
                options_.counters->client_to_backend_bytes.fetch_add(
                    transfer, std::memory_order_relaxed);
            }
        }
        if (input->ReadableBytes() > 0 ||
            destination->OutputBufferBytes() >= options_.high_watermark) {
            if (frontend_) {
                frontend_->StopRead();
            }
        } else if (frontend_ && !frontend_eof_) {
            frontend_->StartRead();
        }
    }
}

void ProxySession::HandleBackendData(net::Buffer* input) {
    TouchActivity();
    if ((state_ == State::kEstablished || state_ == State::kDraining) && frontend_) {
        const std::shared_ptr<net::TcpConnection> destination = frontend_;
        const std::size_t buffered = destination->OutputBufferBytes();
        const std::size_t available =
            buffered < options_.high_watermark ? options_.high_watermark - buffered : 0;
        const std::size_t transfer = std::min(input->ReadableBytes(), available);
        if (transfer > 0) {
            destination->Send(input->Peek(), transfer);
            input->Retrieve(transfer);
            if (options_.counters) {
                options_.counters->backend_to_client_bytes.fetch_add(
                    transfer, std::memory_order_relaxed);
            }
        }
        if (input->ReadableBytes() > 0 ||
            destination->OutputBufferBytes() >= options_.high_watermark) {
            if (backend_) {
                backend_->StopRead();
            }
        } else if (backend_ && !backend_eof_) {
            backend_->StartRead();
        }
    }
}

void ProxySession::HandleFrontendEof() {
    frontend_eof_ = true;
    EnterDraining();
    if (backend_) {
        backend_->ShutdownWrite();
    }
    CheckForCompletedDrain();
}

void ProxySession::HandleBackendEof() {
    backend_eof_ = true;
    EnterDraining();
    if (frontend_) {
        frontend_->ShutdownWrite();
    }
    CheckForCompletedDrain();
}

void ProxySession::HandleTerminalError() {
    if (state_ != State::kClosed) {
        Close(CloseReason::kPeerError);
    }
}

void ProxySession::EnterDraining() {
    if (state_ == State::kEstablished) {
        state_ = State::kDraining;
        if (options_.logger) {
            options_.logger->LogSession(
                id_, backend_lease_ ? backend_lease_.Get()->Id() : "fixed",
                StateName(state_), "none");
        }
    }
}

void ProxySession::CheckForCompletedDrain() {
    if (state_ == State::kDraining && frontend_eof_ && backend_eof_ && frontend_ &&
        backend_ && frontend_->OutputBufferBytes() == 0 &&
        backend_->OutputBufferBytes() == 0) {
        Close(CloseReason::kCompleted);
    }
}

void ProxySession::TouchActivity() {
    if (state_ == State::kClosed ||
        options_.idle_timeout <= std::chrono::milliseconds::zero()) {
        return;
    }
    idle_timer_.Cancel();
    const std::weak_ptr<ProxySession> weak = shared_from_this();
    idle_timer_ = loop_->RunAfter(options_.idle_timeout, [weak] {
        if (const std::shared_ptr<ProxySession> session = weak.lock()) {
            session->HandleIdleTimeout();
        }
    });
}

void ProxySession::HandleIdleTimeout() {
    if (state_ == State::kEstablished || state_ == State::kDraining) {
        if (options_.counters) {
            options_.counters->idle_timeouts.fetch_add(1, std::memory_order_relaxed);
        }
        Close(CloseReason::kIdleTimeout);
    }
}

}  // namespace lb
}  // namespace l4lb
