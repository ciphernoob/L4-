#include "lb/BackendPool.h"

#include <stdexcept>
#include <unordered_set>

namespace l4lb {
namespace lb {

BackendPool::BackendPool(const std::vector<BackendConfig>& configs) {
    if (configs.empty()) {
        throw std::invalid_argument("backend list must not be empty");
    }
    std::unordered_set<std::string> ids;
    backends_.reserve(configs.size());
    for (const BackendConfig& config : configs) {
        if (!ids.insert(config.id).second) {
            throw std::invalid_argument("duplicate backend id: " + config.id);
        }
        backends_.push_back(std::make_shared<Backend>(
            config.id, net::InetAddress(config.ip, config.port), config.weight));
    }
    PublishSnapshotLocked();
}

std::shared_ptr<const BackendSnapshot> BackendPool::Snapshot() const noexcept {
    return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

bool BackendPool::SetHealthy(const std::string& id, bool healthy) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::shared_ptr<Backend>& backend : backends_) {
        if (backend->Id() == id) {
            backend->SetHealthy(healthy);
            PublishSnapshotLocked();
            return true;
        }
    }
    return false;
}

std::shared_ptr<Backend> BackendPool::Find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::shared_ptr<Backend>& backend : backends_) {
        if (backend->Id() == id) {
            return backend;
        }
    }
    return {};
}

std::vector<std::shared_ptr<Backend>> BackendPool::Backends() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backends_;
}

std::uint64_t BackendPool::NoAvailableBackendCount() const noexcept {
    return counters_->no_available_backend.load(std::memory_order_relaxed);
}

void BackendPool::PublishSnapshotLocked() {
    std::shared_ptr<BackendSnapshot> next(new BackendSnapshot());
    next->counters = counters_;
    for (const std::shared_ptr<Backend>& backend : backends_) {
        if (backend->IsHealthy()) {
            next->candidates.push_back(backend);
        }
    }
    std::shared_ptr<const BackendSnapshot> immutable = next;
    std::atomic_store_explicit(&snapshot_, std::move(immutable), std::memory_order_release);
}

}  // namespace lb
}  // namespace l4lb
