#include "net/EventLoop.h"

#include "net/Channel.h"
#include "net/Epoller.h"

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>

namespace l4lb {
namespace net {

EventLoop::EventLoop()
    : thread_id_(std::this_thread::get_id()),
      poller_(new Epoller()),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (!wakeup_fd_) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    wakeup_channel_.reset(new Channel(this, wakeup_fd_.Get()));
    wakeup_channel_->SetReadCallback([this] { HandleWakeup(); });
    wakeup_channel_->EnableReading();
}

EventLoop::~EventLoop() {
    AssertInLoopThread();
    wakeup_channel_->Remove();
}

void EventLoop::Loop() {
    AssertInLoopThread();
    if (looping_) {
        throw std::logic_error("EventLoop::Loop called twice");
    }
    looping_ = true;
    while (!quit_.load(std::memory_order_acquire)) {
        for (Channel* channel : poller_->Poll(-1)) {
            channel->HandleEvent();
        }
        RunPendingFunctors();
    }
    looping_ = false;
}

void EventLoop::Quit() {
    quit_.store(true, std::memory_order_release);
    if (!IsInLoopThread()) {
        Wakeup();
    }
}

void EventLoop::RunInLoop(Functor functor) {
    if (IsInLoopThread()) {
        functor();
    } else {
        QueueInLoop(std::move(functor));
    }
}

void EventLoop::QueueInLoop(Functor functor) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_functors_.push_back(std::move(functor));
    }
    if (!IsInLoopThread() || running_pending_functors_) {
        Wakeup();
    }
}

bool EventLoop::IsInLoopThread() const noexcept {
    return thread_id_ == std::this_thread::get_id();
}

void EventLoop::AssertInLoopThread() const {
    if (!IsInLoopThread()) {
        throw std::logic_error("EventLoop operation called from a foreign thread");
    }
}

void EventLoop::UpdateChannel(Channel* channel) {
    AssertInLoopThread();
    poller_->UpdateChannel(channel);
}

void EventLoop::RemoveChannel(Channel* channel) {
    AssertInLoopThread();
    poller_->RemoveChannel(channel);
}

void EventLoop::Wakeup() {
    const std::uint64_t one = 1;
    const ssize_t result = ::write(wakeup_fd_.Get(), &one, sizeof(one));
    if (result < 0 && errno != EAGAIN && errno != EINTR) {
        throw std::system_error(errno, std::generic_category(), "eventfd write");
    }
}

void EventLoop::HandleWakeup() {
    std::uint64_t value = 0;
    while (true) {
        const ssize_t result = ::read(wakeup_fd_.Get(), &value, sizeof(value));
        if (result == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (result < 0) {
            throw std::system_error(errno, std::generic_category(), "eventfd read");
        }
        break;
    }
}

void EventLoop::RunPendingFunctors() {
    running_pending_functors_ = true;
    std::vector<Functor> functors;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        functors.swap(pending_functors_);
    }
    for (Functor& functor : functors) {
        functor();
    }
    running_pending_functors_ = false;
}

}  // namespace net
}  // namespace l4lb

