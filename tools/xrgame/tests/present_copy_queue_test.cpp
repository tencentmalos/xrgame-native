#include "../../../app/src/main/cpp/winlator/PresentCopyQueue.h"
#include <cassert>
#include <future>
#include <iostream>
#include <thread>

using Q = PresentCopyQueue;

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
    std::cout << "PASS: fence-held admission, capacity, AHB ownership, generation reuse, shutdown, failure\n";
}
