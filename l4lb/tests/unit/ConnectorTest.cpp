#include "Test.h"

#include "lb/Connector.h"
#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"
#include "net/TimerId.h"

#include <cerrno>
#include <chrono>
#include <memory>

using l4lb::lb::Connector;
using l4lb::net::Acceptor;
using l4lb::net::EventLoop;
using l4lb::net::InetAddress;
using l4lb::net::Socket;
using l4lb::net::TcpConnection;
using l4lb::net::TimerId;

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
    int callbacks = 0;
    int error_number = 0;
    std::shared_ptr<Connector> connector(new Connector(
        &loop, InetAddress("192.0.2.1", 65000), std::chrono::seconds(1)));
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
    int error_number = 0;
    std::shared_ptr<Connector> connector(new Connector(
        &loop, InetAddress("192.0.2.1", 65000), std::chrono::milliseconds(1)));
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

