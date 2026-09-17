#include "net/EventLoopThreadPool.h"

#include "net/EventLoop.h"
#include "net/EventLoopThread.h"

#include <stdexcept>

namespace l4lb {
namespace net {

EventLoopThreadPool::~EventLoopThreadPool() {
    Stop();
}

void EventLoopThreadPool::Start(std::size_t thread_count) {
    base_loop_->AssertInLoopThread();
    if (started_) {
        throw std::logic_error("EventLoopThreadPool cannot be started twice");
    }
    started_ = true;
    threads_.reserve(thread_count);
    loops_.reserve(thread_count);
    try {
        for (std::size_t index = 0; index < thread_count; ++index) {
            std::unique_ptr<EventLoopThread> thread(new EventLoopThread());
            loops_.push_back(thread->StartLoop());
            threads_.push_back(std::move(thread));
        }
    } catch (...) {
        Stop();
        throw;
    }
}

void EventLoopThreadPool::Stop() {
    loops_.clear();
    threads_.clear();
    next_ = 0;
    started_ = false;
}

EventLoop* EventLoopThreadPool::NextLoop() {
    base_loop_->AssertInLoopThread();
    if (!started_) {
        throw std::logic_error("EventLoopThreadPool is not started");
    }
    if (loops_.empty()) {
        return base_loop_;
    }
    EventLoop* result = loops_[next_];
    next_ = (next_ + 1) % loops_.size();
    return result;
}

}  // namespace net
}  // namespace l4lb

