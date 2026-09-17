#include "net/Acceptor.h"

#include "net/Channel.h"
#include "net/EventLoop.h"

#include <cerrno>
#include <fcntl.h>
#include <system_error>

namespace l4lb {
namespace net {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& address, bool edge_triggered)
    : loop_(loop),
      listen_socket_(Socket::CreateTcpNonBlocking()),
      listen_address_(address),
      idle_fd_(OpenIdleFd()),
      edge_triggered_(edge_triggered) {
    loop_->AssertInLoopThread();
    listen_socket_.SetReuseAddress(true);
    listen_socket_.Bind(address);
    listen_socket_.Listen();
    listen_address_ = listen_socket_.LocalAddress();
    channel_.reset(new Channel(loop_, listen_socket_.Fd()));
    channel_->SetReadCallback([this] { HandleRead(); });
}

Acceptor::~Acceptor() {
    Stop();
}

void Acceptor::Start() {
    loop_->AssertInLoopThread();
    if (listening_) {
        return;
    }
    listening_ = true;
    channel_->EnableReading();
    if (edge_triggered_) {
        channel_->EnableEdgeTriggered();
    }
}

void Acceptor::Stop() {
    loop_->AssertInLoopThread();
    if (!listening_ && !channel_->IsAdded()) {
        return;
    }
    listening_ = false;
    channel_->Remove();
}

void Acceptor::HandleRead() {
    loop_->AssertInLoopThread();
    while (true) {
        try {
            InetAddress peer("0.0.0.0", 0);
            Socket connection = listen_socket_.Accept(&peer);
            if (!connection.IsValid()) {
                return;
            }
            if (new_connection_callback_) {
                new_connection_callback_(std::move(connection), peer);
            }
        } catch (const std::system_error& error) {
            const int value = error.code().value();
            if (value == EINTR) {
                continue;
            }
            if (value == EMFILE || value == ENFILE) {
                HandleDescriptorExhaustion();
            }
            ReportError(error.code());
            return;
        }
    }
}

void Acceptor::HandleDescriptorExhaustion() {
    idle_fd_.Reset();
    try {
        Socket dropped = listen_socket_.Accept();
        (void)dropped;
    } catch (const std::system_error&) {
    }
    idle_fd_ = OpenIdleFd();
}

void Acceptor::ReportError(const std::error_code& error) {
    if (error_callback_) {
        error_callback_(error);
    }
}

UniqueFd Acceptor::OpenIdleFd() {
    return UniqueFd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
}

}  // namespace net
}  // namespace l4lb

