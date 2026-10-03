// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../../app/src/picoXr/cpp/profiler_core.h"
#include <spatial/core/utils/ProfilerRing.h>
#include <spatial/core/utils/LiteTrace.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <future>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>
using spatial::ProfilerRing;
using spatial::LiteTrace;
using Clock = std::chrono::steady_clock;
static void check(bool value) { if (!value) throw std::runtime_error("profiler contract failed"); }
static volatile uint64_t sink = 0;
static double bench(int count, bool scope, bool detail) {
    auto start = Clock::now();
    for (int i = 0; i < count; ++i) {
        if (scope) { XrProfileScope p("probe.pair", detail); sink = sink + 1; }
        else sink = sink + 1;
    }
    return std::chrono::duration<double, std::nano>(Clock::now()-start).count()/count;
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "probe <directory> bench|fixture|fixture_stale\n"; return 2; }
    std::cerr << "probe_pid=" << getpid() << "\n";
    ProfilerRing::Initialize("XRGameProfilerProbe", argv[1], false, 1024*1024, true);
    ProfilerRing::SetAppInfo("synthetic=instrumentation-contract;not-gameplay;no-source-frames");
    if (std::string(argv[2]) == "bench") {
        for (const auto* mode : {"baseline", "ring_off", "coarse", "detail_filtered", "detail", "instrumentation_off"}) {
            ProfilerRing::SetEnabled(std::string(mode) != "ring_off" && std::string(mode) != "baseline");
            xrgameProfileCommand({std::string(mode) == "detail" ? "detail" : std::string(mode) == "instrumentation_off" ? "off" : "coarse"});
            const bool detailed = std::string(mode) == "detail" || std::string(mode) == "detail_filtered";
            std::vector<double> times;
            bench(1000, std::string(mode) != "baseline", detailed);
            for (int r=0;r<7;++r) times.push_back(bench(100000, std::string(mode) != "baseline", detailed));
            std::sort(times.begin(), times.end());
            std::cout << "{\"mode\":\"" << mode << "\",\"iterationsPerRound\":100000,\"rounds\":7,\"medianNsPerPair\":" << times[3]
                      << ",\"minNsPerPair\":" << times.front() << ",\"maxNsPerPair\":" << times.back() << "}\n";
        }
        ProfilerRing::SetEnabled(false);
        return 0;
    }
    const bool testStale = std::string(argv[2]) == "fixture_stale";
    if (std::string(argv[2]) != "fixture" && !testStale) return 2;
    check(!xrgame_profile_begin("fixture.off", false));
    check(xrgameProfileCommand({"invalid"}).find("error") != std::string::npos);
    ProfilerRing::SetEnabled(true);
    LiteTrace::trackDef(2, "Fixture cross-thread lifecycle");
    // This pinned SDK snapshots live thread rings. Keep the producer registered
    // until the dump is frozen; an exited-thread fixture is retained as negative evidence.
    std::promise<void> consumerDone, releaseConsumer;
    auto release = releaseConsumer.get_future();
    std::thread consumer;
    {
        XrProfileScope outer("fixture.outer");
        { XrProfileScope filtered("fixture.detail_filtered", true); }
        check(xrProfileCall("fixture.return", [] { return 37; }) == 37);
        try { xrProfileCall("fixture.throw", []() -> int { throw 42; }); } catch (int) {}
        auto token = xrgame_profile_region("fixture.queue_to_complete");
        consumer = std::thread([token, &consumerDone, &release] {
            { XrProfileScope work("fixture.worker"); xrgame_profile_region_end(token); }
            consumerDone.set_value();
            release.wait();
        });
        consumerDone.get_future().wait();
    }
    xrgameProfileCommand({"detail"});
    { XrProfileScope scope("fixture.detail", true); scope.end(); scope.end(); }
    if (testStale) {
        auto stale = xrgame_profile_region("fixture.stale_region");
        auto staleScope = xrgame_profile_begin("fixture.stale_scope", false);
        ProfilerRing::SetEnabled(false);
        ProfilerRing::SetEnabled(true);
        xrgame_profile_region_end(stale); xrgame_profile_end(staleScope);
    }
    { XrProfileScope fresh("fixture.fresh_after_generation"); }
    xrgameProfileCommand({"off"});
    check(!xrgame_profile_begin("fixture.instrumentation_off", false));
    ProfilerRing::SetEnabled(false);
    std::cout << ProfilerRing::RequestDump(0);
    int result = 1;
    for (int i=0;i<100;++i) {
        const auto state=ProfilerRing::Status();
        if (state.find("dump_state=ready") != std::string::npos) { std::cout << state; result = 0; break; }
        if (state.find("dump_state=failed") != std::string::npos) { std::cerr << state; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    releaseConsumer.set_value();
    consumer.join();
    return result;
}
