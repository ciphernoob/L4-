#pragma once

#include "lb/ProxySession.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <unordered_map>

namespace l4lb {
namespace net {
class EventLoop;
class TcpConnection;
}  // namespace net

namespace lb {

class SessionMap {
public:
    explicit SessionMap(
        net::EventLoop* loop,
        std::shared_ptr<std::atomic<ProxySession::Id>> global_next_id = nullptr);
    ~SessionMap();

    SessionMap(const SessionMap&) = delete;
    SessionMap& operator=(const SessionMap&) = delete;

    std::shared_ptr<ProxySession> Add(
        std::shared_ptr<net::TcpConnection> frontend,
        ProxySession::Options options);
    bool Remove(ProxySession::Id id);
    void CloseAll();

    std::size_t Size() const noexcept { return sessions_.size(); }
    std::shared_ptr<ProxySession> Find(ProxySession::Id id) const;

private:
    net::EventLoop* loop_;
    std::shared_ptr<std::atomic<ProxySession::Id>> global_next_id_;
    ProxySession::Id next_id_{1};
    std::unordered_map<ProxySession::Id, std::shared_ptr<ProxySession>> sessions_;
};

}  // namespace lb
}  // namespace l4lb
