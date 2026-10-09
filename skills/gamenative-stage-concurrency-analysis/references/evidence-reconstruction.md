# Reconstruct a six-frame window

Requires an APK that implements [the instrumentation contract](instrumentation-contract.md).

## Capture

With the app in a stable XR scene: `tools/xrgame/debugbus.py --serial <s> --start instrumentation`,
`instrumentation coarse`, then `profiler_capture file 64 30`. Poll `profiler_capture status` until
`capture_state=ready`, then pull `files/xrgame/profiles/file-*.prof` and its `.prof.json` sidecar with
`adb exec-out run-as com.tencentmalos.xrgamenative cat …`. litep's `android_capture` does not
recognise this DebugBus service.

## Route A: reference decoder (used for run19)

The litep MCP build 20260921.4 refused the SDK 0b467a8 PROF ("unknown or conflicting SDK wire
provenance", also with `sdk_revision`). `scripts/export_prof_stage_inputs.py --prof <file>
--output-dir <new> --app-info "<sidecar app_info>"` decodes it with the reference decoder shipped in
the litep package and writes `counters.json`, `slices.json` and `manifest.json` on the capture's
absolute clock. Review `identity.decoder` (truncation, diagnostics, sample counts) before building.
Add `fids` and identity to a copy of the manifest, then build and deliver as below.

## Route B: litep MCP queries

1. Open the app ring capture with litep `open_file(format="prof")`. Run `get_import_diagnostics` and
   `get_session_summary`; stop on truncation, dropped records or a binary mismatch. Copy
   `clock_mono_ns` and `clock_trace_ns` from the session's app info.
2. `list_counters` with the filters `vr.g.`, `vr.s.`, `vr.android.` and `vr.present.`. Query them with
   `query_counters`, `include_samples=true` and `max_samples` above every `availableSampleCount`. A
   response is limited to 5000 samples per counter; split long windows into consecutive,
   non-overlapping sub-windows and keep every response. Truncated samples are a hard failure.
3. Pick a stable scene and seven consecutive `vr.g.fid` commits. Re-query every counter over a window
   that starts one frame before the first commit and ends one frame after the last, so the tuple
   of the first frame and the work straddling both edges are present.
4. `find_top_slices` for the zones `host.vr.xr.frame`, `host.vr.xr.wait_frame`,
   `host.vr.xr.input_locate`, `host.vr.projection.render`, `host.vr.xr.end_frame`,
   `host.vr.transport.frame` and `host.vr.transport.acquire_wait` over the same window, and
   `query_regions` for the Kotlin region `vr.control.frame_sync` (save it in the slice-map form the
   contract describes). Save the raw responses. Include `vr.present.fid.l` / `.r` in the counter
   query when present-latency metrics are wanted.
   The builder treats `startMs` / `endMs` as absolute capture-clock milliseconds, like the counter
   sample times. If a litep response is trace-relative, convert it first; a wrong base shows up as
   app zones far outside the frame window.
5. Optional GPU: save gpu-time slices per context (game, app) and mark them uncalibrated unless the
   capture proves alignment.
6. Write the manifest next to the saved responses and run the builder. Give `fids` explicitly when
   the window is not the first seven complete frames in the responses.
7. Deliver with `deliver_stage_diagram.py` into a new directory.

Use litep `analyze_tracefs_lanes` or a scheduler capture for dxvk-cs / dxvk-submit run states when the
question is whether the DXVK drain waits on translation. Keep that evidence beside the bundle; the
lane model does not merge run states into stage intervals.
