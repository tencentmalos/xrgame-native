/* SPDX-License-Identifier: GPL-2.0-or-later */
/* XRGame guest patch SDK v1, ported from shadPS4 guest/custom/v1/include/shad_guest.h.
 * Payloads are freestanding x86-64 code that runs inside the Windows game process
 * (under FEX on ARM64EC Wine). Everything that crosses into game code or into the
 * installer uses the Windows x64 calling convention (XRG_WINAPI); calls inside a
 * payload keep the compiler's default. */
#ifndef XRG_GUEST_V1_H
#define XRG_GUEST_V1_H
typedef __UINT64_TYPE__ xrg_u64;
typedef __INT64_TYPE__ xrg_i64;
typedef __UINT32_TYPE__ xrg_u32;
typedef __INT32_TYPE__ xrg_i32;
typedef __SIZE_TYPE__ xrg_size;
#define XRG_WINAPI __attribute__((ms_abi))
#ifdef __cplusplus
extern "C" {
#endif
/* The installer's services, the only imports outside the game module. */
XRG_WINAPI xrg_u64 xrg_sdk_query(xrg_u64 version); /* v1: bit0 clock, bit1 counters, bit2 logs */
XRG_WINAPI xrg_u64 xrg_sdk_clock_ns(void);         /* QueryPerformanceCounter in ns */
/* A recipe-declared counter: sample count and last value appear in the status. */
XRG_WINAPI xrg_u64 xrg_sdk_counter(xrg_u64 declared_id, xrg_i64 value);
/* A recipe-declared scalar log tag, appended to guest-patch.log. */
XRG_WINAPI xrg_u64 xrg_sdk_log(xrg_u64 declared_tag, xrg_i64 value);
void *memcpy(void *dst, const void *src, xrg_size n);
void *memmove(void *dst, const void *src, xrg_size n);
void *memset(void *dst, int value, xrg_size n);
int memcmp(const void *a, const void *b, xrg_size n);
#ifdef __cplusplus
}
namespace xrg {
/* Times a scope into a declared counter (nanoseconds, including the clock calls). */
class TimedScope {
  xrg_u64 id, start;

public:
  explicit TimedScope(xrg_u64 counter_id) : id(counter_id), start(xrg_sdk_clock_ns()) {}
  ~TimedScope() { xrg_sdk_counter(id, (xrg_i64)(xrg_sdk_clock_ns() - start)); }
  TimedScope(const TimedScope &) = delete;
  TimedScope &operator=(const TimedScope &) = delete;
};
inline xrg_u64 Log(xrg_u64 tag, xrg_i64 value) { return xrg_sdk_log(tag, value); }
} // namespace xrg
#endif
#endif
