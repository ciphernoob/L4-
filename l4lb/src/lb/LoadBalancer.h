#pragma once

#include "lb/BackendPool.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace l4lb {
namespace lb {

enum class SelectionError {
    kNone,
    kNoAvailableBackend,
};

struct SelectionResult {
    BackendLease lease;
    SelectionError error{SelectionError::kNone};

    explicit operator bool() const noexcept { return static_cast<bool>(lease); }
};

class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;
    virtual SelectionResult Select(const std::shared_ptr<const BackendSnapshot>& snapshot) = 0;

protected:
    static SelectionResult NoBackend(const std::shared_ptr<const BackendSnapshot>& snapshot);
};

class RoundRobinLoadBalancer : public LoadBalancer {
public:
    SelectionResult Select(const std::shared_ptr<const BackendSnapshot>& snapshot) override;

private:
    std::atomic<std::uint64_t> sequence_{0};
};

class LeastConnectionsLoadBalancer : public LoadBalancer {
public:
    SelectionResult Select(const std::shared_ptr<const BackendSnapshot>& snapshot) override;
};

}  // namespace lb
}  // namespace l4lb

