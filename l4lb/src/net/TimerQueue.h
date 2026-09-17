#pragma once

#include "net/TimerId.h"
#include "net/UniqueFd.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

namespace l4lb {
namespace net {

class Channel;
class EventLoop;

class TimerQueue {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;
    using Callback = std::function<void()>;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    TimerId AddTimer(Duration delay, Duration interval, Callback callback);

private:
    struct Key {
        Clock::time_point expiration;
        std::uint64_t sequence;

        bool operator<(const Key& other) const noexcept {
            return expiration < other.expiration ||
                   (expiration == other.expiration && sequence < other.sequence);
        }
    };

    struct TimerEntry {
        Duration interval;
        Callback callback;
        std::shared_ptr<detail::TimerCancellation> cancellation;
    };

    void HandleRead();
    void ResetTimerFd();

    EventLoop* loop_;
    UniqueFd timer_fd_;
    std::unique_ptr<Channel> channel_;
    std::uint64_t next_sequence_{0};
    std::map<Key, TimerEntry> timers_;
};

}  // namespace net
}  // namespace l4lb

