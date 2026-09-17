#pragma once

#include "net/Buffer.h"
#include "net/Socket.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <system_error>

namespace l4lb {
namespace net {

class Channel;
class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    enum class State {
        kConnecting,
        kConnected,
        kDisconnected,
    };

    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback =
        std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)>;
    using EventCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using WatermarkCallback =
        std::function<void(const std::shared_ptr<TcpConnection>&, std::size_t)>;
    using ErrorCallback = std::function<void(const std::shared_ptr<TcpConnection>&,
                                             const std::error_code&)>;

    TcpConnection(EventLoop* loop, Socket socket, std::string name);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    void SetConnectionCallback(ConnectionCallback callback) {
        connection_callback_ = std::move(callback);
    }
    void SetMessageCallback(MessageCallback callback) { message_callback_ = std::move(callback); }
    void SetWriteCompleteCallback(EventCallback callback) {
        write_complete_callback_ = std::move(callback);
    }
    void SetHighWatermarkCallback(WatermarkCallback callback) {
        high_watermark_callback_ = std::move(callback);
    }
    void SetLowWatermarkCallback(WatermarkCallback callback) {
        low_watermark_callback_ = std::move(callback);
    }
    void SetEofCallback(EventCallback callback) { eof_callback_ = std::move(callback); }
    void SetErrorCallback(ErrorCallback callback) { error_callback_ = std::move(callback); }
    void SetCloseCallback(EventCallback callback) { close_callback_ = std::move(callback); }

    void SetWatermarks(std::size_t low, std::size_t high);
    void SetMaxInputBufferBytes(std::size_t maximum);
    void Establish();
    void Send(const void* data, std::size_t length);
    void Send(const std::string& data) { Send(data.data(), data.size()); }
    void StartRead();
    void StopRead();
    void ShutdownWrite();
    void ForceClose();

    EventLoop* Loop() const noexcept { return loop_; }
    int Fd() const noexcept { return socket_.Fd(); }
    const std::string& Name() const noexcept { return name_; }
    State GetState() const noexcept { return state_; }
    bool IsReading() const noexcept { return reading_; }
    bool HasReceivedEof() const noexcept { return received_eof_; }
    std::size_t OutputBufferBytes() const noexcept { return output_buffer_.ReadableBytes(); }
    std::size_t InputBufferBytes() const noexcept { return input_buffer_.ReadableBytes(); }
    Buffer* InputBuffer() noexcept { return &input_buffer_; }

private:
    void SendInLoop(const char* data, std::size_t length);
    void HandleRead();
    void HandleWrite();
    void HandleClose();
    void HandleError(int error_number = 0);
    void NotifyWriteComplete();

    EventLoop* loop_;
    Socket socket_;
    const std::string name_;
    std::unique_ptr<Channel> channel_;
    State state_{State::kConnecting};
    bool reading_{false};
    bool received_eof_{false};
    bool shutdown_write_pending_{false};
    Buffer input_buffer_;
    Buffer output_buffer_;
    std::size_t max_input_buffer_bytes_{std::numeric_limits<std::size_t>::max()};
    std::size_t low_watermark_{64 * 1024};
    std::size_t high_watermark_{256 * 1024};
    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    EventCallback write_complete_callback_;
    WatermarkCallback high_watermark_callback_;
    WatermarkCallback low_watermark_callback_;
    EventCallback eof_callback_;
    ErrorCallback error_callback_;
    EventCallback close_callback_;
};

}  // namespace net
}  // namespace l4lb
