#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <stdexcept>

namespace l4lb {
namespace net {

InetAddress::InetAddress(const std::string& ip, std::uint16_t port) {
    address_.sin_family = AF_INET;
    address_.sin_port = htons(port);
    const int result = ::inet_pton(AF_INET, ip.c_str(), &address_.sin_addr);
    if (result != 1) {
        throw std::invalid_argument("invalid IPv4 address: " + ip);
    }
}

InetAddress::InetAddress(const sockaddr_in& address) noexcept : address_(address) {}

std::string InetAddress::Ip() const {
    char text[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &address_.sin_addr, text, sizeof(text)) == nullptr) {
        throw std::runtime_error("inet_ntop failed");
    }
    return text;
}

std::uint16_t InetAddress::Port() const noexcept {
    return ntohs(address_.sin_port);
}

std::string InetAddress::ToString() const {
    return Ip() + ":" + std::to_string(Port());
}

}  // namespace net
}  // namespace l4lb

