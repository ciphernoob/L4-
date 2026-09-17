#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

namespace l4lb {
namespace net {

class EventLoop;

class EventLoopThread {
public:
    EventLoopThread() = default;
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    EventLoop* StartLoop();
    void Stop();
    bool IsStarted() const;

private:
    void ThreadMain();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    EventLoop* loop_{nullptr};
    bool starting_{false};
    bool exited_{false};
};

}  // namespace net
}  // namespace l4lb

