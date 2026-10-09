# Instrumentation contract (version 1)

Status 2026-10-09: implemented in the working tree (unix bridge ABI 7, app transport, xrimmersive,
control server). It has not yet been captured on a device; the first Swan capture is the acceptance
for this contract. Record the APK SHA-256 and this version with every bundle.

Constraints:

- Profiler code exists only in internal `picoXrDebug` builds (`xrgame_profiler.h` compiles to no-ops
  elsewhere; the single SDK owner is `libxrgame_debugbus.so`). Keep it that way.
- The Wine game process has no profiler ring. Its timestamps travel in the existing FRAME message.
- Counter names are static strings in `profiler_core.cpp`, indexed by `XrProfileCounter` in
  `xrgame_profiler.h`; never put paths, tokens or per-file names in a name.

## Game process (unix bridge and PE runtime)

Timestamps are `clock_gettime(CLOCK_MONOTONIC)` nanoseconds taken in the unixlib
(`gamenative_openxr_unix.c`). The PE runtime only reads raw QueryPerformanceCounter ticks around its
drain and passes them in `gn_unix_submit_stereo_args` (`qpc_frequency`, `qpc_drain_begin`,
`qpc_drain_lock`, `qpc_drain_end`, `qpc_call`); QPC has its own origin, so the unixlib converts the
differences to `qpc_call` into ages before its own submit-begin timestamp.

| Field | Begin / end | Where |
| --- | --- | --- |
| fid (`frame=`) | allocated when the stereo submit enters the shipper ring | `submit_views_async` / `submit_views_sync` |
| snap | `serial=` of the FRAME_SYNC reply this frame rendered against | `unix_control_transact` |
| frame_sync | around the FRAME_SYNC control transaction (unix fast path only) | `unix_control_transact` |
| frame_sync.delay_begin (v1.1) | start of the frame-start delay; equals frame_sync begin when no delay was applied | `unix_control_transact` |
| swapchain_wait l / r | image-reuse wait plus the ACQUIRE round trips | `unix_acquire_image`; a shared swapchain goes to the left eye only |
| endframe.drain | DXVK release-transition + flush + submission lock, or vkd3d lock | PE `gn_xrEndFrame` |
| endframe.drain.lock | the point between DXVK's flush and its lock (value) | PE `gn_dxvk_flush_and_lock` |
| endframe.submit | from unixlib entry to the frame entering the shipper ring, including any wait for ring space | `unix_submit_stereo` |
| s.fence_wait | the shipper's `vkWaitForFences` on the game-queue fence | `submit_worker` |
| s.send | first FRAME write .. app receipt of the last FRAME | `ship_views`; the app supplies the end |

A wait (FRAME_SYNC, swapchain) is claimed by the first submit after it, so each appears in at most
one fid. A FRAME_SYNC that went through the winsock fallback has no timestamps, and that fid is
incomplete.

Transport: the last eye's FRAME line ends with

```
 snap=<serial> tb=<submit begin ns> t=<14 offsets from tb, comma-separated, "_" when absent>
```

in the order frame_sync begin/end, swapchain_wait l begin/end, r begin/end, drain begin/lock/end,
submit end, fence_wait begin/end, send begin, and (v1.1) frame_sync delay begin. v1 bridges send
only the first 13; the app accepts both. The line limit is 1023 characters on both sides.

### Pacing and latency switches (v1.1, off by default)

DebugBus `vr_tuning [pacing=off|auto|half] [start=<us>] [predict=0|1]` sets them at runtime.

- The FRAME_SYNC reply carries ` jit=<us>`. When it is non-zero the unixlib delays the next
  FRAME_SYNC: if the game entered xrEndFrame within [-1, +1.5] ms of the previous fid's
  `s.fence_wait` end (it was blocked on that GPU work), the delay grows by half of the
  `swapchain_wait r end -> drain begin` time above the target; otherwise it shrinks by a quarter.
  The fast path sends `FRAME_SYNC delay_us=<n>` so the server's pacing excludes that delay.
- `pacing=half` hands out every second XR serial; `auto` picks 1-3 serials from the game's
  reply-to-request time once the choice has held for 30 requests.
- `predict=1` locates the snapshot's views and hand poses at the XR display time plus
  `vr.predict.lead_milli` / 1000 periods (at most 4); `time=` in the reply carries that time.

## App process

| Name | Kind | Where |
| --- | --- | --- |
| `vr.g.snap`, `vr.g.*.begin_ns` / `.end_ns`, `vr.g.endframe.drain.lock_ns`, `vr.s.*` | counters | transport thread, `publishFrameTiming`, after the FRAME reply; all fields before the commit |
| `vr.s.send.end_ns` | counter | the app's CLOCK_MONOTONIC when the last FRAME line was read |
| `vr.g.fid` | counter, commit | transport thread, last |
| `host.vr.transport.frame` | zone | FRAME handling |
| `host.vr.transport.acquire_wait` | zone | ACQUIRE condition wait |
| `vr.control.frame_sync` | Kotlin region (track 1) | `WindowsVrControlServer.frameSync` |
| `host.vr.xr.frame` | zone | one `runLoop` iteration |
| `host.vr.xr.wait_frame`, `host.vr.xr.input_locate`, `host.vr.xr.end_frame` | zones | `runLoop`, `submitWindowsProjection`, `submitQuadLayer` |
| `host.vr.projection.render` | zone | `submitWindowsProjection` around `render()` |
| `vr.android.serial` | counter | when the runtime snapshot is published (FRAME_SYNC replies with it) |
| `vr.present.fresh` | counter (0-2) | `render()`, fresh eyes drawn this XR frame |
| `vr.present.fid.l`, `vr.present.fid.r` | counters | `render()`, fid drawn per eye this XR frame |
| `vr.g.frame_sync.delay_begin_ns` (v1.1) | counter | transport thread, from the 14th FRAME offset |
| `vr.predict.lead_milli` (v1.1) | counter | `submitWindowsProjection`, EMA of XR serials from the drawn fid's snap to its first draw, x1000 |

`vr.control.frame_sync` is a region, not a zone: fetch it with `query_regions` and save it in the
`{"vr.control.frame_sync": [{"startMs", "endMs"}]}` slice-map form.

## Clocks

`profiler.cpp` writes `clock_boot_ns`, `clock_trace_ns` and `clock_mono_ns` into the ring's
`app_info` at initialization. The manifest offset is `clock_mono_ns - clock_trace_ns`. If a capture
spans a suspend, the offset can change; split the window at the suspend. The builder rejects an
offset under which a FRAME send starts after the app committed it.

## GPU

Optional. KGSL per-context data through litep (`capture_kgsl` / `analyze_gpu_kgsl`) or GPU timestamp
queries around the bridge copy. Treat as uncalibrated unless the capture proves alignment.
