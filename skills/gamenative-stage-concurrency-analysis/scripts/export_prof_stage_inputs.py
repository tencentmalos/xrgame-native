#!/usr/bin/env python3
"""Export a GameNative app PROF capture into stage-builder inputs.

Decodes the capture with the Python reference decoder shipped in the litep MCP package (the
Litep.Core C# decoder must reproduce its event digest), so every timestamp stays on the
capture's absolute clock. Writes into a new directory:

  counters.json   vr.* counter samples in the query_counters response shape
  slices.json     the builder's app zones and the vr.control.frame_sync region
  manifest.json   clock offset from app_info, input list, decoder identity and diagnostics

Nothing is resampled or synthesised; a truncated capture or unclosed spans are reported in the
manifest's identity.decoder block and must be reviewed before interpreting the bundle.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

import build_gn_xr_stage_bundle as builder

COUNTER_PREFIXES = ("vr.g.", "vr.s.", "vr.android.", "vr.present.", "vr.predict.")
# Each X11 PresentPixmap request (the game window, e.g. a VR companion window) is a FrameMark.
PRESENT_COUNTER = "x11.present_request"
ZONES = {name for name in builder.APP_SCOPES if name.startswith("host.")}
REGIONS = {name for name in builder.APP_SCOPES if not name.startswith("host.")}


def find_decoder(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit else []
    if os.environ.get("LITEP_ROOT"):
        candidates.append(Path(os.environ["LITEP_ROOT"]))
    candidates += sorted((Path.home() / "PicoMcp/litep/versions").glob("*"), reverse=True)
    for root in candidates:
        scripts = root / "sdk/skills/spatial-trace-analyzer/scripts"
        if (scripts / "decoder.py").is_file():
            return scripts
    raise SystemExit("litep reference decoder not found; pass --litep-root")


def app_info(events: list) -> dict[str, str]:
    """The SDK publishes app_info as a bookmark or post; fall back to the CLI value."""
    for event in events:
        if "clock_mono_ns=" in event.name and "clock_trace_ns=" in event.name:
            return dict(item.split("=", 1) for item in event.name.split(";") if "=" in item)
    return {}


def export(prof: Path, output: Path, decoder_dir: Path, app_info_text: str | None) -> dict:
    sys.path.insert(0, str(decoder_dir))
    from decoder import read_bytes  # noqa: E402

    data = prof.read_bytes()
    result = read_bytes(data)
    counters: dict[str, list[dict[str, int]]] = {}
    slices: list[dict] = []
    stacks: dict[int, list] = {}
    regions: dict[int, dict] = {}
    for event in result.events:
        if event.kind == "Counter" and event.name.startswith(COUNTER_PREFIXES):
            counters.setdefault(event.name, []).append({"time": event.ts_ns, "value": event.i64})
        elif event.kind == "FrameMark":
            samples = counters.setdefault(PRESENT_COUNTER, [])
            samples.append({"time": event.ts_ns, "value": len(samples) + 1})
        elif event.kind == "SpanBegin":
            stacks.setdefault(event.tid, []).append(event)
        elif event.kind == "SpanEnd":
            stack = stacks.get(event.tid)
            if not stack:
                continue
            begin = stack.pop()
            if begin.name in ZONES:
                slices.append({"name": begin.name, "startMs": begin.ts_ns / 1e6, "endMs": event.ts_ns / 1e6,
                               "tid": event.tid, "threadName": event.thread_name})
        elif event.kind == "RegionBegin" and event.name in REGIONS:
            regions[event.cookie] = {"name": event.name, "start": event.ts_ns, "tid": event.tid,
                                     "threadName": event.thread_name}
        elif event.kind == "RegionEnd" and event.cookie in regions:
            region = regions.pop(event.cookie)
            slices.append({"name": region["name"], "startMs": region["start"] / 1e6, "endMs": event.ts_ns / 1e6,
                           "tid": region["tid"], "threadName": region["threadName"]})

    info = app_info(result.events)
    if not info and app_info_text:
        info = dict(item.split("=", 1) for item in app_info_text.split(";") if "=" in item)
    if "clock_mono_ns" not in info or "clock_trace_ns" not in info:
        raise SystemExit("app_info with clock_mono_ns / clock_trace_ns not found; pass --app-info")

    output.mkdir(parents=True, exist_ok=False)
    (output / "counters.json").write_text(json.dumps({"counters": [
        {"counterName": name, "samples": samples, "truncated": False}
        for name, samples in sorted(counters.items())]}), encoding="utf-8")
    (output / "slices.json").write_text(json.dumps({"slices": slices}), encoding="utf-8")
    decoder_identity = {
        "prof": str(prof), "prof_sha256": hashlib.sha256(data).hexdigest(),
        "decoder": str(decoder_dir / "decoder.py"),
        "decoder_sha256": hashlib.sha256((decoder_dir / "decoder.py").read_bytes()).hexdigest(),
        "session": result.session_name, "process": result.process_name, "pid": result.process_id,
        "clock_source": result.clock_source, "events": len(result.events),
        "chunks_ok": result.chunks_ok, "chunks_skipped": result.chunks_skipped,
        "truncated": result.truncated, "diagnostics": result.diagnostics,
        "open_regions": len(regions),
        "counter_samples": {name: len(samples) for name, samples in sorted(counters.items())},
        "slice_counts": {name: sum(1 for s in slices if s["name"] == name) for name in sorted(ZONES | REGIONS)},
    }
    manifest = {
        "title": "GameNative XR stage concurrency",
        "identity": {"app_info": info, "decoder": decoder_identity},
        "clock": {"mono_minus_trace_ns": int(info["clock_mono_ns"]) - int(info["clock_trace_ns"])},
        "counters": ["counters.json"], "slices": ["slices.json"], "xr_period_ms": 13.889,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                          encoding="utf-8")
    return decoder_identity


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prof", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path, help="new directory")
    parser.add_argument("--litep-root", type=Path, help="litep MCP package root (default: newest in ~/PicoMcp)")
    parser.add_argument("--app-info", help="app_info text from the capture status, when not in the PROF")
    args = parser.parse_args()
    if args.output_dir.exists():
        parser.exit(1, f"{args.output_dir} exists; choose a new directory\n")
    identity = export(args.prof, args.output_dir, find_decoder(args.litep_root), args.app_info)
    print(json.dumps({key: identity[key] for key in
                      ("events", "truncated", "diagnostics", "counter_samples", "slice_counts")}, indent=2))


if __name__ == "__main__":
    main()
