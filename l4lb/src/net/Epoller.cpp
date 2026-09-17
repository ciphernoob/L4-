#include "net/Epoller.h"

#include "net/Channel.h"

#include <cerrno>
#include <system_error>
#include <unistd.h>

namespace l4lb {
namespace net {
namespace {

void ThrowSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

}  // namespace

Epoller::Epoller()
    : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)), events_(kInitialEventCount) {
    if (!epoll_fd_) {
        ThrowSystemError("epoll_create1");
    }
}

std::vector<Channel*> Epoller::Poll(int timeout_ms) {
    const int count = ::epoll_wait(epoll_fd_.Get(), events_.data(),
                                   static_cast<int>(events_.size()), timeout_ms);
    if (count < 0) {
        if (errno == EINTR) {
            return {};
        }
        ThrowSystemError("epoll_wait");
    }

    std::vector<Channel*> active;
    active.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        auto* channel = static_cast<Channel*>(events_[index].data.ptr);
        channel->SetReadyEvents(events_[index].events);
        active.push_back(channel);
    }
    if (count == static_cast<int>(events_.size())) {
        events_.resize(events_.size() * 2);
    }
    return active;
}

void Epoller::UpdateChannel(Channel* channel) {
    if (channel->Events() == 0) {
        RemoveChannel(channel);
        return;
    }

    epoll_event event{};
    event.events = channel->Events();
    event.data.ptr = channel;
    const int operation = channel->IsAdded() ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(epoll_fd_.Get(), operation, channel->Fd(), &event) < 0) {
        ThrowSystemError(operation == EPOLL_CTL_ADD ? "epoll_ctl(ADD)" : "epoll_ctl(MOD)");
    }
    channel->SetAdded(true);
}

void Epoller::RemoveChannel(Channel* channel) {
    if (!channel->IsAdded()) {
        return;
    }
    if (::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_DEL, channel->Fd(), nullptr) < 0 &&
        errno != ENOENT && errno != EBADF) {
        ThrowSystemError("epoll_ctl(DEL)");
    }
    channel->SetAdded(false);
}

}  // namespace net
}  // namespace l4lb

