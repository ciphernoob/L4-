#include "Test.h"

#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/UniqueFd.h"

#include <atomic>
#include <future>
#include <memory>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <thread>

using l4lb::net::Channel;
using l4lb::net::EventLoop;
using l4lb::net::UniqueFd;

L4LB_TEST(ChannelSkipsCallbacksAfterTiedOwnerExpires) {
    EventLoop loop;
    UniqueFd event_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    L4LB_REQUIRE(event_fd.IsValid());
    Channel channel(&loop, event_fd.Get());
    int calls = 0;
    channel.SetReadCallback([&calls] { ++calls; });

    std::shared_ptr<int> owner(new int(1));
    channel.Tie(owner);
    channel.SetReadyEvents(EPOLLIN);
    channel.HandleEvent();
    L4LB_REQUIRE(calls == 1);

    owner.reset();
    channel.HandleEvent();
    L4LB_REQUIRE(calls == 1);
}

L4LB_TEST(ChannelChangesInterestAndRemovesBeforeFdClose) {
    EventLoop loop;
    UniqueFd event_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    Channel channel(&loop, event_fd.Get());
    channel.EnableReading();
    L4LB_REQUIRE(channel.IsAdded());
    L4LB_REQUIRE(channel.IsReading());
    channel.EnableWriting();
    L4LB_REQUIRE(channel.IsWriting());
    channel.DisableWriting();
    L4LB_REQUIRE(!channel.IsWriting());
    channel.Remove();
    L4LB_REQUIRE(!channel.IsAdded());
}

L4LB_TEST(EventLoopWakesForCrossThreadWorkAndQuits) {
    std::promise<EventLoop*> ready;
    std::future<EventLoop*> loop_future = ready.get_future();
    std::atomic<int> calls{0};
    std::thread worker([&ready] {
        EventLoop loop;
        ready.set_value(&loop);
        loop.Loop();
    });

    EventLoop* loop = loop_future.get();
    loop->QueueInLoop([loop, &calls] {
        ++calls;
        loop->Quit();
    });
    worker.join();
    L4LB_REQUIRE(calls.load() == 1);
}

