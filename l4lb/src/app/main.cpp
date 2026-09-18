#include "lb/LoadBalancer.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/UniqueFd.h"

#include <csignal>
#include <iostream>
#include <sys/signalfd.h>
#include <unistd.h>

int main(int argc, char*[]) {
    if (argc != 1) {
        std::cerr << "Teaching version: edit addresses in src/app/main.cpp; no JSON argument.\n";
        return 2;
    }
    try {
        // 教学版的全部配置：一个监听地址 + 一个后端列表。
        l4lb::net::EventLoop loop;
        l4lb::lb::LoadBalancer server(&loop, {"127.0.0.1", 9000},
            {{"127.0.0.1", 9101}, {"127.0.0.1", 9102}, {"127.0.0.1", 9103}});

        // 通过 signalfd 在正常事件回调中退出，不在异步信号处理器里操作 C++ 对象。
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
            throw std::runtime_error("sigprocmask failed");
        }
        l4lb::net::UniqueFd signal_fd(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
        if (!signal_fd) throw std::runtime_error("signalfd failed");
        l4lb::net::Channel signals(&loop, signal_fd.Get());
        signals.SetReadCallback([&] {
            signalfd_siginfo info{};
            while (::read(signal_fd.Get(), &info, sizeof(info)) == sizeof(info)) {}
            loop.Quit();
        });
        signals.EnableReading();
        try {
            server.Start();
            std::cout << "l4lb listening on " << server.ListenAddress().ToString()
                      << " (single loop, round robin)" << std::endl;
            loop.Loop();
        } catch (...) {
            signals.Remove();
            throw;
        }
        signals.Remove();
        server.Stop();  // 立即关闭剩余连接；教学版没有退出宽限期。
    } catch (const std::exception& error) {
        std::cerr << "l4lb: " << error.what() << '\n';
        return 1;
    }
}
