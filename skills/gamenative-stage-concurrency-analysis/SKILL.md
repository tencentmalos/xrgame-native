---
name: gamenative-stage-concurrency-analysis
description: Reconstruct GameNative (SteamPSP) XR per-frame stage timelines across the Wine game process and the Android app process (game thread FRAME_SYNC / swapchain waits / DXVK drain / bridge submit, shipper fence wait and FRAME send, app transport and control threads, Pico OpenXR loop) and deliver a six-frame concurrency view. Use for XR frame pacing, cross-process handoff, wait attribution, copy or GPU contention, and optimization A/B questions on Swan or AYN.
---

# GameNative Stage Concurrency Analysis

Port of Azahar's `azahar-stage-concurrency-analysis`. The interval engine, SVG renderer and Archify
delivery are vendored unchanged in behaviour ([provenance](references/provenance.md)); the lane
model, tokens and evidence rules are GameNative's own. Do not apply Azahar's Guest/PICA/Host Record
semantics here, and do not copy GameNative conclusions back to Azahar.

## Status (2026-10-09)

[Contract version 1](references/instrumentation-contract.md) is implemented in the working tree
(unix bridge ABI 7) but has **not yet been captured on a device**. Until a debug APK built from it
yields a capture, the builder and analyzer run only on the synthetic fixtures in `tests/`, and every
chart must be delivered with `--evidence-kind synthetic`. A measured delivery requires a capture
whose APK implements the contract version it names.

The per-frame structure that the model encodes was derived from source, not measured; see the
XR timing comparison artifact linked from the session record and
[stage-model.md](references/stage-model.md).

## Choose the model

- Frame pacing, waits, cross-process handoff, overlap between the game frame and the Pico loop:
  build a stage bundle with `scripts/build_gn_xr_stage_bundle.py` and deliver it.
- GPU hangs, KGSL snapshots, shader faults: not this skill. Use the Qualcomm snapshot skills in the
  `bug_reports` workspace.
- Draw-level frame content: RenderDoc / GFXReconstruct workflows (`docs/specs/xrgame-native-api-replay-v1.md`).

## Evidence gates

Record before interpreting anything (AGENTS.md hard constraint 5):

1. APK SHA-256, `.so` Build IDs (`libxrgame_debugbus.so`, `gamenative_xr_unixbridge.so`), component
   catalog SHA, device model/build/boot id, game PID, app PID, capture window and duration.
2. Litep `get_import_diagnostics` and `get_session_summary`: reject truncated captures, sample
   truncation and binaries that do not match the APK.
3. The app ring's `app_info` must carry `clock_mono_ns` and `clock_trace_ns`; their difference is
   `clock.mono_minus_trace_ns` in the manifest. The builder rejects a capture whose FRAME send
   starts after the app committed that frame, which is how a wrong offset shows up.
4. Seven consecutive complete `vr.g.fid` commits from a stable scene give the six measured game
   frames. Never pick or rank a "longest" frame.
5. GPU slices are uncalibrated unless the capture proves CPU/GPU alignment; keep them out of overlap
   and concurrency (`include_in_overlap=false`).

Keep failed and aborted captures with their reason.

## Workflow

Read [evidence-reconstruction.md](references/evidence-reconstruction.md) for the exact queries.

1. Capture with an internal `picoXrDebug` APK that implements the contract (instrumentation level
   `coarse` is enough; `detail` adds fence-level zones). Note the DebugBus commands used.
2. Open the `.prof` with the litep MCP, save the raw `find_top_slices` and
   `query_counters(include_samples=true)` responses as JSON files. Do not hand-edit samples.
3. Write a manifest (format in the builder's docstring) and build:

```sh
python skills/gamenative-stage-concurrency-analysis/scripts/build_gn_xr_stage_bundle.py \
  --manifest <run>/manifest.json --output <run>/stage.bundle.json
```

4. Deliver through the Archify viewer. On this Windows workstation the delivery script finds the
   Archify copy bundled with the litep MCP (`~/PicoMcp/litep/versions/*/skills/archify`);
   `--archify` or `ARCHIFY_ROOT` override it.

```sh
python skills/gamenative-stage-concurrency-analysis/scripts/deliver_stage_diagram.py \
  --input <run>/stage.bundle.json --output-dir <new-run-directory> \
  --evidence-kind measured --palette studio --theme dark
```

Existing output directories are never replaced; failed analysis publishes nothing. Open the HTML,
check both themes, select an interval, and record the visual review separately from the receipt.

## Reading the chart

- Roots that count for concurrency: **Game thread** and **XR loop**. Everything else is detail.
- Waits are drawn inline in their thread's row (FRAME_SYNC, swapchain waits, DXVK drain, fence wait,
  ACQUIRE wait). They explain where a frame's time went; they are not separate concurrency.
- Frame bands are **game frames** (bridge submit end of each fid). The XR loop runs on its own 72 Hz
  clock; its `aser` pins show when snapshots were published. Expect the two grids to drift.
- A gap is a dependency only when a token edge proves it: `fid` (game frame), `snap` (the XR snapshot
  serial that FRAME_SYNC returned to that frame) and `aser` (XR loop serial). Classify every other gap
  as unclassified.
- Per-frame annotations add, when the capture has `vr.present.fid.*`: `present_latency_ms` (submit
  end to the first XR frame that draws the fid), `shown_xr_frames` (XR frames that show it) and
  `eye_fid_mismatch` (XR frames in the band whose eyes show different fids). `drain_lock_ms` is the
  part of the DXVK drain spent waiting for its submission lock rather than flushing.

## Report contract

Write reports in Chinese. Include: evidence identity (gate 1), the exact fid window, per-frame
duration and waits, per-stage union/p50/p90/p99, pairwise overlaps, root concurrency histogram,
token contract result, diagnostics, and the artifact receipt. For every proposed optimization name
the producer, the consumer, the signal that releases the consumer, and the measurement that would
show the change. State which numbers are measured and which are derived from source.
