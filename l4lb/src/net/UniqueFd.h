#pragma once

namespace l4lb {
namespace net {

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    int Get() const noexcept { return fd_; }
    bool IsValid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return IsValid(); }

    int Release() noexcept;
    void Reset(int fd = -1) noexcept;

private:
    int fd_{-1};
};

}  // namespace net
}  // namespace l4lb

