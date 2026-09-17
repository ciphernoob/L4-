#include "Test.h"

#include "net/EventLoop.h"
#include "net/TcpConnection.h"
#include "net/TimerId.h"
#include "net/UniqueFd.h"

#include <chrono>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using l4lb::net::EventLoop;
using l4lb::net::Socket;
using l4lb::net::TcpConnection;
using l4lb::net::TimerId;
using l4lb::net::UniqueFd;

namespace {

std::pair<Socket, UniqueFd> MakeSocketPair() {
    int descriptors[2]{};
    L4LB_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                              descriptors) == 0);
    return std::make_pair(Socket(UniqueFd(descriptors[0])), UniqueFd(descriptors[1]));
}

}  // namespace

L4LB_TEST(TcpConnectionEchoesOpaqueBinaryBytes) {
    EventLoop loop;
    auto pair = MakeSocketPair();
    UniqueFd peer = std::move(pair.second);
    std::shared_ptr<TcpConnection> connection(
        new TcpConnection(&loop, std::move(pair.first), "socketpair"));
    connection->SetMessageCallback(
        [](const std::shared_ptr<TcpConnection>& value, l4lb::net::Buffer* input) {
            value->Send(input->RetrieveAllAsString());
        });
    connection->Establish();

    const std::string payload("a\0b\0c", 5);
    L4LB_REQUIRE(::write(peer.Get(), payload.data(), payload.size()) ==
                 static_cast<ssize_t>(payload.size()));

    std::string received;
    TimerId check = loop.RunAfter(std::chrono::milliseconds(10), [&] {
        char bytes[32]{};
        const ssize_t result = ::read(peer.Get(), bytes, sizeof(bytes));
        if (result > 0) {
            received.assign(bytes, static_cast<std::size_t>(result));
        }
        loop.Quit();
    });
    loop.Loop();
    L4LB_REQUIRE(received == payload);
    check.Cancel();
    connection->ForceClose();
    connection.reset();
}

L4LB_TEST(TcpConnectionReportsEofWithoutDroppingReverseDirection) {
    EventLoop loop;
    auto pair = MakeSocketPair();
    UniqueFd peer = std::move(pair.second);
    std::shared_ptr<TcpConnection> connection(
        new TcpConnection(&loop, std::move(pair.first), "half-close"));
    bool eof = false;
    connection->SetMessageCallback(
        [](const std::shared_ptr<TcpConnection>&, l4lb::net::Buffer* input) {
            input->RetrieveAll();
        });
    connection->SetEofCallback([&](const std::shared_ptr<TcpConnection>& value) {
        eof = true;
        value->Send("response");
        value->ShutdownWrite();
    });
    connection->Establish();
    L4LB_REQUIRE(::write(peer.Get(), "request", 7) == 7);
    L4LB_REQUIRE(::shutdown(peer.Get(), SHUT_WR) == 0);

    std::string response;
    TimerId check = loop.RunAfter(std::chrono::milliseconds(10), [&] {
        char bytes[32]{};
        const ssize_t result = ::read(peer.Get(), bytes, sizeof(bytes));
        if (result > 0) {
            response.assign(bytes, static_cast<std::size_t>(result));
        }
        loop.Quit();
    });
    loop.Loop();
    L4LB_REQUIRE(eof);
    L4LB_REQUIRE(response == "response");
    check.Cancel();
    connection->ForceClose();
    connection.reset();
}

