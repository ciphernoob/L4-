#include "net/EventLoopThread.h"

#include "net/EventLoop.h"

#include <stdexcept>

namespace l4lb {
namespace net {

EventLoopThread::~EventLoopThread() {
    Stop();
}

EventLoop* EventLoopThread::StartLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (starting_ || thread_.joinable()) {
        throw std::logic_error("EventLoopThread cannot be started twice");
    }
    starting_ = true;
    exited_ = false;
    thread_ = std::thread([this] { ThreadMain(); });
    condition_.wait(lock, [this] { return loop_ != nullptr || exited_; });
    if (loop_ == nullptr) {
        lock.unlock();
        thread_.join();
        throw std::runtime_error("EventLoopThread exited during startup");
    }
    return loop_;
}

void EventLoopThread::Stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!thread_.joinable()) {
        starting_ = false;
        return;
    }
    if (loop_ != nullptr) {
        loop_->Quit();
    }
    lock.unlock();
    thread_.join();
    lock.lock();
    starting_ = false;
}

bool EventLoopThread::IsStarted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return starting_ && thread_.joinable();
}

void EventLoopThread::ThreadMain() {
    EventLoop loop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        condition_.notify_all();
    }
    loop.Loop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = nullptr;
        exited_ = true;
        condition_.notify_all();
    }
}

}  // namespace net
}  // namespace l4lb

