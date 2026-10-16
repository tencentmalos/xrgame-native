// SPDX-License-Identifier: GPL-3.0-or-later
// Half-Life: Alyx (546560), renderingpipelines.dll: a same-session switch for the game's fragment
// density map, for A/B measurements only.
//
// The VR pipeline builder (renderingpipelines+0x171C0) asks for the FDM texture of its render
// target size every time it builds the pipeline (call at +0x1786C to +0x16C70, which leaves a
// null texture in *out when vr_fdm is 0). While this site is enabled the call is replaced by that
// null result, so the passes render without FDM; `guest_patch disable alyx_etfr_fdm_ab` turns the
// game's FDM back on and `enable` turns it off again. A session therefore starts with FDM off.

namespace {
constexpr xrg_u64 kSkipped = 1; // counter: FDM requests answered with no texture
} // namespace

extern "C" void skip_fdm(XrgSiteContext* ctx) {
    // call renderingpipelines+0x16C70 with rcx = &out: store no texture and return &out in rax.
    xrg_u64* out = reinterpret_cast<xrg_u64*>(ctx->gpr->rcx);
    *out = 0;
    ctx->gpr->rax = ctx->gpr->rcx;
    xrg_sdk_counter(kSkipped, 1);
}
