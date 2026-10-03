// SPDX-License-Identifier: GPL-3.0-or-later
#include "profiler_core.h"
#include <spatial/core/utils/LiteTrace.h>
#include <atomic>

using spatial::LiteTrace;
using spatial::ProfilerRing;
namespace { std::atomic<int> detailLevel{1}; }

bool xrgameProfileEnabled(bool detail) noexcept {
    return detailLevel.load(std::memory_order_relaxed) >= (detail ? 2 : 1) && ProfilerRing::Enabled();
}

uint64_t xrgame_profile_begin(const char* name, bool detail) noexcept {
    if (!xrgameProfileEnabled(detail)) return 0;
    const auto generation = ProfilerRing::Generation();
    LiteTrace::begin(name);
    return generation + 1;
}
void xrgame_profile_end(uint64_t token) noexcept {
    if (token && token - 1 == ProfilerRing::Generation()) LiteTrace::end();
}
XrProfileRegion xrgame_profile_region(const char* name) noexcept {
    if (!xrgameProfileEnabled()) return {};
    const XrProfileRegion token{LiteTrace::regionCookieCreate(), ProfilerRing::Generation()};
    LiteTrace::regionBegin(name, token.cookie, 2);
    return token;
}
void xrgame_profile_region_end(XrProfileRegion token) noexcept {
    if (token.cookie && token.generation == ProfilerRing::Generation()) LiteTrace::regionEnd(token.cookie);
}

void xrgameProfileCounter(int id, int64_t value) noexcept {
    // Static names only: no paths, Steam tokens, per-file names or string interning per sample.
    static constexpr const char* names[] = {"steam.download.depot_bytes_done", "steam.download.depot_bytes_total",
        "steam.download.depots_done", "steam.download.depots_total", "steam.download.verifying",
        "steam.download.depot_id", "steam.download.run_sequence"};
    if (xrgameProfileEnabled() && id >= 0 && id < 7) LiteTrace::counter(names[id], value);
}

std::string xrgameProfileCommand(const std::vector<std::string>& args) {
    if (args.size() > 1 || (!args.empty() && args[0] != "off" && args[0] != "coarse" && args[0] != "detail"))
        return "{\"error\":\"usage: instrumentation [off|coarse|detail]\"}";
    if (!args.empty()) {
        const int next = args[0] == "off" ? 0 : args[0] == "detail" ? 2 : 1;
        detailLevel.store(next, std::memory_order_relaxed);
        // Mode transitions are explicit evidence. Do not synthesize a source frame.
        if (ProfilerRing::Enabled()) {
            LiteTrace::bookmark(next == 0 ? "instrumentation.off" : next == 1 ? "instrumentation.coarse" : "instrumentation.detail");
            LiteTrace::counter("host.instrumentation.level", next);
        }
    }
    const int level = detailLevel.load(std::memory_order_relaxed);
    return std::string("{\"schema\":1,\"mode\":\"") + (level == 0 ? "off" : level == 1 ? "coarse" : "detail") +
        "\",\"recording\":" + (ProfilerRing::Enabled() ? "true" : "false") +
        ",\"scope\":\"android-host\",\"frameSource\":\"x11-present-request\",\"gpuQueries\":false}";
}
