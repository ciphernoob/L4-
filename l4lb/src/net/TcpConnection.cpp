#include "net/TcpConnection.h"

#include "net/Channel.h"
#include "net/EventLoop.h"

#include <cerrno>
#include <algorithm>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace l4lb {
namespace net {

TcpConnection::TcpConnection(EventLoop* loop, Socket socket, std::string name)
    : loop_(loop), socket_(std::move(socket)), name_(std::move(name)) {
    loop_->AssertInLoopThread();
    if (!socket_.IsValid()) {
        throw std::invalid_argument("TcpConnection requires a valid socket");
    }
    channel_.reset(new Channel(loop_, socket_.Fd()));
    channel_->SetReadCallback([this] { HandleRead(); });
    channel_->SetWriteCallback([this] { HandleWrite(); });
    channel_->SetCloseCallback([this] { HandleClose(); });
    channel_->SetErrorCallback([this] { HandleError(); });
}

TcpConnection::~TcpConnection() {
    if (channel_->IsAdded()) {
        std::terminate();
    }
}

void TcpConnection::SetWatermarks(std::size_t low, std::size_t high) {
    loop_->AssertInLoopThread();
    if (low >= high) {
        throw std::invalid_argument("low watermark must be less than high watermark");
    }
    low_watermark_ = low;
    high_watermark_ = high;
}

void TcpConnection::SetMaxInputBufferBytes(std::size_t maximum) {
    loop_->AssertInLoopThread();
    if (maximum == 0 || maximum < input_buffer_.ReadableBytes()) {
        throw std::invalid_argument("input buffer limit is smaller than buffered data");
    }
    max_input_buffer_bytes_ = maximum;
}

void TcpConnection::Establish() {
    loop_->AssertInLoopThread();
    if (state_ != State::kConnecting) {
        throw std::logic_error("TcpConnection::Establish called in invalid state");
    }
    state_ = State::kConnected;
    channel_->Tie(shared_from_this());
    channel_->EnableReading();
    channel_->EnableEdgeTriggered();
    reading_ = true;
    if (connection_callback_) {
        connection_callback_(shared_from_this());
    }
}

void TcpConnection::Send(const void* data, std::size_t length) {
    if (length == 0 || state_ != State::kConnected) {
        return;
    }
    const char* bytes = static_cast<const char*>(data);
    if (loop_->IsInLoopThread()) {
        SendInLoop(bytes, length);
        return;
    }
    const std::string copy(bytes, length);
    const std::weak_ptr<TcpConnection> weak = shared_from_this();
    loop_->QueueInLoop([weak, copy] {
        if (const std::shared_ptr<TcpConnection> connection = weak.lock()) {
            connection->SendInLoop(copy.data(), copy.size());
        }
    });
}

void TcpConnection::StartRead() {
    loop_->AssertInLoopThread();
    if (state_ == State::kConnected && !reading_ && !received_eof_) {
        reading_ = true;
        channel_->EnableReading();
    }
}

void TcpConnection::StopRead() {
    loop_->AssertInLoopThread();
    if (reading_) {
        reading_ = false;
        channel_->DisableReading();
    }
}

void TcpConnection::ShutdownWrite() {
    loop_->AssertInLoopThread();
    if (state_ != State::kConnected || shutdown_write_pending_) {
        return;
    }
    shutdown_write_pending_ = true;
    if (output_buffer_.ReadableBytes() == 0) {
        socket_.ShutdownWrite();
        shutdown_write_pending_ = false;
    }
}

void TcpConnection::ForceClose() {
    loop_->AssertInLoopThread();
    if (state_ == State::kDisconnected) {
        return;
    }
    state_ = State::kDisconnected;
    reading_ = false;
    channel_->Remove();
    socket_.Close();
    if (close_callback_) {
        close_callback_(shared_from_this());
    }
}

void TcpConnection::SendInLoop(const char* data, std::size_t length) {
    loop_->AssertInLoopThread();
    if (state_ != State::kConnected) {
        return;
    }

    std::size_t remaining = length;
    std::size_t sent = 0;
    if (!channel_->IsWriting() && output_buffer_.ReadableBytes() == 0) {
        ssize_t result;
        do {
            result = ::send(socket_.Fd(), data, length, MSG_NOSIGNAL);
        } while (result < 0 && errno == EINTR);
        if (result >= 0) {
            sent = static_cast<std::size_t>(result);
            remaining -= sent;
            if (remaining == 0) {
                NotifyWriteComplete();
                return;
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            HandleError(errno);
            return;
        }
    }

    const std::size_t old_size = output_buffer_.ReadableBytes();
    output_buffer_.Append(data + sent, remaining);
    const std::size_t new_size = output_buffer_.ReadableBytes();
    if (old_size < high_watermark_ && new_size >= high_watermark_ &&
        high_watermark_callback_) {
        high_watermark_callback_(shared_from_this(), new_size);
    }
    if (!channel_->IsWriting()) {
        channel_->EnableWriting();
    }
}

void TcpConnection::HandleRead() {
    loop_->AssertInLoopThread();
    if (state_ == State::kDisconnected) {
        return;
    }
    char buffer[64 * 1024];
    bool received_data = false;
    while (true) {
        const std::size_t buffered = input_buffer_.ReadableBytes();
        if (buffered >= max_input_buffer_bytes_) {
            StopRead();
            break;
        }
        const std::size_t read_capacity =
            std::min(sizeof(buffer), max_input_buffer_bytes_ - buffered);
        const ssize_t result = ::read(socket_.Fd(), buffer, read_capacity);
        if (result > 0) {
            input_buffer_.Append(buffer, static_cast<std::size_t>(result));
            received_data = true;
            if (input_buffer_.ReadableBytes() >= max_input_buffer_bytes_) {
                StopRead();
                break;
            }
            continue;
        }
        if (result == 0) {
            received_eof_ = true;
            StopRead();
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        HandleError(errno);
        return;
    }
    if (received_data && message_callback_) {
        message_callback_(shared_from_this(), &input_buffer_);
    }
    if (received_eof_ && eof_callback_) {
        eof_callback_(shared_from_this());
    }
}

void TcpConnection::HandleWrite() {
    loop_->AssertInLoopThread();
    if (state_ == State::kDisconnected) {
        return;
    }
    const std::size_t old_size = output_buffer_.ReadableBytes();
    while (output_buffer_.ReadableBytes() > 0) {
        ssize_t result;
        do {
            result = ::send(socket_.Fd(), output_buffer_.Peek(),
                            output_buffer_.ReadableBytes(), MSG_NOSIGNAL);
        } while (result < 0 && errno == EINTR);
        if (result > 0) {
            output_buffer_.Retrieve(static_cast<std::size_t>(result));
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        HandleError(result < 0 ? errno : EPIPE);
        return;
    }

    const std::size_t drained_size = output_buffer_.ReadableBytes();
    if (old_size > low_watermark_ && drained_size <= low_watermark_ &&
        low_watermark_callback_) {
        low_watermark_callback_(shared_from_this(), drained_size);
    }
    if (state_ == State::kDisconnected) {
        return;
    }
    if (output_buffer_.ReadableBytes() == 0) {
        channel_->DisableWriting();
        NotifyWriteComplete();
        if (shutdown_write_pending_) {
            socket_.ShutdownWrite();
            shutdown_write_pending_ = false;
        }
    }
}

void TcpConnection::HandleClose() {
    ForceClose();
}

void TcpConnection::HandleError(int error_number) {
    if (error_number == 0) {
        try {
            error_number = socket_.GetSocketError();
        } catch (const std::system_error& error) {
            error_number = error.code().value();
        }
    }
    if (error_number == 0) {
        error_number = EIO;
    }
    if (error_callback_) {
        error_callback_(shared_from_this(),
                        std::error_code(error_number, std::generic_category()));
    }
    ForceClose();
}

void TcpConnection::NotifyWriteComplete() {
    if (write_complete_callback_) {
        write_complete_callback_(shared_from_this());
    }
}

}  // namespace net
}  // namespace l4lb
