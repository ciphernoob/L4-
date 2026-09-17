#include "Test.h"

#include "lb/ProxySession.h"
#include "lb/SessionMap.h"
#include "lb/BackendPool.h"
#include "lb/LoadBalancer.h"
#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"
#include "net/TimerId.h"
#include "net/UniqueFd.h"

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using l4lb::lb::ProxySession;
using l4lb::lb::SessionMap;
using l4lb::lb::BackendPool;
using l4lb::lb::RoundRobinLoadBalancer;
using l4lb::lb::SessionCounters;
using l4lb::net::Acceptor;
using l4lb::net::EventLoop;
using l4lb::net::InetAddress;
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

std::string BinaryPayload(std::size_t size, unsigned int salt) {
    std::string result(size, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index] = static_cast<char>((index * 131U + salt) & 0xffU);
    }
    return result;
}

void WriteAll(int fd, const std::string& data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t result = ::write(fd, data.data() + written, data.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        L4LB_REQUIRE(false);
    }
}

void TryWrite(int fd, const std::string& data) {
    const ssize_t result = ::write(fd, data.data(), data.size());
    L4LB_REQUIRE(result >= 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
                 errno == EINTR);
}

void Drain(int fd) {
    char bytes[64 * 1024];
    while (true) {
        const ssize_t result = ::read(fd, bytes, sizeof(bytes));
        if (result > 0 || (result < 0 && errno == EINTR)) {
            continue;
        }
        L4LB_REQUIRE(result == 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
}

}  // namespace

L4LB_TEST(SessionMapOwnsSessionAndCloseIsIdempotent) {
    EventLoop loop;
    SessionMap sessions(&loop);
    auto pair = MakeSocketPair();
    UniqueFd client = std::move(pair.second);
    std::shared_ptr<TcpConnection> frontend(
        new TcpConnection(&loop, std::move(pair.first), "frontend-idempotent"));
    frontend->Establish();

    ProxySession::Options options{InetAddress("127.0.0.1", 1),
                                  std::chrono::milliseconds(50), 1024, 4096};
    std::shared_ptr<ProxySession> session = sessions.Add(frontend, options);
    L4LB_REQUIRE(session->GetState() == ProxySession::State::kAccepted);
    L4LB_REQUIRE(sessions.Size() == 1);
    session->Close();
    session->Close();
    L4LB_REQUIRE(session->GetState() == ProxySession::State::kClosed);
    L4LB_REQUIRE(sessions.Size() == 0);
    frontend.reset();
    client.Reset();
}

L4LB_TEST(ProxySessionForwardsLargeOpaqueStreamsAndPreservesHalfClose) {
    EventLoop loop;
    SessionMap sessions(&loop);
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::shared_ptr<TcpConnection> backend_peer;
    TimerId response_timer;
    std::string backend_received;
    const std::string request = BinaryPayload(96 * 1024, 17);
    const std::string response = BinaryPayload(144 * 1024, 93);

    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        backend_peer.reset(new TcpConnection(&loop, std::move(socket), "backend-peer"));
        backend_peer->SetMessageCallback(
            [&](const std::shared_ptr<TcpConnection>&, l4lb::net::Buffer* input) {
                backend_received.append(input->Peek(), input->ReadableBytes());
                input->RetrieveAll();
            });
        backend_peer->SetEofCallback([&](const std::shared_ptr<TcpConnection>& connection) {
            response_timer =
                loop.RunAfter(std::chrono::milliseconds(20), [connection, &response] {
                connection->Send(response);
                connection->ShutdownWrite();
            });
        });
        backend_peer->Establish();
    });
    acceptor.Start();

    auto pair = MakeSocketPair();
    UniqueFd client = std::move(pair.second);
    std::shared_ptr<TcpConnection> frontend(
        new TcpConnection(&loop, std::move(pair.first), "frontend-proxy"));
    frontend->Establish();
    ProxySession::Options options{acceptor.ListenAddress(),
                                  std::chrono::milliseconds(500), 32 * 1024,
                                  128 * 1024};
    std::shared_ptr<ProxySession> session = sessions.Add(frontend, options);
    session->Start();
    L4LB_REQUIRE(session->GetState() == ProxySession::State::kConnecting);

    WriteAll(client.Get(), request);
    L4LB_REQUIRE(::shutdown(client.Get(), SHUT_WR) == 0);

    bool observed_draining = false;
    bool received_eof = false;
    bool timed_out = false;
    std::string client_received;
    TimerId poller;
    poller = loop.RunEvery(std::chrono::milliseconds(1), [&] {
        if (session->GetState() == ProxySession::State::kDraining) {
            observed_draining = true;
        }
        char bytes[16 * 1024];
        while (true) {
            const ssize_t count = ::read(client.Get(), bytes, sizeof(bytes));
            if (count > 0) {
                client_received.append(bytes, static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0) {
                received_eof = true;
                poller.Cancel();
                loop.Quit();
            }
            break;
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] {
        timed_out = true;
        session->Close();
        if (backend_peer &&
            backend_peer->GetState() != TcpConnection::State::kDisconnected) {
            backend_peer->ForceClose();
        }
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(observed_draining);
    L4LB_REQUIRE(received_eof);
    L4LB_REQUIRE(backend_received == request);
    L4LB_REQUIRE(client_received == response);
    L4LB_REQUIRE(session->PeakPendingFrontendBytes() == request.size());
    L4LB_REQUIRE(session->PeakPendingFrontendBytes() <= options.high_watermark);
    L4LB_REQUIRE(session->GetState() == ProxySession::State::kClosed);
    L4LB_REQUIRE(session->Reason() == ProxySession::CloseReason::kCompleted);
    L4LB_REQUIRE(sessions.Size() == 0);

    watchdog.Cancel();
    poller.Cancel();
    response_timer.Cancel();
    if (backend_peer && backend_peer->GetState() != TcpConnection::State::kDisconnected) {
        backend_peer->ForceClose();
    }
    backend_peer.reset();
    frontend.reset();
    acceptor.Stop();
}

L4LB_TEST(ProxySessionAppliesAndReleasesBackpressureInBothDirections) {
    EventLoop loop;
    SessionMap sessions(&loop);
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::unique_ptr<Socket> backend_peer;
    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        backend_peer.reset(new Socket(std::move(socket)));
    });
    acceptor.Start();

    auto pair = MakeSocketPair();
    UniqueFd client = std::move(pair.second);
    std::shared_ptr<TcpConnection> frontend(
        new TcpConnection(&loop, std::move(pair.first), "frontend-backpressure"));
    frontend->Establish();
    ProxySession::Options options{acceptor.ListenAddress(),
                                  std::chrono::milliseconds(500), 4 * 1024,
                                  8 * 1024};
    std::shared_ptr<ProxySession> session = sessions.Add(frontend, options);
    session->Start();

    enum class Phase {
        kWaitForConnection,
        kFillBackend,
        kDrainBackend,
        kFillClient,
        kDrainClient,
        kDone,
    };
    Phase phase = Phase::kWaitForConnection;
    bool backend_paused = false;
    bool backend_resumed = false;
    bool client_paused = false;
    bool client_resumed = false;
    bool timed_out = false;
    std::size_t maximum_backend_buffer = 0;
    std::size_t maximum_client_buffer = 0;
    const std::string pressure(16 * 1024, 'p');

    TimerId driver;
    driver = loop.RunEvery(std::chrono::milliseconds(1), [&] {
        if (phase == Phase::kWaitForConnection && backend_peer &&
            session->BackendConnection()) {
            const int send_buffer = 4096;
            L4LB_REQUIRE(::setsockopt(session->BackendConnection()->Fd(), SOL_SOCKET,
                                      SO_SNDBUF, &send_buffer,
                                      sizeof(send_buffer)) == 0);
            L4LB_REQUIRE(::setsockopt(session->Frontend()->Fd(), SOL_SOCKET, SO_SNDBUF,
                                      &send_buffer, sizeof(send_buffer)) == 0);
            phase = Phase::kFillBackend;
        }

        if (session->BackendConnection()) {
            maximum_backend_buffer =
                std::max(maximum_backend_buffer,
                         session->BackendConnection()->OutputBufferBytes());
            maximum_client_buffer =
                std::max(maximum_client_buffer, session->Frontend()->OutputBufferBytes());
        }

        if (phase == Phase::kFillBackend) {
            TryWrite(client.Get(), pressure);
            if (session->BackendConnection()->OutputBufferBytes() >=
                    options.high_watermark &&
                !session->Frontend()->IsReading()) {
                backend_paused = true;
                phase = Phase::kDrainBackend;
            }
        } else if (phase == Phase::kDrainBackend) {
            Drain(backend_peer->Fd());
            if (session->Frontend()->IsReading() &&
                session->BackendConnection()->OutputBufferBytes() <=
                    options.low_watermark) {
                backend_resumed = true;
                phase = Phase::kFillClient;
            }
        } else if (phase == Phase::kFillClient) {
            TryWrite(backend_peer->Fd(), pressure);
            if (session->Frontend()->OutputBufferBytes() >= options.high_watermark &&
                !session->BackendConnection()->IsReading()) {
                client_paused = true;
                phase = Phase::kDrainClient;
            }
        } else if (phase == Phase::kDrainClient) {
            Drain(client.Get());
            if (session->BackendConnection()->IsReading() &&
                session->Frontend()->OutputBufferBytes() <= options.low_watermark) {
                client_resumed = true;
                phase = Phase::kDone;
                session->Close();
                driver.Cancel();
                loop.Quit();
            }
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(5), [&] {
        timed_out = true;
        session->Close();
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(backend_paused && backend_resumed);
    L4LB_REQUIRE(client_paused && client_resumed);
    L4LB_REQUIRE(maximum_backend_buffer <= options.high_watermark);
    L4LB_REQUIRE(maximum_client_buffer <= options.high_watermark);
    L4LB_REQUIRE(sessions.Size() == 0);

    watchdog.Cancel();
    driver.Cancel();
    backend_peer.reset();
    frontend.reset();
    acceptor.Stop();
}

L4LB_TEST(ProxySessionRetriesOnlyUnusedBackendsAndKeepsBufferedData) {
    EventLoop loop;
    InetAddress refused("127.0.0.1", 0);
    {
        Socket reservation = Socket::CreateTcpNonBlocking();
        reservation.Bind(refused);
        refused = reservation.LocalAddress();
    }
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::unique_ptr<Socket> backend_peer;
    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        backend_peer.reset(new Socket(std::move(socket)));
    });
    acceptor.Start();

    std::shared_ptr<BackendPool> pool(new BackendPool(
        {{"refused", refused.Ip(), refused.Port(), 1},
         {"working", acceptor.ListenAddress().Ip(), acceptor.ListenAddress().Port(), 1}}));
    std::shared_ptr<l4lb::lb::LoadBalancer> balancer(new RoundRobinLoadBalancer());

    SessionMap sessions(&loop);
    auto pair = MakeSocketPair();
    UniqueFd client = std::move(pair.second);
    std::shared_ptr<TcpConnection> frontend(
        new TcpConnection(&loop, std::move(pair.first), "frontend-retry"));
    frontend->Establish();
    ProxySession::Options options(refused, std::chrono::milliseconds(200), 4096,
                                  16384);
    options.backend_pool = pool;
    options.load_balancer = balancer;
    options.max_connect_retries = 1;
    std::shared_ptr<ProxySession> session = sessions.Add(frontend, options);
    session->Start();

    const std::string payload("before\0connect", 14);
    WriteAll(client.Get(), payload);
    std::string received;
    std::string selected_id;
    bool timed_out = false;
    TimerId poller;
    poller = loop.RunEvery(std::chrono::milliseconds(1), [&] {
        if (!backend_peer) {
            return;
        }
        char bytes[64];
        const ssize_t count = ::read(backend_peer->Fd(), bytes, sizeof(bytes));
        if (count > 0) {
            received.append(bytes, static_cast<std::size_t>(count));
        }
        if (received == payload) {
            selected_id = session->SelectedBackend()->Id();
            poller.Cancel();
            session->Close();
            loop.Quit();
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] {
        timed_out = true;
        session->Close();
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(received == payload);
    L4LB_REQUIRE(session->ConnectAttempts() == 2);
    L4LB_REQUIRE(selected_id == "working");
    L4LB_REQUIRE(pool->Find("refused")->ActiveSessions() == 0);
    L4LB_REQUIRE(pool->Find("working")->ActiveSessions() == 0);
    watchdog.Cancel();
    poller.Cancel();
    backend_peer.reset();
    frontend.reset();
    acceptor.Stop();
}

L4LB_TEST(ProxySessionClosesIdleEstablishedConnectionAndCountsTimeout) {
    EventLoop loop;
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::unique_ptr<Socket> backend_peer;
    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        backend_peer.reset(new Socket(std::move(socket)));
    });
    acceptor.Start();

    SessionMap sessions(&loop);
    auto pair = MakeSocketPair();
    UniqueFd client = std::move(pair.second);
    std::shared_ptr<TcpConnection> frontend(
        new TcpConnection(&loop, std::move(pair.first), "frontend-idle"));
    frontend->Establish();
    std::shared_ptr<SessionCounters> counters(new SessionCounters());
    ProxySession::Options options(acceptor.ListenAddress(),
                                  std::chrono::milliseconds(200), 4096, 16384);
    options.counters = counters;
    options.idle_timeout = std::chrono::milliseconds(20);
    std::shared_ptr<ProxySession> session = sessions.Add(frontend, options);
    session->Start();

    bool timed_out = false;
    TimerId observer;
    observer = loop.RunEvery(std::chrono::milliseconds(2), [&] {
        if (session->GetState() == ProxySession::State::kClosed) {
            observer.Cancel();
            loop.Quit();
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(1), [&] {
        timed_out = true;
        session->Close();
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(session->Reason() == ProxySession::CloseReason::kIdleTimeout);
    L4LB_REQUIRE(counters->idle_timeouts.load() == 1);
    L4LB_REQUIRE(sessions.Size() == 0);
    observer.Cancel();
    watchdog.Cancel();
    backend_peer.reset();
    frontend.reset();
    acceptor.Stop();
}

L4LB_TEST(ProxySessionRecordsBackendConnectionTimeout) {
    EventLoop loop;
    SessionMap sessions(&loop);
    auto pair = MakeSocketPair();
    UniqueFd client = std::move(pair.second);
    std::shared_ptr<TcpConnection> frontend(
        new TcpConnection(&loop, std::move(pair.first), "frontend-timeout"));
    frontend->Establish();
    std::shared_ptr<SessionCounters> counters(new SessionCounters());
    ProxySession::Options options(InetAddress("192.0.2.1", 65000),
                                  std::chrono::milliseconds(1), 4096, 16384);
    options.counters = counters;
    std::shared_ptr<ProxySession> session = sessions.Add(frontend, options);
    session->Start();

    TimerId watchdog = loop.RunAfter(std::chrono::seconds(1), [&] {
        session->Close();
        loop.Quit();
    });
    TimerId observer;
    observer = loop.RunEvery(std::chrono::milliseconds(1), [&] {
        if (session->GetState() == ProxySession::State::kClosed) {
            observer.Cancel();
            loop.Quit();
        }
    });
    loop.Loop();

    L4LB_REQUIRE(session->Reason() == ProxySession::CloseReason::kConnectTimeout);
    L4LB_REQUIRE(counters->connect_timeouts.load() == 1);
    L4LB_REQUIRE(counters->connect_failures.load() == 1);
    watchdog.Cancel();
    observer.Cancel();
    frontend.reset();
}

L4LB_TEST(ProxySessionHandlesBackendResetAndReleasesOwnershipExactlyOnce) {
    EventLoop loop;
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0), true);
    std::unique_ptr<Socket> backend_peer;
    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        backend_peer.reset(new Socket(std::move(socket)));
    });
    acceptor.Start();

    std::shared_ptr<BackendPool> pool(new BackendPool(
        {{"resetting", acceptor.ListenAddress().Ip(),
          acceptor.ListenAddress().Port(), 1}}));
    std::shared_ptr<l4lb::lb::LoadBalancer> balancer(
        new RoundRobinLoadBalancer());
    std::shared_ptr<SessionCounters> counters(new SessionCounters());
    SessionMap sessions(&loop);
    auto pair = MakeSocketPair();
    UniqueFd client = std::move(pair.second);
    std::shared_ptr<TcpConnection> frontend(
        new TcpConnection(&loop, std::move(pair.first), "frontend-reset"));
    frontend->Establish();
    ProxySession::Options options(acceptor.ListenAddress(),
                                  std::chrono::milliseconds(200), 4096, 16384);
    options.backend_pool = pool;
    options.load_balancer = balancer;
    options.counters = counters;
    std::shared_ptr<ProxySession> session = sessions.Add(frontend, options);
    const std::weak_ptr<ProxySession> weak_session = session;
    session->Start();

    bool reset_sent = false;
    bool timed_out = false;
    TimerId driver;
    driver = loop.RunEvery(std::chrono::milliseconds(1), [&] {
        if (!reset_sent && backend_peer && session &&
            session->GetState() == ProxySession::State::kEstablished) {
            const linger reset_linger{1, 0};
            L4LB_REQUIRE(::setsockopt(backend_peer->Fd(), SOL_SOCKET, SO_LINGER,
                                      &reset_linger, sizeof(reset_linger)) == 0);
            backend_peer->Close();
            reset_sent = true;
            return;
        }
        if (reset_sent && session &&
            session->GetState() == ProxySession::State::kClosed) {
            L4LB_REQUIRE(session->Reason() == ProxySession::CloseReason::kPeerError);
            session->Close();
            session->Close();
            session.reset();
            frontend.reset();
            loop.QueueInLoop([&] {
                L4LB_REQUIRE(weak_session.expired());
                L4LB_REQUIRE(sessions.Size() == 0);
                L4LB_REQUIRE(pool->Find("resetting")->ActiveSessions() == 0);
                L4LB_REQUIRE(counters->current_sessions.load() == 0);
                driver.Cancel();
                loop.Quit();
            });
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] {
        timed_out = true;
        if (session) {
            session->Close();
        }
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(reset_sent);
    watchdog.Cancel();
    driver.Cancel();
    backend_peer.reset();
    client.Reset();
    acceptor.Stop();
}
