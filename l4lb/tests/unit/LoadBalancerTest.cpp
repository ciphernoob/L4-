#include "Test.h"

#include "lb/BackendPool.h"
#include "lb/LoadBalancer.h"

#include <atomic>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using l4lb::lb::BackendConfig;
using l4lb::lb::BackendLease;
using l4lb::lb::BackendPool;
using l4lb::lb::LeastConnectionsLoadBalancer;
using l4lb::lb::RoundRobinLoadBalancer;
using l4lb::lb::SelectionError;
using l4lb::lb::SelectionResult;

namespace {

std::vector<BackendConfig> ThreeBackends() {
    return {{"a", "127.0.0.1", 9001, 1},
            {"b", "127.0.0.1", 9002, 1},
            {"c", "127.0.0.1", 9003, 1}};
}

}  // namespace

L4LB_TEST(BackendPoolRejectsInvalidDefinitions) {
    bool duplicate = false;
    try {
        BackendPool pool({{"a", "127.0.0.1", 1, 1}, {"a", "127.0.0.1", 2, 1}});
        (void)pool;
    } catch (const std::invalid_argument&) {
        duplicate = true;
    }
    L4LB_REQUIRE(duplicate);

    bool weight = false;
    try {
        BackendPool pool({{"a", "127.0.0.1", 1, 0}});
        (void)pool;
    } catch (const std::invalid_argument&) {
        weight = true;
    }
    L4LB_REQUIRE(weight);

    bool address = false;
    try {
        BackendPool pool({{"a", "bad-ip", 1, 1}});
        (void)pool;
    } catch (const std::invalid_argument&) {
        address = true;
    }
    L4LB_REQUIRE(address);
}

L4LB_TEST(RoundRobinDistributesAndSkipsUnhealthyBackends) {
    BackendPool pool(ThreeBackends());
    RoundRobinLoadBalancer balancer;
    std::map<std::string, int> counts;
    for (int index = 0; index < 6; ++index) {
        SelectionResult result = balancer.Select(pool.Snapshot());
        L4LB_REQUIRE(static_cast<bool>(result));
        ++counts[result.lease.Get()->Id()];
    }
    L4LB_REQUIRE(counts["a"] == 2);
    L4LB_REQUIRE(counts["b"] == 2);
    L4LB_REQUIRE(counts["c"] == 2);

    L4LB_REQUIRE(pool.SetHealthy("b", false));
    for (int index = 0; index < 10; ++index) {
        SelectionResult result = balancer.Select(pool.Snapshot());
        L4LB_REQUIRE(result.lease.Get()->Id() != "b");
    }
}

L4LB_TEST(LeastConnectionsUsesStableTieBreakAndRaiiLease) {
    BackendPool pool(ThreeBackends());
    LeastConnectionsLoadBalancer balancer;
    SelectionResult first = balancer.Select(pool.Snapshot());
    L4LB_REQUIRE(first.lease.Get()->Id() == "a");
    L4LB_REQUIRE(pool.Find("a")->ActiveSessions() == 1);

    BackendLease moved = std::move(first.lease);
    L4LB_REQUIRE(!first.lease);
    L4LB_REQUIRE(pool.Find("a")->ActiveSessions() == 1);
    SelectionResult second = balancer.Select(pool.Snapshot());
    L4LB_REQUIRE(second.lease.Get()->Id() == "b");
    second.lease.Reset();
    moved.Reset();
    moved.Reset();
    L4LB_REQUIRE(pool.Find("a")->ActiveSessions() == 0);
    L4LB_REQUIRE(pool.Find("b")->ActiveSessions() == 0);
}

L4LB_TEST(NoAvailableBackendReturnsExplicitErrorAndMetric) {
    BackendPool pool({{"a", "127.0.0.1", 9001, 1}});
    RoundRobinLoadBalancer balancer;
    L4LB_REQUIRE(pool.SetHealthy("a", false));
    SelectionResult result = balancer.Select(pool.Snapshot());
    L4LB_REQUIRE(!result);
    L4LB_REQUIRE(result.error == SelectionError::kNoAvailableBackend);
    L4LB_REQUIRE(pool.NoAvailableBackendCount() == 1);
}

L4LB_TEST(BackendSnapshotsCanPublishWhileSelectorsRead) {
    BackendPool pool(ThreeBackends());
    RoundRobinLoadBalancer balancer;
    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int index = 0; index < 2000; ++index) {
            pool.SetHealthy("b", (index % 2) == 0);
        }
    });
    start.store(true, std::memory_order_release);
    for (int index = 0; index < 5000; ++index) {
        SelectionResult result = balancer.Select(pool.Snapshot());
        L4LB_REQUIRE(static_cast<bool>(result));
    }
    writer.join();
}
