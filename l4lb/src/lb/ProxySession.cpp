#include "lb/ProxySession.h"

#include "lb/Connector.h"
#include "net/EventLoop.h"

#include <cassert>
#include <chrono>

namespace l4lb {
namespace lb {

ProxySession::ProxySession(net::EventLoop* loop, net::Socket client,
                           net::InetAddress backend)
    : loop_(loop), backend_address_(std::move(backend)),
      client_(std::make_shared<net::TcpConnection>(loop, std::move(client), "client")) {}

ProxySession::~ProxySession() {
    assert(closed_ && "owner must Close the session before releasing it");
}

void ProxySession::Start() {
    const std::weak_ptr<ProxySession> weak = shared_from_this();
    client_->SetMessageCallback([weak](const net::TcpConnection::Ptr&, net::Buffer* input) {
        if (auto session = weak.lock()) session->OnClientMessage(input);
    });
    client_->SetEofCallback([weak](const net::TcpConnection::Ptr&) {
        if (auto session = weak.lock()) session->OnClientEof();
    });
    client_->SetCloseCallback([weak](const net::TcpConnection::Ptr&) {
        if (auto session = weak.lock()) session->Close();
    });
    client_->SetWriteCompleteCallback([weak](const net::TcpConnection::Ptr&) {
        if (auto session = weak.lock()) {
            if (!session->closed_ && session->backend_) session->backend_->StartRead();
            session->CheckFinished();
        }
    });
    // 后端尚未接通，不读客户端；早到的数据留在内核 TCP 接收缓冲区。
    client_->Establish(false);
    connector_ = std::make_shared<Connector>(
        loop_, backend_address_, std::chrono::seconds(3));
    connector_->SetSuccessCallback([weak](const std::shared_ptr<Connector>&, net::Socket socket) {
        if (auto session = weak.lock()) session->OnBackendConnected(std::move(socket));
    });
    connector_->SetErrorCallback([weak](const std::shared_ptr<Connector>&, const std::error_code&) {
        if (auto session = weak.lock()) session->Close();
    });
    connector_->Start();
}

void ProxySession::OnBackendConnected(net::Socket socket) {
    if (closed_) return;
    backend_ = std::make_shared<net::TcpConnection>(loop_, std::move(socket), "backend");
    const std::weak_ptr<ProxySession> weak = shared_from_this();
    backend_->SetMessageCallback([weak](const net::TcpConnection::Ptr&, net::Buffer* input) {
        if (auto session = weak.lock()) session->OnBackendMessage(input);
    });
    backend_->SetEofCallback([weak](const net::TcpConnection::Ptr&) {
        if (auto session = weak.lock()) session->OnBackendEof();
    });
    backend_->SetCloseCallback([weak](const net::TcpConnection::Ptr&) {
        if (auto session = weak.lock()) session->Close();
    });
    backend_->SetWriteCompleteCallback([weak](const net::TcpConnection::Ptr&) {
        if (auto session = weak.lock()) {
            if (!session->closed_) session->client_->StartRead();
            session->CheckFinished();
        }
    });
    backend_->Establish();
    client_->StartRead();
    connector_.reset();  // Connector 已把 Socket 的所有权交给 backend_。
}

void ProxySession::OnClientMessage(net::Buffer* input) {
    if (closed_) return;
    backend_->Send(input->Peek(), input->ReadableBytes());
    input->RetrieveAll();
    if (!closed_ && backend_->OutputBufferBytes() != 0) {
        client_->StopRead();  // 后端没写完，先不读下一批客户端数据。
    }
}

void ProxySession::OnBackendMessage(net::Buffer* input) {
    if (closed_) return;
    client_->Send(input->Peek(), input->ReadableBytes());
    input->RetrieveAll();
    if (!closed_ && client_->OutputBufferBytes() != 0) {
        backend_->StopRead();
    }
}

void ProxySession::OnClientEof() {
    if (closed_) return;
    backend_->ShutdownWrite();  // 已缓存的数据排空后才发送 FIN。
    CheckFinished();
}

void ProxySession::OnBackendEof() {
    if (closed_) return;
    client_->ShutdownWrite();
    CheckFinished();
}

void ProxySession::CheckFinished() {
    if (!closed_ && backend_ && client_->HasReceivedEof() && backend_->HasReceivedEof() &&
        client_->OutputBufferBytes() == 0 && backend_->OutputBufferBytes() == 0) {
        Close();
    }
}

void ProxySession::Close() {
    if (closed_) return;
    closed_ = true;  // 必须先标记；关闭两端会再次进入关闭回调。
    const auto self = shared_from_this();
    if (connector_) connector_->Cancel();
    client_->ForceClose();
    if (backend_) backend_->ForceClose();
    // 保留到当前事件批次结束，包括取消中的 Connector/Channel。
    loop_->QueueInLoop([self] {});
    if (on_close_) on_close_();
}

}  // namespace lb
}  // namespace l4lb
