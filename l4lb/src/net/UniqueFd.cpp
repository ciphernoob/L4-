#include "net/UniqueFd.h"

#include <unistd.h>
#include <utility>

namespace l4lb {
namespace net {

UniqueFd::~UniqueFd() {
    Reset();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        Reset(other.Release());
    }
    return *this;
}

int UniqueFd::Release() noexcept {
    return std::exchange(fd_, -1);
}

void UniqueFd::Reset(int fd) noexcept {
    if (fd_ == fd) {
        return;
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

}  // namespace net
}  // namespace l4lb
