#pragma once

#include "net/UniqueFd.h"
#include "net/TimerId.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace l4lb {
namespace net {

class Channel;
class Epoller;
class TimerQueue;

class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void Loop();
    void Quit();
    void RunInLoop(Functor functor);
    void QueueInLoop(Functor functor);
    TimerId RunAfter(std::chrono::milliseconds delay, Functor functor);
    TimerId RunEvery(std::chrono::milliseconds interval, Functor functor);

    bool IsInLoopThread() const noexcept;
    void AssertInLoopThread() const;
    void UpdateChannel(Channel* channel);
    void RemoveChannel(Channel* channel);

private:
    void Wakeup();
    void HandleWakeup();
    void RunPendingFunctors();

    const std::thread::id thread_id_;
    std::atomic<bool> quit_{false};
    bool looping_{false};
    bool running_pending_functors_{false};
    std::unique_ptr<Epoller> poller_;
    UniqueFd wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;
    std::unique_ptr<TimerQueue> timer_queue_;
    std::mutex pending_mutex_;
    std::vector<Functor> pending_functors_;
};

}  // namespace net
}  // namespace l4lb
