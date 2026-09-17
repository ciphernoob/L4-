#include "lb/SessionMap.h"

#include "net/EventLoop.h"
#include "net/TcpConnection.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace l4lb {
namespace lb {

SessionMap::SessionMap(
    net::EventLoop* loop,
    std::shared_ptr<std::atomic<ProxySession::Id>> global_next_id)
    : loop_(loop), global_next_id_(std::move(global_next_id)) {
    if (loop_ == nullptr) {
        throw std::invalid_argument("SessionMap requires an EventLoop");
    }
}

SessionMap::~SessionMap() {
    if (!sessions_.empty()) {
        loop_->AssertInLoopThread();
        CloseAll();
    }
}

std::shared_ptr<ProxySession> SessionMap::Add(
    std::shared_ptr<net::TcpConnection> frontend,
    ProxySession::Options options) {
    loop_->AssertInLoopThread();
    const ProxySession::Id id = global_next_id_
                                    ? global_next_id_->fetch_add(
                                          1, std::memory_order_relaxed)
                                    : next_id_++;
    std::shared_ptr<ProxySession> session(new ProxySession(
        loop_, id, std::move(frontend), std::move(options),
        [this](ProxySession::Id closed_id) { Remove(closed_id); }));
    sessions_.emplace(id, session);
    return session;
}

bool SessionMap::Remove(ProxySession::Id id) {
    loop_->AssertInLoopThread();
    return sessions_.erase(id) != 0;
}

void SessionMap::CloseAll() {
    loop_->AssertInLoopThread();
    std::vector<std::shared_ptr<ProxySession>> sessions;
    sessions.reserve(sessions_.size());
    for (const auto& entry : sessions_) {
        sessions.push_back(entry.second);
    }
    for (const std::shared_ptr<ProxySession>& session : sessions) {
        session->Close(ProxySession::CloseReason::kExplicit);
    }
    sessions_.clear();
}

std::shared_ptr<ProxySession> SessionMap::Find(ProxySession::Id id) const {
    const auto found = sessions_.find(id);
    return found == sessions_.end() ? nullptr : found->second;
}

}  // namespace lb
}  // namespace l4lb
