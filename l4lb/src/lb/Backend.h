#pragma once

#include "net/InetAddress.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace l4lb {
namespace lb {

class Backend {
public:
    Backend(std::string id, net::InetAddress address, int weight);

    const std::string& Id() const noexcept { return id_; }
    const net::InetAddress& Address() const noexcept { return address_; }
    int Weight() const noexcept { return weight_; }
    bool IsHealthy() const noexcept { return healthy_.load(std::memory_order_acquire); }
    void SetHealthy(bool healthy) noexcept {
        healthy_.store(healthy, std::memory_order_release);
    }
    std::size_t ActiveSessions() const noexcept {
        return active_sessions_.load(std::memory_order_relaxed);
    }

private:
    friend class BackendLease;
    void Acquire() noexcept { active_sessions_.fetch_add(1, std::memory_order_relaxed); }
    void Release() noexcept { active_sessions_.fetch_sub(1, std::memory_order_relaxed); }

    const std::string id_;
    const net::InetAddress address_;
    const int weight_;
    std::atomic<bool> healthy_{true};
    std::atomic<std::size_t> active_sessions_{0};
};

class BackendLease {
public:
    BackendLease() noexcept = default;
    ~BackendLease();

    BackendLease(const BackendLease&) = delete;
    BackendLease& operator=(const BackendLease&) = delete;
    BackendLease(BackendLease&& other) noexcept;
    BackendLease& operator=(BackendLease&& other) noexcept;

    static BackendLease Acquire(std::shared_ptr<Backend> backend);
    Backend* Get() const noexcept { return backend_.get(); }
    const std::shared_ptr<Backend>& SharedBackend() const noexcept { return backend_; }
    explicit operator bool() const noexcept { return static_cast<bool>(backend_); }
    void Reset() noexcept;

private:
    explicit BackendLease(std::shared_ptr<Backend> backend) noexcept
        : backend_(std::move(backend)) {}

    std::shared_ptr<Backend> backend_;
};

}  // namespace lb
}  // namespace l4lb
