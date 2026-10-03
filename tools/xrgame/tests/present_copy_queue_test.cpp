#include "../../../app/src/main/cpp/winlator/PresentCopyQueue.h"
#include <cassert>
#include <future>
#include <iostream>
#include <thread>

using Q = PresentCopyQueue;

#ifdef XRGAME_PROFILE
// Exercise instrumentation with the existing ownership/failure/concurrency matrix.
// No SDK is linked: the release build below must also compile without these exports.
static std::atomic<unsigned> regionBegins{0}, regionEnds{0}, scopeBegins{0}, scopeEnds{0};
extern "C" uint64_t xrgame_profile_begin(const char*, bool detail) noexcept {
    assert(detail); ++scopeBegins; return 1;
}
extern "C" void xrgame_profile_end(uint64_t token) noexcept { assert(token); ++scopeEnds; }
extern "C" XrProfileRegion xrgame_profile_region(const char*) noexcept {
    return {++regionBegins, 1};
}
extern "C" void xrgame_profile_region_end(XrProfileRegion token) noexcept { assert(token.cookie); ++regionEnds; }
#endif

int main() {
    // Hold the simulated GPU fence. Admission and unrelated queries must progress,
    // with neither early completion nor capacity freed merely by dequeueing.
    Q queue(2);
    std::promise<void> entered, release;
    auto fence = release.get_future().share();
    int completed = 0, destroyed = 0;
    auto buffer = std::shared_ptr<int>(new int(1), [&](int* p) { ++destroyed; delete p; });
    assert(queue.push(10, [&, buffer](const Q::Ticket& ticket) {
        entered.set_value(); fence.wait();
        return ticket->load() ? Q::Copied : Q::Skipped;
    }, [&](Q::Result result) { assert(result == Q::Skipped); ++completed; }));
    buffer.reset();
    std::thread worker([&] { queue.runOne(); });
    entered.get_future().wait();
    assert(completed == 0 && destroyed == 0);
    queue.invalidate(10); // Destroy/reuse X drawable during an in-flight copy.
    assert(queue.push(10, [](const Q::Ticket& ticket) {
        assert(ticket->load()); return Q::Copied;
    }, [&](Q::Result result) { assert(result == Q::Copied); ++completed; }));
    assert(!queue.push(11, [](const Q::Ticket&) { return Q::Copied; }, [](Q::Result) {}));
    assert(completed == 0 && destroyed == 0);
    release.set_value(); worker.join();
    assert(completed == 1 && destroyed == 1);
    assert(queue.runOne() && completed == 2);

    // Queued cancellation must not execute GPU work; callbacks may reenter the
    // queue, and every retained owner is released even at shutdown.
    assert(queue.push(12, [](const Q::Ticket&) { assert(false); return Q::Failed; },
        [&](Q::Result result) { assert(result == Q::Skipped); queue.invalidate(12); ++completed; }));
    queue.close();
    assert(!queue.push(13, [](const Q::Ticket&) { return Q::Copied; }, [](Q::Result) {}));
    assert(queue.runOne() && completed == 3 && !queue.runOne());

    // A failed GPU wait is not completion proof. Fail closed, skip work that has
    // never reached the GPU, and never change the failing result to COPIED/SKIP.
    Q failed;
    assert(failed.push(1, [](const Q::Ticket&) { return Q::Failed; },
        [](Q::Result result) { assert(result == Q::Failed); }));
    assert(failed.push(2, [](const Q::Ticket&) { assert(false); return Q::Copied; },
        [](Q::Result result) { assert(result == Q::Skipped); }));
    failed.runOne();
    assert(!failed.push(3, [](const Q::Ticket&) { return Q::Copied; }, [](Q::Result) {}));
    failed.runOne();
    assert(!failed.hasWork());
#ifdef XRGAME_PROFILE
    // Rejected jobs create no region; copied, invalidated and failed jobs all retire one.
    assert(regionBegins == 5 && regionEnds == 5);
    assert(scopeBegins == 5 && scopeEnds == 5);
#endif
    // Submission returns while the first GPU read is still pending. A draw (and
    // another copy) can be submitted, but no callback/capacity/AHB is released.
    {
        Q pipeline(2);
        std::promise<void> waiting, gpuDone;
        auto gpu = gpuDone.get_future().share();
        std::atomic<int> notified{0}, released{0};
        auto held = std::shared_ptr<int>(new int(1), [&](int* p) { ++released; delete p; });
        assert(pipeline.pushDeferred(20, [&, held](const Q::Ticket&) -> Q::Retirement {
            return [&, held] { waiting.set_value(); gpu.wait(); return Q::Copied; };
        }, [&](Q::Result r) { assert(r == Q::Skipped); ++notified; }));
        held.reset();
        assert(pipeline.runOne());
        waiting.get_future().wait();
        assert(notified == 0 && released == 0); // Simulated draw can now be submitted.
        pipeline.invalidate(20);
        assert(pipeline.pushDeferred(20, [](const Q::Ticket& t) -> Q::Retirement {
            assert(t->load()); return [] { return Q::Copied; };
        }, [&](Q::Result r) { assert(r == Q::Copied); ++notified; }));
        assert(pipeline.runOne());
        assert(!pipeline.push(21, [](const Q::Ticket&) { return Q::Copied; }, [](Q::Result) {}));
        assert(notified == 0 && released == 0);
        gpuDone.set_value();
        // Wait on a callback boundary without destroying/invalidation of generation 2.
        while (notified.load() != 2) std::this_thread::yield();
        pipeline.shutdown();
        assert(released == 1);
    }
    // Failure invalidates generations, but a second already-submitted copy must
    // still wait for its own retirement proof before SKIP, including at shutdown.
    {
        Q pipeline;
        std::promise<void> fail, secondEntered, secondDone;
        auto firstFence = fail.get_future().share(), secondFence = secondDone.get_future().share();
        std::atomic<int> notified{0};
        assert(pipeline.pushDeferred(1, [&](const Q::Ticket&) -> Q::Retirement {
            return [=] { firstFence.wait(); return Q::Failed; };
        }, [&](Q::Result r) { assert(r == Q::Failed); ++notified; }));
        assert(pipeline.pushDeferred(2, [&](const Q::Ticket&) -> Q::Retirement {
            return [&] { secondEntered.set_value(); secondFence.wait(); return Q::Copied; };
        }, [&](Q::Result r) { assert(r == Q::Skipped); ++notified; }));
        pipeline.runOne(); pipeline.runOne();
        fail.set_value(); secondEntered.get_future().wait();
        assert(notified == 1);
        auto stopping = std::async(std::launch::async, [&] { pipeline.shutdown(); });
        assert(stopping.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        secondDone.set_value(); stopping.get();
        assert(notified == 2);
    }
#ifdef XRGAME_PROFILE
    assert(regionBegins == 9 && regionEnds == 9);
    assert(scopeBegins == 9 && scopeEnds == 9);
#endif
    std::cout << "PASS: fence-held admission, capacity, AHB ownership, generation reuse, shutdown, failure\n";
}
