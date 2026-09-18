#include "Test.h"

#include "lb/Connector.h"
#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"
#include "net/TimerId.h"

#include <cerrno>
#include <chrono>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <vector>

using l4lb::lb::Connector;
using l4lb::net::Acceptor;
using l4lb::net::EventLoop;
using l4lb::net::InetAddress;
using l4lb::net::Socket;
using l4lb::net::TcpConnection;
using l4lb::net::TimerId;

namespace {
// 填满本机 listen 队列但不 accept，确定性地制造 pending connect。
// 不依赖公网路由、防火墙或 192.0.2.0/24 在当前网络里的行为。
struct FullBacklog {
    Socket listener = Socket::CreateTcpNonBlocking();
    std::vector<Socket> clients;
    FullBacklog() {
        listener.Bind({"127.0.0.1", 0});
        listener.Listen(1);
        const auto address = listener.LocalAddress().SockAddr();
        for (int i = 0; i < 2; ++i) {
            Socket client = Socket::CreateTcpNonBlocking();
            const int result = ::connect(client.Fd(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
            L4LB_REQUIRE(result == 0 || errno == EINPROGRESS);
            pollfd event{client.Fd(), POLLOUT, 0};
            L4LB_REQUIRE(::poll(&event, 1, 1000) == 1);
            L4LB_REQUIRE(client.GetSocketError() == 0);
            clients.push_back(std::move(client));
        }
    }
};
}

L4LB_TEST(ConnectorCompletesNonBlockingConnectionAndTransfersSocket) {
    EventLoop loop;
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::shared_ptr<TcpConnection> backend_connection;
    std::unique_ptr<Socket> accepted;
    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        accepted.reset(new Socket(std::move(socket)));
    });
    acceptor.Start();

    std::shared_ptr<Connector> connector(new Connector(
        &loop, acceptor.ListenAddress(), std::chrono::milliseconds(500)));
    connector->SetSuccessCallback(
        [&](const std::shared_ptr<Connector>& value, Socket socket) {
            L4LB_REQUIRE(value->Loop() == &loop);
            backend_connection.reset(new TcpConnection(&loop, std::move(socket), "backend"));
            L4LB_REQUIRE(backend_connection->Loop() == &loop);
            backend_connection->Establish();
            loop.Quit();
        });
    connector->SetErrorCallback(
        [&](const std::shared_ptr<Connector>&, const std::error_code&) { loop.Quit(); });
    connector->Start();
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] { loop.Quit(); });
    loop.Loop();

    L4LB_REQUIRE(connector->GetState() == Connector::State::kSucceeded);
    L4LB_REQUIRE(accepted && accepted->IsValid());
    L4LB_REQUIRE(backend_connection && backend_connection->GetState() ==
                                           TcpConnection::State::kConnected);
    watchdog.Cancel();
    backend_connection->ForceClose();
    backend_connection.reset();
    acceptor.Stop();
}

L4LB_TEST(ConnectorReportsConnectionRefused) {
    EventLoop loop;
    InetAddress unused("127.0.0.1", 0);
    {
        Socket temporary = Socket::CreateTcpNonBlocking();
        temporary.Bind(unused);
        unused = temporary.LocalAddress();
    }

    int error_number = 0;
    std::shared_ptr<Connector> connector(
        new Connector(&loop, unused, std::chrono::milliseconds(500)));
    connector->SetSuccessCallback(
        [&](const std::shared_ptr<Connector>&, Socket) { loop.Quit(); });
    connector->SetErrorCallback(
        [&](const std::shared_ptr<Connector>&, const std::error_code& error) {
            error_number = error.value();
            loop.Quit();
        });
    connector->Start();
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] { loop.Quit(); });
    loop.Loop();
    L4LB_REQUIRE(connector->GetState() == Connector::State::kFailed);
    L4LB_REQUIRE(error_number == ECONNREFUSED);
    watchdog.Cancel();
}

L4LB_TEST(ConnectorCancellationIsIdempotent) {
    EventLoop loop;
    FullBacklog blocked;
    int callbacks = 0;
    int error_number = 0;
    std::shared_ptr<Connector> connector(new Connector(
        &loop, blocked.listener.LocalAddress(), std::chrono::seconds(1)));
    connector->SetErrorCallback(
        [&](const std::shared_ptr<Connector>&, const std::error_code& error) {
            ++callbacks;
            error_number = error.value();
        });
    connector->Start();
    connector->Cancel();
    connector->Cancel();
    TimerId stopper = loop.RunAfter(std::chrono::milliseconds(5), [&] { loop.Quit(); });
    loop.Loop();
    L4LB_REQUIRE(connector->GetState() == Connector::State::kCancelled);
    L4LB_REQUIRE(callbacks == 1);
    L4LB_REQUIRE(error_number == ECANCELED);
    stopper.Cancel();
}

L4LB_TEST(ConnectorAppliesConnectionTimeout) {
    EventLoop loop;
    FullBacklog blocked;
    int error_number = 0;
    std::shared_ptr<Connector> connector(new Connector(
        &loop, blocked.listener.LocalAddress(), std::chrono::milliseconds(1)));
    connector->SetErrorCallback(
        [&](const std::shared_ptr<Connector>&, const std::error_code& error) {
            error_number = error.value();
            loop.Quit();
        });
    connector->Start();
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] { loop.Quit(); });
    loop.Loop();
    L4LB_REQUIRE(connector->GetState() == Connector::State::kFailed);
    L4LB_REQUIRE(error_number == ETIMEDOUT);
    watchdog.Cancel();
}
