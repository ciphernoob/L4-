#include "lb/HealthChecker.h"

#include "lb/Backend.h"
#include "lb/BackendPool.h"
#include "net/EventLoop.h"

#include <stdexcept>
#include <utility>

namespace l4lb {
namespace lb {

HealthChecker::HealthChecker(net::EventLoop* loop, std::shared_ptr<BackendPool> pool,
                             Options options)
    : loop_(loop), pool_(std::move(pool)), options_(options) {
    if (loop_ == nullptr || !pool_) {
        throw std::invalid_argument("HealthChecker requires loop and backend pool");
    }
    if (options_.interval <= std::chrono::milliseconds::zero() ||
        options_.timeout <= std::chrono::milliseconds::zero() ||
        options_.timeout >= options_.interval || options_.failure_threshold == 0 ||
        options_.success_threshold == 0) {
        throw std::invalid_argument("HealthChecker options are invalid");
    }
}

HealthChecker::~HealthChecker() {
    Stop();
}

void HealthChecker::Start() {
    loop_->AssertInLoopThread();
    if (started_) {
        throw std::logic_error("HealthChecker cannot be started twice");
    }
    started_ = true;
    for (const std::shared_ptr<Backend>& backend : pool_->Backends()) {
        ProbeState state;
        state.backend = backend;
        states_.emplace(backend->Id(), std::move(state));
    }
    for (auto& entry : states_) {
        const std::string id = entry.first;
        entry.second.timer = loop_->RunEvery(options_.interval,
                                             [this, id] { Probe(id); });
        Probe(id);
    }
}

void HealthChecker::Stop() {
    if (!started_) {
        return;
    }
    loop_->AssertInLoopThread();
    started_ = false;
    for (auto& entry : states_) {
        entry.second.timer.Cancel();
        if (entry.second.connector) {
            entry.second.connector->Cancel();
            entry.second.connector.reset();
        }
    }
    states_.clear();
}

void HealthChecker::Probe(const std::string& id) {
    if (!started_) {
        return;
    }
    auto found = states_.find(id);
    if (found == states_.end() || found->second.connector) {
        return;
    }
    ProbeState& state = found->second;
    state.connector.reset(
        new Connector(loop_, state.backend->Address(), options_.timeout));
    state.connector->SetSuccessCallback(
        [this, id](const std::shared_ptr<Connector>&, net::Socket) {
            Complete(id, true);
        });
    state.connector->SetErrorCallback(
        [this, id](const std::shared_ptr<Connector>&, const std::error_code&) {
            Complete(id, false);
        });
    state.connector->Start();
}

void HealthChecker::Complete(const std::string& id, bool succeeded) {
    auto found = states_.find(id);
    if (!started_ || found == states_.end()) {
        return;
    }
    ProbeState& state = found->second;
    state.connector.reset();
    if (succeeded) {
        state.consecutive_failures = 0;
        ++state.consecutive_successes;
        if (!state.backend->IsHealthy() &&
            state.consecutive_successes >= options_.success_threshold) {
            pool_->SetHealthy(id, true);
            state.consecutive_successes = 0;
        }
    } else {
        state.consecutive_successes = 0;
        ++state.consecutive_failures;
        if (state.backend->IsHealthy() &&
            state.consecutive_failures >= options_.failure_threshold) {
            pool_->SetHealthy(id, false);
            state.consecutive_failures = 0;
        }
    }
}

}  // namespace lb
}  // namespace l4lb
