#include "Test.h"

#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/UniqueFd.h"

#include <cerrno>
#include <chrono>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <vector>

using l4lb::net::Acceptor;
using l4lb::net::EventLoop;
using l4lb::net::InetAddress;
using l4lb::net::Socket;
using l4lb::net::UniqueFd;

namespace {

void VerifyBurstAccept(bool edge_triggered) {
    constexpr int kConnectionCount = 32;
    EventLoop loop;
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0), edge_triggered);
    std::vector<Socket> accepted;
    accepted.reserve(kConnectionCount);
    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        accepted.push_back(std::move(socket));
        if (accepted.size() == kConnectionCount) {
            loop.Quit();
        }
    });
    acceptor.Start();
    auto watchdog = loop.RunAfter(std::chrono::seconds(2), [&] { loop.Quit(); });

    const sockaddr_in address = acceptor.ListenAddress().SockAddr();
    std::thread clients([address] {
        std::vector<UniqueFd> sockets;
        sockets.reserve(kConnectionCount);
        for (int index = 0; index < kConnectionCount; ++index) {
            UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
            L4LB_REQUIRE(socket.IsValid());
            int result;
            do {
                result = ::connect(socket.Get(), reinterpret_cast<const sockaddr*>(&address),
                                   sizeof(address));
            } while (result < 0 && errno == EINTR);
            L4LB_REQUIRE(result == 0);
            sockets.push_back(std::move(socket));
        }
    });

    loop.Loop();
    clients.join();
    L4LB_REQUIRE(accepted.size() == kConnectionCount);
    watchdog.Cancel();
    acceptor.Stop();
}

}  // namespace

L4LB_TEST(AcceptorDrainsBurstInLevelTriggeredMode) {
    VerifyBurstAccept(false);
}

L4LB_TEST(AcceptorDrainsBurstInEdgeTriggeredMode) {
    VerifyBurstAccept(true);
}
