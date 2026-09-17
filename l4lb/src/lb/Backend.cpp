#include "lb/Backend.h"

#include <stdexcept>
#include <utility>

namespace l4lb {
namespace lb {

Backend::Backend(std::string id, net::InetAddress address, int weight)
    : id_(std::move(id)), address_(std::move(address)), weight_(weight) {
    if (id_.empty()) {
        throw std::invalid_argument("backend id must not be empty");
    }
    if (weight_ <= 0) {
        throw std::invalid_argument("backend weight must be positive");
    }
}

BackendLease::~BackendLease() {
    Reset();
}

BackendLease::BackendLease(BackendLease&& other) noexcept
    : backend_(std::move(other.backend_)) {}

BackendLease& BackendLease::operator=(BackendLease&& other) noexcept {
    if (this != &other) {
        Reset();
        backend_ = std::move(other.backend_);
    }
    return *this;
}

BackendLease BackendLease::Acquire(std::shared_ptr<Backend> backend) {
    if (!backend) {
        return BackendLease();
    }
    backend->Acquire();
    return BackendLease(std::move(backend));
}

void BackendLease::Reset() noexcept {
    if (backend_) {
        backend_->Release();
        backend_.reset();
    }
}

}  // namespace lb
}  // namespace l4lb

