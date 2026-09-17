#pragma once

#include "net/UniqueFd.h"

#include <vector>
#include <sys/epoll.h>

namespace l4lb {
namespace net {

class Channel;

class Epoller {
public:
    Epoller();

    Epoller(const Epoller&) = delete;
    Epoller& operator=(const Epoller&) = delete;

    std::vector<Channel*> Poll(int timeout_ms);
    void UpdateChannel(Channel* channel);
    void RemoveChannel(Channel* channel);

private:
    static constexpr int kInitialEventCount = 64;
    UniqueFd epoll_fd_;
    std::vector<epoll_event> events_;
};

}  // namespace net
}  // namespace l4lb

