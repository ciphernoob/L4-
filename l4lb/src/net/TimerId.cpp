#include "net/TimerId.h"

#include <utility>

namespace l4lb {
namespace net {

TimerId::~TimerId() {
    Cancel();
}

TimerId::TimerId(TimerId&& other) noexcept
    : cancellation_(std::move(other.cancellation_)) {}

TimerId& TimerId::operator=(TimerId&& other) noexcept {
    if (this != &other) {
        Cancel();
        cancellation_ = std::move(other.cancellation_);
    }
    return *this;
}

void TimerId::Cancel() noexcept {
    if (cancellation_) {
        cancellation_->cancelled.store(true, std::memory_order_release);
    }
}

bool TimerId::IsCancelled() const noexcept {
    return !cancellation_ || cancellation_->cancelled.load(std::memory_order_acquire);
}

}  // namespace net
}  // namespace l4lb

