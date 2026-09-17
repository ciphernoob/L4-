#include "lb/LoadBalancer.h"

#include <limits>

namespace l4lb {
namespace lb {

SelectionResult LoadBalancer::NoBackend(
    const std::shared_ptr<const BackendSnapshot>& snapshot) {
    if (snapshot && snapshot->counters) {
        snapshot->counters->no_available_backend.fetch_add(1, std::memory_order_relaxed);
    }
    return SelectionResult{BackendLease(), SelectionError::kNoAvailableBackend};
}

SelectionResult RoundRobinLoadBalancer::Select(
    const std::shared_ptr<const BackendSnapshot>& snapshot) {
    if (!snapshot || snapshot->candidates.empty()) {
        return NoBackend(snapshot);
    }
    const std::uint64_t current = sequence_.fetch_add(1, std::memory_order_relaxed);
    std::shared_ptr<Backend> backend =
        snapshot->candidates[current % snapshot->candidates.size()];
    return SelectionResult{BackendLease::Acquire(std::move(backend)), SelectionError::kNone};
}

SelectionResult LeastConnectionsLoadBalancer::Select(
    const std::shared_ptr<const BackendSnapshot>& snapshot) {
    if (!snapshot || snapshot->candidates.empty()) {
        return NoBackend(snapshot);
    }
    std::shared_ptr<Backend> selected;
    std::size_t minimum = std::numeric_limits<std::size_t>::max();
    for (const std::shared_ptr<Backend>& backend : snapshot->candidates) {
        const std::size_t active = backend->ActiveSessions();
        if (active < minimum) {
            selected = backend;
            minimum = active;
        }
    }
    return SelectionResult{BackendLease::Acquire(std::move(selected)), SelectionError::kNone};
}

}  // namespace lb
}  // namespace l4lb

