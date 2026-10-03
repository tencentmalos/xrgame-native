// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// Only internal picoXrDebug links the single SDK owner in libxrgame_debugbus.
// Other flavors compile these hooks away; no Foundation headers cross this boundary.
struct XrProfileRegion { uint64_t cookie, generation; };
#ifdef XRGAME_PROFILE
extern "C" {
__attribute__((visibility("default"))) uint64_t xrgame_profile_begin(const char* name, bool detail) noexcept;
__attribute__((visibility("default"))) void xrgame_profile_end(uint64_t token) noexcept;
__attribute__((visibility("default"))) XrProfileRegion xrgame_profile_region(const char* name) noexcept;
__attribute__((visibility("default"))) void xrgame_profile_region_end(XrProfileRegion token) noexcept;
}
#else
inline uint64_t xrgame_profile_begin(const char*, bool) noexcept { return 0; }
inline void xrgame_profile_end(uint64_t) noexcept {}
inline XrProfileRegion xrgame_profile_region(const char*) noexcept { return {}; }
inline void xrgame_profile_region_end(XrProfileRegion) noexcept {}
#endif

// Physical-thread CPU elapsed scope, not GPU execution or on-CPU time.
class XrProfileScope final {
public:
    explicit XrProfileScope(const char* name, bool detail = false) noexcept
        : token(xrgame_profile_begin(name, detail)) {}
    ~XrProfileScope() { end(); }
    void end() noexcept { if (token) { xrgame_profile_end(token); token = 0; } }
    XrProfileScope(const XrProfileScope&) = delete;
    XrProfileScope& operator=(const XrProfileScope&) = delete;
private:
    uint64_t token;
};

// Wrap a real driver call without changing its return/error path or lexical lock lifetime.
template<class F> inline auto xrProfileCall(const char* name, F&& call, bool detail = false) -> decltype(call()) {
    XrProfileScope scope(name, detail);
    return call();
}
