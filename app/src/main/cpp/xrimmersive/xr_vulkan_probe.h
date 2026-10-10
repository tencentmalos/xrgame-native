#pragma once

// Diagnostic for the composite's out-of-bounds GPU writes (debug.xrgame.xr.ccuprobe): renders into
// an image whose memory has a filled guard region behind it, uses that image in a second pass and
// logs every guard byte the GPU changed.

#include "xr_vulkan.h"

namespace xrimmersive::vulkan {

// Bits of the probe variant.
constexpr uint32_t kProbeMutable = 1u;     // the first target is MUTABLE_FORMAT, like the upscaler's images
constexpr uint32_t kProbeCopy = 2u;        // the second pass copies the first target instead of sampling it
constexpr uint32_t kProbeRenderOnly = 4u;  // no second pass
constexpr uint32_t kProbeLarge = 8u;       // the first target has the output size instead of the source size

// Records and runs `runs` probes of `variant` on the composite queue and waits for each.
void RunGuardProbe(Context &context, VkFormat format, uint32_t variant, uint32_t runs);

}  // namespace xrimmersive::vulkan
