#include "net/TcpConnection.h"

#include "net/Channel.h"
#include "net/EventLoop.h"

#include <cerrno>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace l4lb {
namespace net {

constexpr std::size_t TcpConnection::kReadBatchSize;

TcpConnection::TcpConnection(EventLoop* loop, Socket socket, std::string name)
    : loop_(loop), socket_(std::move(socket)), name_(std::move(name)),
      channel_(new Channel(loop, socket_.Fd())) {
    loop_->AssertInLoopThread();
    if (!socket_.IsValid()) {
        throw std::invalid_argument("TcpConnection requires a valid socket");
    }
    channel_->SetReadCallback([this] { HandleRead(); });
    channel_->SetWriteCallback([this] { HandleWrite(); });
    channel_->SetCloseCallback([this] { ForceClose(); });
    channel_->SetErrorCallback([this] { ForceClose(); });
}

TcpConnection::~TcpConnection() {
    if (channel_->IsAdded()) {
        std::terminate();  // 必须先在 loop 中 ForceClose，再销毁对象。
    }
}

void TcpConnection::Establish(bool start_reading) {
    loop_->AssertInLoopThread();
    if (state_ != State::kConnecting) {
        throw std::logic_error("connection already established");
    }
    state_ = State::kConnected;
    channel_->Tie(shared_from_this());
    if (start_reading) {
        StartRead();
    }
}

void TcpConnection::StartRead() {
    if (state_ == State::kConnected && !received_eof_ && !reading_) {
        reading_ = true;
        channel_->EnableReading();  // 默认 LT；仍有数据时会继续通知。
    }
}

void TcpConnection::StopRead() {
    if (reading_) {
        reading_ = false;
        channel_->DisableReading();
    }
}

void TcpConnection::HandleRead() {
    // 另一个回调可能已暂停读取，或关闭了本批次中的这个连接。
    if (state_ != State::kConnected || !reading_) {
        return;
    }
    char bytes[kReadBatchSize];
    ssize_t count;
    do {
        count = ::read(socket_.Fd(), bytes, sizeof(bytes));
    } while (count < 0 && errno == EINTR);

    if (count > 0) {
        input_.Append(bytes, static_cast<std::size_t>(count));
        if (message_callback_) {
            message_callback_(shared_from_this(), &input_);
        }
        // 一次只读一批，让业务回调有机会暂停来源端。
    } else if (count == 0) {
        received_eof_ = true;
        StopRead();
        if (eof_callback_) {
            eof_callback_(shared_from_this());
        }
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ForceClose();
    }
}

void TcpConnection::Send(const void* data, std::size_t length) {
    loop_->AssertInLoopThread();
    if (state_ != State::kConnected || shutdown_requested_ || length == 0) {
        return;
    }
    // 优先直接写；部分写和 EAGAIN 的剩余字节交给 EPOLLOUT。
    std::size_t sent = 0;
    if (output_.ReadableBytes() == 0) {
        ssize_t count;
        do {
            count = ::send(socket_.Fd(), data, length, MSG_NOSIGNAL);
        } while (count < 0 && errno == EINTR);
        if (count >= 0) {
            sent = static_cast<std::size_t>(count);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ForceClose();
            return;
        }
    }
    if (sent < length) {
        output_.Append(static_cast<const char*>(data) + sent, length - sent);
        channel_->EnableWriting();
    }
}

void TcpConnection::HandleWrite() {
    if (state_ != State::kConnected || output_.ReadableBytes() == 0) {
        return;
    }
    ssize_t count;
    do {
        count = ::send(socket_.Fd(), output_.Peek(), output_.ReadableBytes(), MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count > 0) {
        output_.Retrieve(static_cast<std::size_t>(count));
        if (output_.ReadableBytes() == 0) {
            channel_->DisableWriting();
            FinishWrite();
            if (state_ == State::kConnected && write_complete_callback_) {
                write_complete_callback_(shared_from_this());
            }
        }
    } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        ForceClose();
    }
}

void TcpConnection::ShutdownWrite() {
    if (state_ == State::kConnected) {
        shutdown_requested_ = true;
        FinishWrite();
    }
}

void TcpConnection::FinishWrite() {
    if (shutdown_requested_ && !write_shutdown_ && output_.ReadableBytes() == 0) {
        write_shutdown_ = true;
        socket_.ShutdownWrite();
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
    // epoll 本批次可能还保存着本 Channel 的指针，批次结束后才允许析构。
    const auto self = shared_from_this();
    loop_->QueueInLoop([self] {});
    if (close_callback_) {
        close_callback_(self);
    }
}

}  // namespace net
}  // namespace l4lb
