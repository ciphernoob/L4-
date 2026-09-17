#pragma once

#include "net/InetAddress.h"
#include "net/Socket.h"
#include "net/TimerId.h"

#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

namespace l4lb {
namespace net {
class Channel;
class EventLoop;
}  // namespace net

namespace lb {

class Connector : public std::enable_shared_from_this<Connector> {
public:
    enum class State {
        kIdle,
        kConnecting,
        kSucceeded,
        kFailed,
        kCancelled,
    };

    using SuccessCallback =
        std::function<void(const std::shared_ptr<Connector>&, net::Socket)>;
    using ErrorCallback = std::function<void(const std::shared_ptr<Connector>&,
                                             const std::error_code&)>;

    Connector(net::EventLoop* loop, net::InetAddress address,
              std::chrono::milliseconds timeout);
    ~Connector();

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    void SetSuccessCallback(SuccessCallback callback) {
        success_callback_ = std::move(callback);
    }
    void SetErrorCallback(ErrorCallback callback) { error_callback_ = std::move(callback); }

    void Start();
    void Cancel();

    State GetState() const noexcept { return state_; }
    net::EventLoop* Loop() const noexcept { return loop_; }

private:
    void BeginWaiting();
    void HandleWrite();
    void HandleError();
    void HandleTimeout();
    void CompleteSuccess();
    void CompleteFailure(int error_number, State terminal_state);
    void RemoveChannel();

    net::EventLoop* loop_;
    const net::InetAddress address_;
    const std::chrono::milliseconds timeout_;
    State state_{State::kIdle};
    net::Socket socket_;
    std::unique_ptr<net::Channel> channel_;
    net::TimerId timeout_timer_;
    SuccessCallback success_callback_;
    ErrorCallback error_callback_;
};

}  // namespace lb
}  // namespace l4lb

