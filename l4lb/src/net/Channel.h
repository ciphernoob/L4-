#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace l4lb {
namespace net {

class EventLoop;

class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd) noexcept;
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void HandleEvent();
    void Tie(const std::shared_ptr<void>& owner) noexcept;

    void SetReadCallback(EventCallback callback) { read_callback_ = std::move(callback); }
    void SetWriteCallback(EventCallback callback) { write_callback_ = std::move(callback); }
    void SetCloseCallback(EventCallback callback) { close_callback_ = std::move(callback); }
    void SetErrorCallback(EventCallback callback) { error_callback_ = std::move(callback); }

    void EnableReading();
    void DisableReading();
    void EnableWriting();
    void DisableWriting();
    void DisableAll();
    void EnableEdgeTriggered();
    void Remove();

    int Fd() const noexcept { return fd_; }
    std::uint32_t Events() const noexcept { return events_; }
    void SetReadyEvents(std::uint32_t events) noexcept { ready_events_ = events; }
    bool IsWriting() const noexcept;
    bool IsReading() const noexcept;
    bool IsAdded() const noexcept { return added_; }
    void SetAdded(bool added) noexcept { added_ = added; }

private:
    void Update();
    void HandleEventWithGuard();

    EventLoop* loop_;
    const int fd_;
    std::uint32_t events_{0};
    std::uint32_t ready_events_{0};
    bool added_{false};
    bool tied_{false};
    std::weak_ptr<void> tie_;
    EventCallback read_callback_;
    EventCallback write_callback_;
    EventCallback close_callback_;
    EventCallback error_callback_;
};

}  // namespace net
}  // namespace l4lb
