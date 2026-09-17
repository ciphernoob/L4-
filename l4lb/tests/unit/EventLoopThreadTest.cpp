#include "Test.h"

#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/EventLoopThreadPool.h"

#include <future>
#include <stdexcept>
#include <thread>

using l4lb::net::EventLoop;
using l4lb::net::EventLoopThread;
using l4lb::net::EventLoopThreadPool;

L4LB_TEST(EventLoopThreadRunsTasksOnOwnedThreadAndJoins) {
    const std::thread::id caller = std::this_thread::get_id();
    std::thread::id worker_id;
    {
        EventLoopThread thread;
        EventLoop* loop = thread.StartLoop();
        std::promise<void> finished;
        std::future<void> future = finished.get_future();
        loop->QueueInLoop([&] {
            L4LB_REQUIRE(loop->IsInLoopThread());
            worker_id = std::this_thread::get_id();
            finished.set_value();
        });
        future.get();
    }
    L4LB_REQUIRE(worker_id != caller);
}

L4LB_TEST(EventLoopThreadRejectsRepeatedStart) {
    EventLoopThread thread;
    thread.StartLoop();
    bool threw = false;
    try {
        thread.StartLoop();
    } catch (const std::logic_error&) {
        threw = true;
    }
    L4LB_REQUIRE(threw);
}

L4LB_TEST(EventLoopThreadPoolAssignsLoopsRoundRobin) {
    EventLoop base;
    EventLoopThreadPool pool(&base);
    pool.Start(2);
    EventLoop* first = pool.NextLoop();
    EventLoop* second = pool.NextLoop();
    EventLoop* third = pool.NextLoop();
    L4LB_REQUIRE(first != second);
    L4LB_REQUIRE(first == third);

    std::promise<bool> first_result;
    std::promise<bool> second_result;
    std::future<bool> first_future = first_result.get_future();
    std::future<bool> second_future = second_result.get_future();
    first->QueueInLoop([first, &first_result] { first_result.set_value(first->IsInLoopThread()); });
    second->QueueInLoop(
        [second, &second_result] { second_result.set_value(second->IsInLoopThread()); });
    L4LB_REQUIRE(first_future.get());
    L4LB_REQUIRE(second_future.get());
    pool.Stop();
    L4LB_REQUIRE(!pool.IsStarted());
}

