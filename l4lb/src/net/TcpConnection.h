#pragma once

#include "net/Buffer.h"
#include "net/Socket.h"

#include <functional>
#include <memory>
#include <string>

namespace l4lb {
namespace net {

class Channel;
class EventLoop;

// 一个 TcpConnection 只管理一个 TCP socket，不知道负载均衡业务。
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    enum class State { kConnecting, kConnected, kDisconnected };
    using Ptr = std::shared_ptr<TcpConnection>;
    using EventCallback = std::function<void(const Ptr&)>;
    using MessageCallback = std::function<void(const Ptr&, Buffer*)>;
    static constexpr std::size_t kReadBatchSize = 16 * 1024;

    TcpConnection(EventLoop* loop, Socket socket, std::string name);
    ~TcpConnection();
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    void SetMessageCallback(MessageCallback cb) { message_callback_ = std::move(cb); }
    void SetWriteCompleteCallback(EventCallback cb) { write_complete_callback_ = std::move(cb); }
    void SetEofCallback(EventCallback cb) { eof_callback_ = std::move(cb); }
    void SetCloseCallback(EventCallback cb) { close_callback_ = std::move(cb); }

    void Establish(bool start_reading = true);
    void Send(const void* data, std::size_t length);
    void Send(const std::string& data) { Send(data.data(), data.size()); }
    void StartRead();
    void StopRead();
    void ShutdownWrite();
    void ForceClose();

    EventLoop* Loop() const { return loop_; }
    int Fd() const { return socket_.Fd(); }
    State GetState() const { return state_; }
    bool IsReading() const { return reading_; }
    bool HasReceivedEof() const { return received_eof_; }
    std::size_t OutputBufferBytes() const { return output_.ReadableBytes(); }
    std::size_t InputBufferBytes() const { return input_.ReadableBytes(); }

private:
    void HandleRead();
    void HandleWrite();
    void FinishWrite();

    EventLoop* loop_;
    Socket socket_;
    std::string name_;
    std::unique_ptr<Channel> channel_;
    State state_{State::kConnecting};
    bool reading_{false};
    bool received_eof_{false};
    bool shutdown_requested_{false};
    bool write_shutdown_{false};
    Buffer input_;
    Buffer output_;
    MessageCallback message_callback_;
    EventCallback write_complete_callback_;
    EventCallback eof_callback_;
    EventCallback close_callback_;
};

}  // namespace net
}  // namespace l4lb
