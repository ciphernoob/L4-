#include "Test.h"

#include "net/EventLoop.h"
#include "net/TimerId.h"

#include <chrono>
#include <memory>

using l4lb::net::EventLoop;
using l4lb::net::TimerId;
using namespace std::chrono;

L4LB_TEST(OneShotTimerRunsAndStopsLoop) {
    EventLoop loop;
    int calls = 0;
    TimerId timer = loop.RunAfter(milliseconds(5), [&] {
        ++calls;
        loop.Quit();
    });
    loop.Loop();
    L4LB_REQUIRE(calls == 1);
    L4LB_REQUIRE(!timer.IsCancelled());
}

L4LB_TEST(ScopedTimerCancellationIsAutomaticAndIdempotent) {
    EventLoop loop;
    int cancelled_calls = 0;
    {
        TimerId cancelled = loop.RunAfter(milliseconds(1), [&] { ++cancelled_calls; });
        cancelled.Cancel();
        cancelled.Cancel();
    }
    TimerId stopper = loop.RunAfter(milliseconds(10), [&] { loop.Quit(); });
    loop.Loop();
    L4LB_REQUIRE(cancelled_calls == 0);
    L4LB_REQUIRE(!stopper.IsCancelled());
}

L4LB_TEST(RepeatingTimerCanCancelItself) {
    EventLoop loop;
    int calls = 0;
    std::unique_ptr<TimerId> repeating;
    repeating.reset(new TimerId(loop.RunEvery(milliseconds(2), [&] {
        ++calls;
        if (calls == 3) {
            repeating->Cancel();
            loop.Quit();
        }
    })));
    loop.Loop();
    L4LB_REQUIRE(calls == 3);
    L4LB_REQUIRE(repeating->IsCancelled());
}

