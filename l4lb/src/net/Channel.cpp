#include "net/Channel.h"

#include "net/EventLoop.h"

#include <cassert>
#include <sys/epoll.h>

namespace l4lb {
namespace net {

Channel::Channel(EventLoop* loop, int fd) noexcept : loop_(loop), fd_(fd) {}

Channel::~Channel() {
    assert(!added_ && "Channel must be removed from its EventLoop before destruction");
}

void Channel::HandleEvent() {
    if (!tied_) {
        HandleEventWithGuard();
        return;
    }
    const std::shared_ptr<void> guard = tie_.lock();
    if (guard) {
        HandleEventWithGuard();
    }
}

void Channel::Tie(const std::shared_ptr<void>& owner) noexcept {
    tie_ = owner;
    tied_ = true;
}

void Channel::EnableReading() {
    events_ |= static_cast<std::uint32_t>(EPOLLIN | EPOLLPRI | EPOLLRDHUP);
    Update();
}

void Channel::DisableReading() {
    events_ &= ~static_cast<std::uint32_t>(EPOLLIN | EPOLLPRI | EPOLLRDHUP);
    Update();
}

void Channel::EnableWriting() {
    events_ |= static_cast<std::uint32_t>(EPOLLOUT);
    Update();
}

void Channel::DisableWriting() {
    events_ &= ~static_cast<std::uint32_t>(EPOLLOUT);
    Update();
}

void Channel::DisableAll() {
    events_ = 0;
    Update();
}

void Channel::EnableEdgeTriggered() {
    events_ |= static_cast<std::uint32_t>(EPOLLET);
    Update();
}

void Channel::Remove() {
    loop_->RemoveChannel(this);
}

bool Channel::IsWriting() const noexcept {
    return (events_ & static_cast<std::uint32_t>(EPOLLOUT)) != 0;
}

bool Channel::IsReading() const noexcept {
    return (events_ & static_cast<std::uint32_t>(EPOLLIN)) != 0;
}

void Channel::Update() {
    loop_->UpdateChannel(this);
}

void Channel::HandleEventWithGuard() {
    if ((ready_events_ & static_cast<std::uint32_t>(EPOLLHUP)) != 0 &&
        (ready_events_ & static_cast<std::uint32_t>(EPOLLIN)) == 0) {
        if (close_callback_) {
            close_callback_();
        }
    }
    if ((ready_events_ & static_cast<std::uint32_t>(EPOLLERR)) != 0) {
        if (error_callback_) {
            error_callback_();
        }
    }
    if ((ready_events_ & static_cast<std::uint32_t>(EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0) {
        if (read_callback_) {
            read_callback_();
        }
    }
    if ((ready_events_ & static_cast<std::uint32_t>(EPOLLOUT)) != 0) {
        if (write_callback_) {
            write_callback_();
        }
    }
}

}  // namespace net
}  // namespace l4lb

