# GameNative XR stage model

Derived from the working tree on 2026-10-09 (source reading, not measurement). File references:
RT = `app/src/main/windows/openxr_runtime/gamenative_openxr_runtime.c`,
UX = `.../openxr_runtime/unix/gamenative_openxr_unix.c`, IM = `app/src/main/cpp/xrimmersive/xr_immersive.cpp`,
TR = `.../xrimmersive/xr_windows_transport.cpp`, PJ = `.../xrimmersive/xr_windows_projection.cpp`,
CS = `app/src/main/java/app/gamenative/ui/screen/xr/windows/WindowsVrControlServer.kt`.
Line numbers drift; search by function name.

## Processes and threads

| Process | Thread | Per-frame work |
| --- | --- | --- |
| Wine game | render thread (Alyx + OpenComposite + PE runtime under FEX) | xrWaitFrame (FRAME_SYNC), game work, per-eye swapchain wait/acquire, xrEndFrame (DXVK flush + lock, bridge submit) |
| Wine game | dxvk-cs / dxvk-submit / dxvk-queue / dxvk-frame | D3D11 translation, queue submission, fence retirement |
| Wine game | shipper (unixlib pthread) | waits the per-frame VkFence, then sends FRAME for both eyes |
| App | control thread (TCP 127.0.0.1:38476) | answers FRAME_SYNC after the snapshot serial advances |
| App | transport thread (AF_UNIX `@gamenative-xr`) | BUFFER / FRAME / ACQUIRE; keeps the latest frame per eye |
| App | XR thread (Pico OpenXR, 72 Hz) | xrWaitFrame, input, locate, publish snapshot, draw both eyes, xrEndFrame |
| Pico runtime | compositor | ATW and distortion |

## Lanes

| Stage | Role | Evidence | Token |
| --- | --- | --- | --- |
| Game thread | root | `vr.g.frame_sync.begin_ns` .. `vr.g.endframe.submit.end_ns` | fid |
| Game thread / FRAME_SYNC | detail, inline | `vr.g.frame_sync.*` | fid |
| Game thread / swapchain wait L, R | detail, inline | `vr.g.swapchain_wait.{l,r}.*` (image-reuse gate + ACQUIRE) | fid |
| Game thread / DXVK drain | detail, inline | `vr.g.endframe.drain.*` (FlushRenderingCommands + LockSubmissionQueue) | fid |
| Game thread / bridge submit | detail, inline | `vr.g.endframe.submit.*` (`submit_game_queue_work`) | fid |
| Shipper | detail | fence wait begin .. FRAME send end | fid |
| Shipper / fence wait, FRAME send | detail, inline | `vr.s.fence_wait.*`, `vr.s.send.*` | fid |
| App transport | detail | zone `host.vr.transport.frame`; inline `host.vr.transport.acquire_wait` | (fid in zone args, optional) |
| App control / FRAME_SYNC | detail | Kotlin region `vr.control.frame_sync` | snap |
| XR loop | root | zone `host.vr.xr.frame`; inline wait_frame / input_locate / projection.render / end_frame | aser |
| GPU \<context\> | detail | optional gpu-time slices; excluded from overlap unless calibrated | none |

The engine merges each stage before counting, so inline children never inflate concurrency.

## Frame boundary and tokens

- **fid**: allocated per submitted game frame in the bridge (`unix_submit_stereo`), carried through
  the shipper ring into FRAME `frame=`. The frame band ends at that fid's bridge submit end:
  seven consecutive fids give six bands `fid N .. fid N+5`.
- **snap**: the snapshot serial that FRAME_SYNC returned to the game for this frame (`serial=` in the
  FRAME_SYNC reply, RT parses and passes it down). It links a game frame to the XR loop iteration
  whose pose it rendered with.
- **aser**: the XR loop serial published with each snapshot (IM `frameSerial`). Pins on the XR row.

A game frame and an XR iteration are on different clocks: the game runs free (FRAME_SYNC returns
immediately when the game is slower than 72 Hz), the XR loop is paced by Pico's xrWaitFrame. Do not
expect bands and pins to align, and do not call a band "late" relative to an XR pin without a snap edge.

## Interval math

Unchanged from the vendored engine: half-open intervals, stage wall time = union, raw sum diagnostic
only, pairwise overlap with three denominators, concurrency over distinct root-stage unions, every
interval clipped to the window, percentiles exclude occurrences clipped by the window.

## Mapping to Azahar's vocabulary

| Azahar | GameNative |
| --- | --- |
| Guest | Game thread (its waits are inline details) |
| PICA | dxvk-cs translation (not instrumented; tracefs run states only) |
| Host Record | dxvk-cs record + dxvk-submit, plus DXVK drain and bridge submit |
| StatusLayer | app overlay quad (not modelled) |
| FSR / FG | none (AppSW off; compositor ATW is the nearest analogue) |
| Present | virtual xrEndFrame + shipper, then app draw + Pico xrEndFrame |
| GPU | three contexts share the GPU: game queue, app GL, compositor |

GameNative-only stages: TCP control plane, AF_UNIX FRAME/ACQUIRE with fd passing, OpenComposite's
OpenVR-to-OpenXR copy, the bridge AHB copy, per-frame DXVK drain under the queue lock, two free-running
frame clocks with latest-frame overwrite, and the app-side redraw.
