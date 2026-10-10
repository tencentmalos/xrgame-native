#pragma once
// Forced into Foundation's XR ImGui layer sources: their profiler scopes become this build's
// xrgame profiler scopes instead of pulling in Foundation's module runtime.
#include "xrgame_profiler.h"
#define XRGAME_FOUNDATION_SCOPE_JOIN_(a, b) a##b
#define XRGAME_FOUNDATION_SCOPE_JOIN(a, b) XRGAME_FOUNDATION_SCOPE_JOIN_(a, b)
#define SPATIAL_PROFILER_AUTO_SCOPE_NAME(name) \
    XrProfileScope XRGAME_FOUNDATION_SCOPE_JOIN(foundation_scope_, __LINE__) { name, true }
