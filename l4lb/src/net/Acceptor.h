#pragma once

#include "net/InetAddress.h"
#include "net/Socket.h"
#include "net/UniqueFd.h"

#include <functional>
#include <memory>
#include <system_error>
#include <utility>

namespace l4lb {
namespace net {

class Channel;
class EventLoop;

class Acceptor {
public:
    using NewConnectionCallback = std::function<void(Socket, const InetAddress&)>;
    using ErrorCallback = std::function<void(const std::error_code&)>;

    Acceptor(EventLoop* loop, const InetAddress& address, bool edge_triggered);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void SetNewConnectionCallback(NewConnectionCallback callback) {
        new_connection_callback_ = std::move(callback);
    }
    void SetErrorCallback(ErrorCallback callback) { error_callback_ = std::move(callback); }

    void Start();
    void Stop();
    bool IsListening() const noexcept { return listening_; }
    const InetAddress& ListenAddress() const noexcept { return listen_address_; }

private:
    void HandleRead();
    void HandleDescriptorExhaustion();
    void ReportError(const std::error_code& error);
    static UniqueFd OpenIdleFd();

    EventLoop* loop_;
    Socket listen_socket_;
    InetAddress listen_address_;
    std::unique_ptr<Channel> channel_;
    UniqueFd idle_fd_;
    bool edge_triggered_;
    bool listening_{false};
    NewConnectionCallback new_connection_callback_;
    ErrorCallback error_callback_;
};

}  // namespace net
}  // namespace l4lb
