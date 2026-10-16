/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Mid-function site, ported from shadPS4 guest/custom/v1/include/shad_site.h.
 * The handler runs at an instruction boundary with the complete integer and SSE
 * machine state, and may change it.
 *
 *   mode "before":  handler, then the original instructions it displaced.
 *   mode "replace": handler instead of the `expected` instructions; execution
 *                   continues after them. The handler must reproduce every effect
 *                   of those instructions that later code relies on.
 *
 * Writes to gpr (except rsp), rflags and the xmm registers in `fxsave` take effect
 * when the handler returns. Unlike the shadPS4 adapter only the SSE state is saved
 * (FXSAVE), so a handler must be SSE-only (the builder compiles for x86-64 without
 * AVX) and cannot stand in for VEX instructions that clear upper YMM halves.
 * The handler runs with the guest MXCSR. No exceptions, longjmp or GS changes. */
#ifndef XRG_SITE_V1_H
#define XRG_SITE_V1_H
#include "xrg_guest.h"
typedef struct XrgSiteGpr {
    xrg_u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
    xrg_u64 r8, r9, r10, r11, r12, r13, r14, r15, rflags;
} XrgSiteGpr;
typedef struct XrgSiteContext {
    xrg_u64 rsp;           /* guest rsp at the site (read-only) */
    XrgSiteGpr* gpr;       /* rax..r15, rflags */
    unsigned char* fxsave; /* 512 bytes: x87, MXCSR and xmm0-15 at offset 160 */
    xrg_u64 reserved;
} XrgSiteContext;
#if defined(__cplusplus)
static_assert(sizeof(XrgSiteGpr) == 128 && sizeof(XrgSiteContext) == 32);
#endif

/* xmm register n (16 bytes) inside the FXSAVE image. */
static inline unsigned char* xrg_site_xmm(XrgSiteContext* ctx, unsigned n) {
    return ctx->fxsave + 160u + 16u * n;
}
/* Scalar lane 0 of xmm register n, as float. */
static inline float* xrg_site_xmm_f32(XrgSiteContext* ctx, unsigned n) {
    return (float*)xrg_site_xmm(ctx, n);
}
#endif
