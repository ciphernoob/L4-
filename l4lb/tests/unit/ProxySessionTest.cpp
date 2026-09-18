#include "Test.h"
#include "lb/ProxySession.h"
#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/TimerId.h"

#include <algorithm>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

using namespace l4lb::net;
using l4lb::lb::ProxySession;

namespace {
std::pair<Socket, UniqueFd> SocketPair() {
    int fds[2];
    L4LB_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
    return {Socket(UniqueFd(fds[0])), UniqueFd(fds[1])};
}

void SendMore(int fd, const std::string& data, std::size_t& offset) {
    if (offset == data.size()) return;
    const auto count = ::send(fd, data.data() + offset,
        std::min(std::size_t(65536), data.size() - offset), MSG_NOSIGNAL);
    if (count > 0) offset += static_cast<std::size_t>(count);
    else L4LB_REQUIRE(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}

void ReceiveMore(int fd, std::string& output, bool& eof) {
    if (eof) return;
    char bytes[65536];
    const auto count = ::read(fd, bytes, sizeof(bytes));
    if (count > 0) output.append(bytes, static_cast<std::size_t>(count));
    else if (count == 0) eof = true;
    else L4LB_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
}
}

// 两端都先停止读取，制造双向积压；恢复后必须完整传输，并保留双向 EOF。
L4LB_TEST(SessionBoundsBothDirectionsAndDrainsBinaryDataBeforeFin) {
    EventLoop loop;
    Acceptor acceptor(&loop, {"127.0.0.1", 0}, false);
    std::unique_ptr<Socket> peer;
    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        peer.reset(new Socket(std::move(socket)));
    });
    acceptor.Start();
    auto pair = SocketPair();
    auto session = std::make_shared<ProxySession>(&loop, std::move(pair.first),
                                                  acceptor.ListenAddress());
    int closes = 0;
    session->SetCloseCallback([&] { ++closes; });
    session->Start();
    L4LB_REQUIRE(!session->Client()->IsReading());

    std::string request(2 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < request.size(); ++i) request[i] = static_cast<char>(i * 131);
    const std::string response(request.rbegin(), request.rend());
    std::size_t sent_request = 0, sent_response = 0;
    // 后端 connect 完成前就发送，验证内核缓存的早到数据不会丢失。
    SendMore(pair.second.Get(), request, sent_request);
    std::string received_request, received_response;
    bool client_fin = false, backend_fin = false, client_eof = false, backend_eof = false;
    bool client_paused = false, backend_paused = false, configured = false, timeout = false;
    int ticks = 0;
    TimerId driver = loop.RunEvery(std::chrono::milliseconds(1), [&] {
        if (!peer || !session->Backend()) return;
        if (!configured) {
            const int small = 4096;
            L4LB_REQUIRE(::setsockopt(session->Client()->Fd(), SOL_SOCKET, SO_SNDBUF,
                                     &small, sizeof(small)) == 0);
            L4LB_REQUIRE(::setsockopt(session->Backend()->Fd(), SOL_SOCKET, SO_SNDBUF,
                                     &small, sizeof(small)) == 0);
            configured = true;
        }
        ++ticks;
        L4LB_REQUIRE(session->Client()->OutputBufferBytes() <= TcpConnection::kReadBatchSize);
        L4LB_REQUIRE(session->Backend()->OutputBufferBytes() <= TcpConnection::kReadBatchSize);
        L4LB_REQUIRE(session->Client()->InputBufferBytes() == 0);
        L4LB_REQUIRE(session->Backend()->InputBufferBytes() == 0);
        if (!session->IsClosed()) {
            client_paused |= !session->Client()->IsReading() &&
                              session->Backend()->OutputBufferBytes() > 0;
            backend_paused |= !session->Backend()->IsReading() &&
                               session->Client()->OutputBufferBytes() > 0;
        }
        if (!client_fin) {
            SendMore(pair.second.Get(), request, sent_request);
            if (sent_request == request.size()) {
                L4LB_REQUIRE(::shutdown(pair.second.Get(), SHUT_WR) == 0);
                client_fin = true;
            }
        }
        if (!backend_fin) {
            SendMore(peer->Fd(), response, sent_response);
            if (sent_response == response.size()) {
                L4LB_REQUIRE(::shutdown(peer->Fd(), SHUT_WR) == 0);
                backend_fin = true;
            }
        }
        // 至少 100ms 不读，然后每毫秒只读一批，覆盖慢读和暂停恢复。
        if (ticks >= 100) {
            ReceiveMore(pair.second.Get(), received_response, client_eof);
            ReceiveMore(peer->Fd(), received_request, backend_eof);
        }
        if (session->IsClosed() && client_eof && backend_eof) loop.Quit();
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(15), [&] {
        timeout = true;
        session->Close();
        loop.Quit();
    });
    loop.Loop();
    session->Close();
    session->Close();
    L4LB_REQUIRE(!timeout);
    L4LB_REQUIRE(closes == 1);
    L4LB_REQUIRE(client_paused && backend_paused);
    L4LB_REQUIRE(received_request == request);
    L4LB_REQUIRE(received_response == response);
}

L4LB_TEST(SessionResetClosesBothEndsOnce) {
    EventLoop loop;
    Acceptor acceptor(&loop, {"127.0.0.1", 0}, false);
    std::unique_ptr<Socket> peer;
    acceptor.SetNewConnectionCallback([&](Socket socket, const InetAddress&) {
        peer.reset(new Socket(std::move(socket)));
    });
    acceptor.Start();
    auto pair = SocketPair();
    auto session = std::make_shared<ProxySession>(&loop, std::move(pair.first),
                                                  acceptor.ListenAddress());
    int closes = 0;
    session->SetCloseCallback([&] { ++closes; loop.Quit(); });
    session->Start();
    bool reset = false, timeout = false;
    TimerId driver = loop.RunEvery(std::chrono::milliseconds(1), [&] {
        if (peer && session->Backend() && !reset) {
            const linger rst{1, 0};
            L4LB_REQUIRE(::setsockopt(peer->Fd(), SOL_SOCKET, SO_LINGER, &rst, sizeof(rst)) == 0);
            peer->Close();
            reset = true;
        }
    });
    TimerId watchdog = loop.RunAfter(std::chrono::seconds(2), [&] {
        timeout = true;
        session->Close();
        loop.Quit();
    });
    loop.Loop();
    session->Close();
    L4LB_REQUIRE(!timeout && reset && closes == 1);
    L4LB_REQUIRE(session->Client()->Fd() == -1);
    L4LB_REQUIRE(session->Backend()->Fd() == -1);
}
