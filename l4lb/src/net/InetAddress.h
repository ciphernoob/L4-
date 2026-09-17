#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace l4lb {
namespace net {

class InetAddress {
public:
    InetAddress(const std::string& ip, std::uint16_t port);
    explicit InetAddress(const sockaddr_in& address) noexcept;

    const sockaddr_in& SockAddr() const noexcept { return address_; }
    sockaddr_in* MutableSockAddr() noexcept { return &address_; }
    std::string Ip() const;
    std::uint16_t Port() const noexcept;
    std::string ToString() const;

private:
    sockaddr_in address_{};
};

}  // namespace net
}  // namespace l4lb

