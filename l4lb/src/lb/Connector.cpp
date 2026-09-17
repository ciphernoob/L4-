#include "lb/Connector.h"

#include "net/Channel.h"
#include "net/EventLoop.h"

#include <cerrno>
#include <sys/socket.h>

namespace l4lb {
namespace lb {

Connector::Connector(net::EventLoop* loop, net::InetAddress address,
                     std::chrono::milliseconds timeout)
    : loop_(loop), address_(std::move(address)), timeout_(timeout) {
    if (timeout_ < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("connector timeout must be non-negative");
    }
}

Connector::~Connector() {
    if (channel_ && channel_->IsAdded()) {
        std::terminate();
    }
}

void Connector::Start() {
    loop_->AssertInLoopThread();
    if (state_ != State::kIdle) {
        throw std::logic_error("Connector::Start called more than once");
    }
    socket_ = net::Socket::CreateTcpNonBlocking();
    const sockaddr_in& address = address_.SockAddr();
    const int result = ::connect(socket_.Fd(), reinterpret_cast<const sockaddr*>(&address),
                                 sizeof(address));
    if (result == 0) {
        state_ = State::kConnecting;
        CompleteSuccess();
        return;
    }

    const int error = errno;
    switch (error) {
        case EINPROGRESS:
        case EINTR:
        case EALREADY:
            state_ = State::kConnecting;
            BeginWaiting();
            return;
        case EISCONN:
            state_ = State::kConnecting;
            CompleteSuccess();
            return;
        default:
            state_ = State::kConnecting;
            CompleteFailure(error, State::kFailed);
            return;
    }
}

void Connector::Cancel() {
    loop_->AssertInLoopThread();
    if (state_ == State::kSucceeded || state_ == State::kFailed ||
        state_ == State::kCancelled) {
        return;
    }
    if (state_ == State::kIdle) {
        state_ = State::kCancelled;
        return;
    }
    CompleteFailure(ECANCELED, State::kCancelled);
}

void Connector::BeginWaiting() {
    channel_.reset(new net::Channel(loop_, socket_.Fd()));
    channel_->Tie(shared_from_this());
    channel_->SetWriteCallback([this] { HandleWrite(); });
    channel_->SetErrorCallback([this] { HandleError(); });
    channel_->SetCloseCallback([this] { HandleError(); });
    channel_->EnableWriting();
    const std::weak_ptr<Connector> weak = shared_from_this();
    timeout_timer_ = loop_->RunAfter(timeout_, [weak] {
        if (const std::shared_ptr<Connector> connector = weak.lock()) {
            connector->HandleTimeout();
        }
    });
}

void Connector::HandleWrite() {
    if (state_ != State::kConnecting) {
        return;
    }
    const int error = socket_.GetSocketError();
    if (error == 0) {
        CompleteSuccess();
    } else {
        CompleteFailure(error, State::kFailed);
    }
}

void Connector::HandleError() {
    if (state_ != State::kConnecting) {
        return;
    }
    int error = socket_.GetSocketError();
    if (error == 0) {
        error = ECONNABORTED;
    }
    CompleteFailure(error, State::kFailed);
}

void Connector::HandleTimeout() {
    if (state_ == State::kConnecting) {
        CompleteFailure(ETIMEDOUT, State::kFailed);
    }
}

void Connector::CompleteSuccess() {
    const std::shared_ptr<Connector> self = shared_from_this();
    RemoveChannel();
    timeout_timer_.Cancel();
    state_ = State::kSucceeded;
    net::Socket connected = std::move(socket_);
    if (success_callback_) {
        success_callback_(self, std::move(connected));
    }
}

void Connector::CompleteFailure(int error_number, State terminal_state) {
    const std::shared_ptr<Connector> self = shared_from_this();
    RemoveChannel();
    timeout_timer_.Cancel();
    socket_.Close();
    state_ = terminal_state;
    if (error_callback_) {
        error_callback_(self, std::error_code(error_number, std::generic_category()));
    }
}

void Connector::RemoveChannel() {
    if (channel_ && channel_->IsAdded()) {
        channel_->Remove();
    }
}

}  // namespace lb
}  // namespace l4lb

