#include "Test.h"

#include "app/L4ProxyServer.h"
#include "config/Config.h"
#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"
#include "net/TimerId.h"
#include "net/UniqueFd.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using l4lb::app::L4ProxyServer;
using l4lb::config::Algorithm;
using l4lb::config::HealthCheckConfig;
using l4lb::config::ServerConfig;
using l4lb::lb::BackendConfig;
using l4lb::net::Acceptor;
using l4lb::net::EventLoop;
using l4lb::net::InetAddress;
using l4lb::net::Socket;
using l4lb::net::TcpConnection;
using l4lb::net::TimerId;
using l4lb::net::UniqueFd;

namespace {

UniqueFd ConnectBlocking(const InetAddress& address) {
    UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    L4LB_REQUIRE(fd.IsValid());
    const sockaddr_in& target = address.SockAddr();
    L4LB_REQUIRE(::connect(fd.Get(), reinterpret_cast<const sockaddr*>(&target),
                           sizeof(target)) == 0);
    const int flags = ::fcntl(fd.Get(), F_GETFL, 0);
    L4LB_REQUIRE(flags >= 0);
    L4LB_REQUIRE(::fcntl(fd.Get(), F_SETFL, flags | O_NONBLOCK) == 0);
    return fd;
}

void ReadAvailable(int fd, std::string* output) {
    char bytes[16 * 1024];
    while (true) {
        const ssize_t count = ::read(fd, bytes, sizeof(bytes));
        if (count > 0) {
            output->append(bytes, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        L4LB_REQUIRE(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
}

void RunDistributionScenario(Algorithm algorithm) {
    EventLoop loop;
    Acceptor first(&loop, InetAddress("127.0.0.1", 0), true);
    Acceptor second(&loop, InetAddress("127.0.0.1", 0), true);
    std::vector<std::unique_ptr<Socket>> backend_peers;
    const auto keep = [&](Socket socket, const InetAddress&) {
        backend_peers.emplace_back(new Socket(std::move(socket)));
    };
    first.SetNewConnectionCallback(keep);
    second.SetNewConnectionCallback(keep);
    first.Start();
    second.Start();
    HealthCheckConfig health;
    health.interval = std::chrono::milliseconds(1000);
    health.timeout = std::chrono::milliseconds(100);
    ServerConfig config{InetAddress("127.0.0.1", 0),
                        InetAddress("127.0.0.1", 0),
                        2,
                        algorithm,
                        std::vector<BackendConfig>{
                            {"first", "127.0.0.1", first.ListenAddress().Port(), 1},
                            {"second", "127.0.0.1", second.ListenAddress().Port(), 1}},
                        std::chrono::milliseconds(500),
                        std::chrono::milliseconds(5000),
                        4096,
                        16384,
                        health,
                        0,
                        std::chrono::milliseconds(100)};
    L4ProxyServer server(&loop, config);
    server.Start();
    std::vector<UniqueFd> clients;
    for (int index = 0; index < 6; ++index) {
        clients.push_back(ConnectBlocking(server.ListenAddress()));
    }

    std::size_t first_active = 0;
    std::size_t second_active = 0;
    bool timed_out = false;
    TimerId observer;
    observer = loop.RunEvery(std::chrono::milliseconds(2), [&] {
        if (server.Counters()->total_sessions.load() == 6 &&
            server.Counters()->current_sessions.load() == 6) {
            first_active = server.Backends()->Find("first")->ActiveSessions();
            second_active = server.Backends()->Find("second")->ActiveSessions();
            if (first_active + second_active == 6) {
                observer.Cancel();
                server.Stop();
                loop.Quit();
            }
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] {
        timed_out = true;
        server.Stop();
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(first_active == 3);
    L4LB_REQUIRE(second_active == 3);
    L4LB_REQUIRE(server.Backends()->Find("first")->ActiveSessions() == 0);
    L4LB_REQUIRE(server.Backends()->Find("second")->ActiveSessions() == 0);
    observer.Cancel();
    watchdog.Cancel();
    clients.clear();
    backend_peers.clear();
    first.Stop();
    second.Stop();
}

}  // namespace

L4LB_TEST(L4ProxyServerDistributesAcrossWorkersWithBothAlgorithms) {
    RunDistributionScenario(Algorithm::kRoundRobin);
    RunDistributionScenario(Algorithm::kLeastConnections);
}

L4LB_TEST(L4ProxyServerRunsDataAndAdminFlowAcrossWorkers) {
    EventLoop loop;
    Acceptor backend_acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::vector<std::shared_ptr<TcpConnection>> backend_connections;
    backend_acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        std::shared_ptr<TcpConnection> connection(
            new TcpConnection(&loop, std::move(socket), "test-backend"));
        connection->SetMessageCallback(
            [](const std::shared_ptr<TcpConnection>& value, l4lb::net::Buffer* input) {
                value->Send(input->Peek(), input->ReadableBytes());
                input->RetrieveAll();
            });
        connection->SetEofCallback(
            [](const std::shared_ptr<TcpConnection>& value) { value->ShutdownWrite(); });
        connection->Establish();
        backend_connections.push_back(connection);
    });
    backend_acceptor.Start();

    ServerConfig config{InetAddress("127.0.0.1", 0),
                        InetAddress("127.0.0.1", 0),
                        2,
                        Algorithm::kRoundRobin,
                        std::vector<BackendConfig>{{"echo", "127.0.0.1",
                                                    backend_acceptor.ListenAddress().Port(), 1}},
                        std::chrono::milliseconds(500),
                        std::chrono::milliseconds(5000),
                        4096,
                        16384,
                        HealthCheckConfig{},
                        1,
                        std::chrono::milliseconds(1000)};
    L4ProxyServer server(&loop, config);
    server.Start();

    UniqueFd client = ConnectBlocking(server.ListenAddress());
    UniqueFd second_client = ConnectBlocking(server.ListenAddress());
    UniqueFd admin = ConnectBlocking(server.AdminAddress());
    const std::string payload("assembled\0flow", 14);
    const std::string second_payload("second-worker");
    L4LB_REQUIRE(::write(client.Get(), payload.data(), payload.size()) ==
                 static_cast<ssize_t>(payload.size()));
    L4LB_REQUIRE(::write(second_client.Get(), second_payload.data(),
                         second_payload.size()) ==
                 static_cast<ssize_t>(second_payload.size()));
    const std::string request = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
    L4LB_REQUIRE(::write(admin.Get(), request.data(), request.size()) ==
                 static_cast<ssize_t>(request.size()));

    std::string echoed;
    std::string second_echoed;
    std::string metrics;
    bool timed_out = false;
    TimerId poller;
    poller = loop.RunEvery(std::chrono::milliseconds(2), [&] {
        ReadAvailable(client.Get(), &echoed);
        ReadAvailable(second_client.Get(), &second_echoed);
        ReadAvailable(admin.Get(), &metrics);
        if (echoed == payload && second_echoed == second_payload &&
            metrics.find("HTTP/1.1 200 OK") != std::string::npos &&
            metrics.find("l4lb_sessions_total") != std::string::npos) {
            poller.Cancel();
            loop.Quit();
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(3), [&] {
        timed_out = true;
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(echoed == payload);
    L4LB_REQUIRE(second_echoed == second_payload);
    L4LB_REQUIRE(server.Counters()->total_sessions.load() == 2);
    L4LB_REQUIRE(server.Counters()->client_to_backend_bytes.load() ==
                 payload.size() + second_payload.size());
    L4LB_REQUIRE(server.Counters()->backend_to_client_bytes.load() ==
                 payload.size() + second_payload.size());

    client.Reset();
    second_client.Reset();
    admin.Reset();
    watchdog.Cancel();
    poller.Cancel();
    server.Stop();
    L4LB_REQUIRE(server.Counters()->current_sessions.load() == 0);
    for (const std::shared_ptr<TcpConnection>& connection : backend_connections) {
        if (connection->GetState() != TcpConnection::State::kDisconnected) {
            connection->ForceClose();
        }
    }
    backend_connections.clear();
    backend_acceptor.Stop();
}

L4LB_TEST(L4ProxyServerDrainsUntilSessionsFinishDuringShutdownGrace) {
    EventLoop loop;
    Acceptor backend_acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::vector<std::unique_ptr<Socket>> backend_peers;
    backend_acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        backend_peers.emplace_back(new Socket(std::move(socket)));
    });
    backend_acceptor.Start();

    HealthCheckConfig health;
    health.interval = std::chrono::milliseconds(1000);
    health.timeout = std::chrono::milliseconds(100);
    ServerConfig config{InetAddress("127.0.0.1", 0),
                        InetAddress("127.0.0.1", 0),
                        1,
                        Algorithm::kRoundRobin,
                        std::vector<BackendConfig>{{"hold", "127.0.0.1",
                                                    backend_acceptor.ListenAddress().Port(), 1}},
                        std::chrono::milliseconds(500),
                        std::chrono::milliseconds(5000),
                        4096,
                        16384,
                        health,
                        0,
                        std::chrono::milliseconds(500)};
    L4ProxyServer server(&loop, config);
    server.Start();
    UniqueFd client = ConnectBlocking(server.ListenAddress());
    L4LB_REQUIRE(::write(client.Get(), "x", 1) == 1);
    bool observed_draining = false;
    bool timed_out = false;
    TimerId begin = loop.RunAfter(std::chrono::milliseconds(20), [&] {
        L4LB_REQUIRE(server.Counters()->current_sessions.load() == 1);
        server.BeginShutdown();
    });
    TimerId finish = loop.RunAfter(std::chrono::milliseconds(50), [&] {
        observed_draining = server.IsStarted() &&
                            server.Counters()->current_sessions.load() == 1;
        client.Reset();
        backend_peers.clear();
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] {
        timed_out = true;
        server.Stop();
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(observed_draining);
    L4LB_REQUIRE(!server.IsStarted());
    L4LB_REQUIRE(server.Counters()->current_sessions.load() == 0);
    begin.Cancel();
    finish.Cancel();
    watchdog.Cancel();
    backend_acceptor.Stop();
}

L4LB_TEST(L4ProxyServerForcesRemainingSessionsAfterShutdownDeadline) {
    EventLoop loop;
    Acceptor backend_acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::vector<std::unique_ptr<Socket>> backend_peers;
    backend_acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        backend_peers.emplace_back(new Socket(std::move(socket)));
    });
    backend_acceptor.Start();
    HealthCheckConfig health;
    health.interval = std::chrono::milliseconds(1000);
    health.timeout = std::chrono::milliseconds(100);
    ServerConfig config{InetAddress("127.0.0.1", 0),
                        InetAddress("127.0.0.1", 0),
                        1,
                        Algorithm::kRoundRobin,
                        std::vector<BackendConfig>{{"hold", "127.0.0.1",
                                                    backend_acceptor.ListenAddress().Port(), 1}},
                        std::chrono::milliseconds(500),
                        std::chrono::milliseconds(5000),
                        4096,
                        16384,
                        health,
                        0,
                        std::chrono::milliseconds(50)};
    L4ProxyServer server(&loop, config);
    server.Start();
    UniqueFd client = ConnectBlocking(server.ListenAddress());
    L4LB_REQUIRE(::write(client.Get(), "x", 1) == 1);
    bool timed_out = false;
    TimerId begin = loop.RunAfter(std::chrono::milliseconds(20), [&] {
        L4LB_REQUIRE(server.Counters()->current_sessions.load() == 1);
        server.BeginShutdown();
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] {
        timed_out = true;
        server.Stop();
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(!server.IsStarted());
    L4LB_REQUIRE(server.Counters()->current_sessions.load() == 0);
    begin.Cancel();
    watchdog.Cancel();
    client.Reset();
    backend_peers.clear();
    backend_acceptor.Stop();
}
