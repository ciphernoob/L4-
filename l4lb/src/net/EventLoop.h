#pragma once

#include "net/UniqueFd.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace l4lb {
namespace net {

class Channel;
class Epoller;

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
    std::mutex pending_mutex_;
    std::vector<Functor> pending_functors_;
};

}  // namespace net
}  // namespace l4lb

