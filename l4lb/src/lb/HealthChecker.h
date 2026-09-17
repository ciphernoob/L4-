#pragma once

#include "lb/Connector.h"
#include "net/TimerId.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace l4lb {
namespace net {
class EventLoop;
}
namespace lb {

class Backend;
class BackendPool;

class HealthChecker {
public:
    struct Options {
        std::chrono::milliseconds interval;
        std::chrono::milliseconds timeout;
        unsigned int failure_threshold;
        unsigned int success_threshold;
    };

    HealthChecker(net::EventLoop* loop, std::shared_ptr<BackendPool> pool,
                  Options options);
    ~HealthChecker();

    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    void Start();
    void Stop();
    bool IsStarted() const noexcept { return started_; }

private:
    struct ProbeState {
        std::shared_ptr<Backend> backend;
        std::shared_ptr<Connector> connector;
        net::TimerId timer;
        unsigned int consecutive_successes{0};
        unsigned int consecutive_failures{0};
    };

    void Probe(const std::string& id);
    void Complete(const std::string& id, bool succeeded);

    net::EventLoop* loop_;
    std::shared_ptr<BackendPool> pool_;
    Options options_;
    std::unordered_map<std::string, ProbeState> states_;
    bool started_{false};
};

}  // namespace lb
}  // namespace l4lb
