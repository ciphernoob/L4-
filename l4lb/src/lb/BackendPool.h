#pragma once

#include "lb/Backend.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace l4lb {
namespace lb {

struct BackendConfig {
    std::string id;
    std::string ip;
    std::uint16_t port;
    int weight;
};

struct SelectionCounters {
    std::atomic<std::uint64_t> no_available_backend{0};
};

struct BackendSnapshot {
    std::vector<std::shared_ptr<Backend>> candidates;
    std::shared_ptr<SelectionCounters> counters;
};

class BackendPool {
public:
    explicit BackendPool(const std::vector<BackendConfig>& configs);

    BackendPool(const BackendPool&) = delete;
    BackendPool& operator=(const BackendPool&) = delete;

    std::shared_ptr<const BackendSnapshot> Snapshot() const noexcept;
    bool SetHealthy(const std::string& id, bool healthy);
    std::shared_ptr<Backend> Find(const std::string& id) const;
    std::vector<std::shared_ptr<Backend>> Backends() const;
    std::uint64_t NoAvailableBackendCount() const noexcept;

private:
    void PublishSnapshotLocked();

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Backend>> backends_;
    std::shared_ptr<SelectionCounters> counters_{new SelectionCounters()};
    std::shared_ptr<const BackendSnapshot> snapshot_;
};

}  // namespace lb
}  // namespace l4lb
