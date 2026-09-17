#pragma once

#include <atomic>
#include <memory>

namespace l4lb {
namespace net {

namespace detail {

struct TimerCancellation {
    std::atomic<bool> cancelled{false};
};

}  // namespace detail

class TimerQueue;

class TimerId {
public:
    TimerId() noexcept = default;
    ~TimerId();

    TimerId(const TimerId&) = delete;
    TimerId& operator=(const TimerId&) = delete;
    TimerId(TimerId&& other) noexcept;
    TimerId& operator=(TimerId&& other) noexcept;

    void Cancel() noexcept;
    bool IsCancelled() const noexcept;
    bool IsValid() const noexcept { return static_cast<bool>(cancellation_); }

private:
    friend class TimerQueue;
    explicit TimerId(std::shared_ptr<detail::TimerCancellation> cancellation) noexcept
        : cancellation_(std::move(cancellation)) {}

    std::shared_ptr<detail::TimerCancellation> cancellation_;
};

}  // namespace net
}  // namespace l4lb

