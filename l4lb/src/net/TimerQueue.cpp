#include "net/TimerQueue.h"

#include "net/Channel.h"
#include "net/EventLoop.h"

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <sys/timerfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace l4lb {
namespace net {
namespace {

void ThrowSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

timespec ToTimespec(TimerQueue::Clock::duration duration) {
    if (duration <= TimerQueue::Clock::duration::zero()) {
        duration = std::chrono::microseconds(1);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
    timespec value{};
    value.tv_sec = static_cast<time_t>(seconds.count());
    value.tv_nsec = static_cast<long>(nanoseconds.count());
    return value;
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop), timer_fd_(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) {
    if (!timer_fd_) {
        ThrowSystemError("timerfd_create");
    }
    channel_.reset(new Channel(loop_, timer_fd_.Get()));
    channel_->SetReadCallback([this] { HandleRead(); });
    channel_->EnableReading();
}

TimerQueue::~TimerQueue() {
    loop_->AssertInLoopThread();
    channel_->Remove();
}

TimerId TimerQueue::AddTimer(Duration delay, Duration interval, Callback callback) {
    loop_->AssertInLoopThread();
    if (delay < Duration::zero() || interval < Duration::zero()) {
        throw std::invalid_argument("timer durations must be non-negative");
    }

    const bool reset_earliest = timers_.empty() || Clock::now() + delay < timers_.begin()->first.expiration;
    std::shared_ptr<detail::TimerCancellation> cancellation(new detail::TimerCancellation());
    const Key key{Clock::now() + delay, ++next_sequence_};
    timers_.emplace(key, TimerEntry{interval, std::move(callback), cancellation});
    if (reset_earliest) {
        ResetTimerFd();
    }
    return TimerId(std::move(cancellation));
}

void TimerQueue::HandleRead() {
    std::uint64_t expirations = 0;
    ssize_t result;
    do {
        result = ::read(timer_fd_.Get(), &expirations, sizeof(expirations));
    } while (result < 0 && errno == EINTR);
    if (result != static_cast<ssize_t>(sizeof(expirations))) {
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        ThrowSystemError("timerfd read");
    }

    const Clock::time_point now = Clock::now();
    const Key upper{now, UINT64_MAX};
    const auto end = timers_.upper_bound(upper);
    std::vector<TimerEntry> expired;
    for (auto iterator = timers_.begin(); iterator != end; ++iterator) {
        expired.push_back(std::move(iterator->second));
    }
    timers_.erase(timers_.begin(), end);

    for (TimerEntry& timer : expired) {
        if (!timer.cancellation->cancelled.load(std::memory_order_acquire)) {
            timer.callback();
        }
    }

    const Clock::time_point restart_time = Clock::now();
    for (TimerEntry& timer : expired) {
        if (timer.interval > Duration::zero() &&
            !timer.cancellation->cancelled.load(std::memory_order_acquire)) {
            const Key key{restart_time + timer.interval, ++next_sequence_};
            timers_.emplace(key, std::move(timer));
        }
    }
    ResetTimerFd();
}

void TimerQueue::ResetTimerFd() {
    itimerspec value{};
    if (!timers_.empty()) {
        value.it_value = ToTimespec(timers_.begin()->first.expiration - Clock::now());
    }
    if (::timerfd_settime(timer_fd_.Get(), 0, &value, nullptr) < 0) {
        ThrowSystemError("timerfd_settime");
    }
}

}  // namespace net
}  // namespace l4lb

