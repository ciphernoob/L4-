#include "Test.h"

#include "lb/Backend.h"
#include "lb/BackendPool.h"
#include "lb/HealthChecker.h"
#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/TimerId.h"

#include <chrono>
#include <memory>
#include <vector>

using l4lb::lb::BackendLease;
using l4lb::lb::BackendPool;
using l4lb::lb::HealthChecker;
using l4lb::net::Acceptor;
using l4lb::net::EventLoop;
using l4lb::net::InetAddress;
using l4lb::net::Socket;
using l4lb::net::TimerId;

L4LB_TEST(HealthCheckerRemovesAndRestoresBackendWithoutTouchingExistingLease) {
    EventLoop loop;
    InetAddress address("127.0.0.1", 0);
    {
        Socket reservation = Socket::CreateTcpNonBlocking();
        reservation.Bind(address);
        address = reservation.LocalAddress();
    }
    std::shared_ptr<BackendPool> pool(new BackendPool(
        {{"recovering", address.Ip(), address.Port(), 1}}));
    BackendLease existing = BackendLease::Acquire(pool->Find("recovering"));
    HealthChecker checker(
        &loop, pool,
        HealthChecker::Options{std::chrono::milliseconds(20),
                               std::chrono::milliseconds(5), 2, 2});
    checker.Start();

    bool observed_down = false;
    bool recovered = false;
    bool timed_out = false;
    std::unique_ptr<Acceptor> recovered_server;
    TimerId observer;
    observer = loop.RunEvery(std::chrono::milliseconds(2), [&] {
        if (!observed_down && !pool->Find("recovering")->IsHealthy()) {
            observed_down = true;
            L4LB_REQUIRE(pool->Snapshot()->candidates.empty());
            L4LB_REQUIRE(existing.Get() != nullptr);
            L4LB_REQUIRE(existing.Get()->ActiveSessions() == 1);
            recovered_server.reset(new Acceptor(&loop, address, true));
            recovered_server->SetNewConnectionCallback(
                [](Socket, const InetAddress&) {});
            recovered_server->Start();
        } else if (observed_down && pool->Find("recovering")->IsHealthy()) {
            recovered = true;
            L4LB_REQUIRE(pool->Snapshot()->candidates.size() == 1);
            observer.Cancel();
            loop.Quit();
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] {
        timed_out = true;
        loop.Quit();
    });
    loop.Loop();

    L4LB_REQUIRE(!timed_out);
    L4LB_REQUIRE(observed_down && recovered);
    L4LB_REQUIRE(existing.Get()->ActiveSessions() == 1);
    checker.Stop();
    existing.Reset();
    L4LB_REQUIRE(pool->Find("recovering")->ActiveSessions() == 0);
    observer.Cancel();
    watchdog.Cancel();
    if (recovered_server) {
        recovered_server->Stop();
    }
}
