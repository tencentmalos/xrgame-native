// SPDX-License-Identifier: GPL-3.0-or-later
// Half-Life: Alyx (546560), rendersystemvulkan.dll: the eye-tracked FDM offset in units of one
// eye when both eyes share one double-wide layer.
//
// CRenderContextVulkan's EndRendering (rendersystemvulkan+0x34320) asks the VR interface for each
// eye's foveation centre in NDC and ends an FDM pass with
// offset = round(centre * 0.5 * render area / granularity) * granularity, through
// VK_EXT_fragment_density_map_offset. With true multiview the render area is one eye; in the
// single-layer stereo mode it holds both eyes side by side, so x moves twice as far as the gaze.
// This site halves the x centres before that conversion when the pass has one view and a render
// area wider than tall. Together with etfr_fdm_split each eye's full-density region then follows
// the gaze in its own half.

namespace {
constexpr xrg_u64 kHalved = 1; // counter: offsets converted per eye, last = render area width
constexpr xrg_u64 kKept = 2;   // counter: offsets left as the game computes them, last = view count
constexpr xrg_u64 kCentreX = 3; // counter: the game's eye 0 centre x before any change, x 10000
constexpr xrg_u64 kCentreY = 4; // counter: the game's eye 0 centre y, x 10000

// CRenderContextVulkan, held in rdi at the site. BeginRendering (+0x347C0) copies the rendering
// description to +0x1310, whose dword 2 is the view count, and the render area to +0x154C.
constexpr xrg_u64 kViewCount = 0x1318;
constexpr xrg_u64 kAreaWidth = 0x1554;
constexpr xrg_u64 kAreaHeight = 0x1558;
// The centres, x and y per eye, filled by the VR interface (vtable +0x110) at rbp - 0x80.
constexpr xrg_u64 kCentres = 0x80;
} // namespace

extern "C" void halve_centre_x(XrgSiteContext* ctx) {
    const unsigned char* render = reinterpret_cast<const unsigned char*>(ctx->gpr->rdi);
    xrg_i32 views;
    xrg_u32 width, height;
    memcpy(&views, render + kViewCount, sizeof(views));
    memcpy(&width, render + kAreaWidth, sizeof(width));
    memcpy(&height, render + kAreaHeight, sizeof(height));
    float* centres = reinterpret_cast<float*>(ctx->gpr->rbp - kCentres);
    xrg_sdk_counter(kCentreX, static_cast<xrg_i64>(centres[0] * 10000.0f));
    xrg_sdk_counter(kCentreY, static_cast<xrg_i64>(centres[1] * 10000.0f));
    if (views > 1 || width <= height) {
        xrg_sdk_counter(kKept, views);
        return;
    }
    centres[0] *= 0.5f;
    centres[2] *= 0.5f;
    xrg_sdk_counter(kHalved, width);
}
