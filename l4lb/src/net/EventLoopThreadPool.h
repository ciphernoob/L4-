#pragma once

#include "net/EventLoopThread.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace l4lb {
namespace net {

class EventLoop;
class EventLoopThreadPool {
public:
    explicit EventLoopThreadPool(EventLoop* base_loop) noexcept : base_loop_(base_loop) {}
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void Start(std::size_t thread_count);
    void Stop();
    EventLoop* NextLoop();
    const std::vector<EventLoop*>& Loops() const noexcept { return loops_; }
    bool IsStarted() const noexcept { return started_; }

private:
    EventLoop* base_loop_;
    bool started_{false};
    std::size_t next_{0};
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace net
}  // namespace l4lb
