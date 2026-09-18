#include "Test.h"
#include "lb/LoadBalancer.h"
#include "net/EventLoop.h"
#include "net/TimerId.h"

#include <sys/socket.h>

using namespace l4lb::net;
using l4lb::lb::LoadBalancer;

L4LB_TEST(LoadBalancerRejectsEmptyBackendList) {
    EventLoop loop;
    bool rejected = false;
    try {
        LoadBalancer server(&loop, {"127.0.0.1", 0}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    L4LB_REQUIRE(rejected);
}

L4LB_TEST(LoadBalancerReleasesSessionWhenBackendRefuses) {
    EventLoop loop;
    // 占用一个没有 listen 的端口，确保 connect 得到拒绝且不会被其他进程抢占。
    Socket reserved = Socket::CreateTcpNonBlocking();
    reserved.Bind({"127.0.0.1", 0});
    LoadBalancer server(&loop, {"127.0.0.1", 0}, {reserved.LocalAddress()});
    server.Start();
    Socket client = Socket::CreateTcpNonBlocking();
    const auto address = server.ListenAddress().SockAddr();
    const int result = ::connect(client.Fd(), reinterpret_cast<const sockaddr*>(&address),
                                 sizeof(address));
    L4LB_REQUIRE(result == 0 || errno == EINPROGRESS);
    bool finished = false;
    TimerId check = loop.RunEvery(std::chrono::milliseconds(5), [&] {
        char byte;
        const auto count = ::recv(client.Fd(), &byte, 1, 0);
        if (count == 0 || (count < 0 && errno == ECONNRESET)) {
            finished = true;
            L4LB_REQUIRE(server.SessionCount() == 0);
            loop.Quit();
        }
    });
    TimerId timeout = loop.RunAfter(std::chrono::seconds(2), [&] { loop.Quit(); });
    loop.Loop();
    server.Stop();
    L4LB_REQUIRE(finished);
}
