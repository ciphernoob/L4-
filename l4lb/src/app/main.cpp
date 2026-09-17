#include <iostream>

#ifdef L4LB_HAS_CONFIG

#include "app/L4ProxyServer.h"
#include "config/Config.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/UniqueFd.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <functional>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <sys/signalfd.h>
#include <unistd.h>

namespace {

class SignalWatcher {
public:
    SignalWatcher(l4lb::net::EventLoop* loop, std::function<void()> callback)
        : loop_(loop), callback_(std::move(callback)) {
        sigemptyset(&mask_);
        sigaddset(&mask_, SIGINT);
        sigaddset(&mask_, SIGTERM);
        if (::pthread_sigmask(SIG_BLOCK, &mask_, &previous_mask_) != 0) {
            throw std::runtime_error("pthread_sigmask failed");
        }
        fd_.Reset(::signalfd(-1, &mask_, SFD_NONBLOCK | SFD_CLOEXEC));
        if (!fd_) {
            ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
            throw std::runtime_error(std::string("signalfd failed: ") +
                                     std::strerror(errno));
        }
        channel_.reset(new l4lb::net::Channel(loop_, fd_.Get()));
        channel_->SetReadCallback([this] { HandleSignal(); });
        channel_->EnableReading();
    }

    ~SignalWatcher() {
        channel_->Remove();
        channel_.reset();
        fd_.Reset();
        ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    }

private:
    void HandleSignal() {
        signalfd_siginfo info{};
        while (::read(fd_.Get(), &info, sizeof(info)) ==
               static_cast<ssize_t>(sizeof(info))) {
            if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM) {
                callback_();
            }
        }
    }

    l4lb::net::EventLoop* loop_;
    std::function<void()> callback_;
    sigset_t mask_{};
    sigset_t previous_mask_{};
    l4lb::net::UniqueFd fd_;
    std::unique_ptr<l4lb::net::Channel> channel_;
};

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <config.json>\n";
        return 2;
    }
    try {
        const l4lb::config::ServerConfig config =
            l4lb::config::LoadConfigFile(argv[1]);
        l4lb::net::EventLoop loop;
        l4lb::app::L4ProxyServer* server_pointer = nullptr;
        SignalWatcher signals(&loop, [&server_pointer] {
            if (server_pointer != nullptr) {
                server_pointer->BeginShutdown();
            }
        });
        l4lb::app::L4ProxyServer server(&loop, config);
        server_pointer = &server;
        server.Start();
        std::cout << "l4lb started: data=" << server.ListenAddress().ToString()
                  << " admin=" << server.AdminAddress().ToString()
                  << " workers=" << config.workers
                  << " algorithm=" << l4lb::config::AlgorithmName(config.algorithm)
                  << " backends=" << config.backends.size() << std::endl;
        loop.Loop();
        server.Stop();
        std::cout << "l4lb stopped\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "l4lb startup failed: " << error.what() << '\n';
        return 1;
    }
}

#else

int main() {
    std::cerr << "l4lb was built without jsoncpp; configuration support is unavailable\n";
    return 1;
}

#endif
