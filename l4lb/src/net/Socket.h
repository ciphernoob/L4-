#pragma once

#include "net/InetAddress.h"
#include "net/UniqueFd.h"

#include <utility>

namespace l4lb {
namespace net {

class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(UniqueFd fd) noexcept : fd_(std::move(fd)) {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;

    static Socket CreateTcpNonBlocking();

    int Fd() const noexcept { return fd_.Get(); }
    bool IsValid() const noexcept { return fd_.IsValid(); }
    UniqueFd Release() noexcept { return UniqueFd(fd_.Release()); }

    void SetReuseAddress(bool enabled);
    void Bind(const InetAddress& address);
    InetAddress LocalAddress() const;
    void Listen(int backlog = SOMAXCONN);
    Socket Accept(InetAddress* peer_address = nullptr);
    void ShutdownWrite();
    int GetSocketError() const;

private:
    UniqueFd fd_;
};

}  // namespace net
}  // namespace l4lb
