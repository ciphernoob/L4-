#include "Test.h"

#include "net/InetAddress.h"
#include "net/Socket.h"

#include <stdexcept>
#include <sys/socket.h>
#include <utility>
#include <unistd.h>

using l4lb::net::InetAddress;
using l4lb::net::Socket;

L4LB_TEST(InetAddressRoundTripsIpv4AndPort) {
    const InetAddress address("127.0.0.1", 9000);
    L4LB_REQUIRE(address.Ip() == "127.0.0.1");
    L4LB_REQUIRE(address.Port() == 9000);
    L4LB_REQUIRE(address.ToString() == "127.0.0.1:9000");
}

L4LB_TEST(InetAddressRejectsInvalidInput) {
    bool threw = false;
    try {
        const InetAddress ignored("not-an-ip", 80);
        (void)ignored;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    L4LB_REQUIRE(threw);
}

L4LB_TEST(SocketMovesUniqueOwnership) {
    Socket original = Socket::CreateTcpNonBlocking();
    const int fd = original.Fd();
    Socket moved(std::move(original));
    L4LB_REQUIRE(!original.IsValid());
    L4LB_REQUIRE(moved.IsValid());
    L4LB_REQUIRE(moved.Fd() == fd);
}

L4LB_TEST(SocketBindsListensAcceptsAndReportsErrors) {
    Socket listener = Socket::CreateTcpNonBlocking();
    listener.SetReuseAddress(true);
    listener.Bind(InetAddress("127.0.0.1", 0));
    listener.Listen();
    const InetAddress local = listener.LocalAddress();
    L4LB_REQUIRE(local.Port() != 0);

    const int client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    L4LB_REQUIRE(client >= 0);
    const sockaddr_in address = local.SockAddr();
    L4LB_REQUIRE(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

    Socket accepted = listener.Accept();
    L4LB_REQUIRE(accepted.IsValid());
    L4LB_REQUIRE(accepted.GetSocketError() == 0);
    accepted.ShutdownWrite();
    ::close(client);
}
