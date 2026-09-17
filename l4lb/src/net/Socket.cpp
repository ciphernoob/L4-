#include "net/Socket.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>

namespace l4lb {
namespace net {
namespace {

void ThrowSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

}  // namespace

Socket Socket::CreateTcpNonBlocking() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        ThrowSystemError("socket");
    }
    return Socket(UniqueFd(fd));
}

void Socket::SetReuseAddress(bool enabled) {
    const int value = enabled ? 1 : 0;
    if (::setsockopt(Fd(), SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) < 0) {
        ThrowSystemError("setsockopt(SO_REUSEADDR)");
    }
}

void Socket::Bind(const InetAddress& address) {
    const sockaddr_in& value = address.SockAddr();
    if (::bind(Fd(), reinterpret_cast<const sockaddr*>(&value), sizeof(value)) < 0) {
        ThrowSystemError("bind");
    }
}

InetAddress Socket::LocalAddress() const {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(Fd(), reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        ThrowSystemError("getsockname");
    }
    return InetAddress(address);
}

void Socket::Listen(int backlog) {
    if (::listen(Fd(), backlog) < 0) {
        ThrowSystemError("listen");
    }
}

Socket Socket::Accept(InetAddress* peer_address) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    const int accepted = ::accept4(Fd(), reinterpret_cast<sockaddr*>(&peer), &length,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Socket();
        }
        ThrowSystemError("accept4");
    }
    if (peer_address != nullptr) {
        *peer_address = InetAddress(peer);
    }
    return Socket(UniqueFd(accepted));
}

void Socket::ShutdownWrite() {
    if (::shutdown(Fd(), SHUT_WR) < 0 && errno != ENOTCONN) {
        ThrowSystemError("shutdown(SHUT_WR)");
    }
}

int Socket::GetSocketError() const {
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(Fd(), SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
        ThrowSystemError("getsockopt(SO_ERROR)");
    }
    return error;
}

}  // namespace net
}  // namespace l4lb
