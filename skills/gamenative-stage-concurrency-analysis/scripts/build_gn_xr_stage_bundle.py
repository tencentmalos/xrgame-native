#!/usr/bin/env python3
"""Build a GameNative XR stage bundle (schema v2) from saved Litep responses.

The XR path spans two processes, so the evidence has two shapes (see
references/instrumentation-contract.md):

* App process (Litep ring "XRGameNative"): profiler zones on the trace clock --
  host.vr.xr.frame with host.vr.xr.wait_frame / input_locate / host.vr.projection.render /
  host.vr.xr.end_frame inside it, host.vr.transport.frame / acquire_wait, and the Kotlin region
  vr.control.frame_sync -- plus the counters vr.android.serial and vr.present.fresh.
* Game (Wine) process: no profiler ring. Its per-frame spans are CLOCK_MONOTONIC nanoseconds that
  travel in the FRAME message. The app transport thread republishes them as one committed counter
  tuple per frame: every vr.g.* / vr.s.* field once, then the commit vr.g.fid. A field belongs to
  the commit at or after it and after the previous commit; never join by nearest time.

Monotonic values are converted to the trace clock with clock.mono_minus_trace_ns, taken from the
capture's app_info (clock_mono_ns - clock_trace_ns). Frame bands end at each frame's bridge-submit
end: seven consecutive fids give six measured game frames.

Rows, top to bottom:
  Game thread     root    FRAME_SYNC begin .. bridge submit end, per fid; FRAME_SYNC, swapchain
                          waits, DXVK drain and bridge submit are drawn inline
  Shipper         detail  fence wait begin .. FRAME send end, both drawn inline
  App transport   detail  FRAME handling, ACQUIRE waits inline
  App control     detail  server side of FRAME_SYNC
  XR loop         root    one Pico OpenXR frame, its phases inline; aser pins mark snapshots
  GPU <context>   detail  optional gpu-time slices, excluded from overlap unless calibrated

Manifest:
{
  "fids": [first, ..., last] (optional; default: the first seven complete commits),
  "title": "...", "subtitle": "...", "identity": {...},
  "clock": {"mono_minus_trace_ns": <int>},
  "counters": ["counters.json", ...],     # query_counters responses with include_samples
  "slices": ["xr.json", ...],             # find_top_slices responses or {"scope": [slices]} maps
  "gpu": [{"stage": "GPU game ctx", "file": "gpu.json", "calibrated": false}],
  "xr_period_ms": 13.889
}
Only evidence present in the responses is drawn; nothing is synthesised.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

COMMIT = "vr.g.fid"
# field -> (begin counter, end counter); values are CLOCK_MONOTONIC nanoseconds.
SPANS = {
    "frame_sync": ("vr.g.frame_sync.begin_ns", "vr.g.frame_sync.end_ns"),
    "swapchain_wait_l": ("vr.g.swapchain_wait.l.begin_ns", "vr.g.swapchain_wait.l.end_ns"),
    "swapchain_wait_r": ("vr.g.swapchain_wait.r.begin_ns", "vr.g.swapchain_wait.r.end_ns"),
    "drain": ("vr.g.endframe.drain.begin_ns", "vr.g.endframe.drain.end_ns"),
    "submit": ("vr.g.endframe.submit.begin_ns", "vr.g.endframe.submit.end_ns"),
    "fence_wait": ("vr.s.fence_wait.begin_ns", "vr.s.fence_wait.end_ns"),
    "send": ("vr.s.send.begin_ns", "vr.s.send.end_ns"),
}
REQUIRED_SPANS = ("frame_sync", "drain", "submit", "fence_wait", "send")
# drain_lock: CLOCK_MONOTONIC time between DXVK's flush and its submission-queue lock.
VALUES = {"snap": "vr.g.snap", "drain_lock": "vr.g.endframe.drain.lock_ns"}
ANDROID_SERIAL = "vr.android.serial"
PRESENT_FRESH = "vr.present.fresh"
# The fid each eye shows in an XR frame, published once per drawn XR frame.
PRESENT_FID = ("vr.present.fid.l", "vr.present.fid.r")

GAME = "Game thread"
SHIPPER = "Shipper"
TRANSPORT = "App transport"
CONTROL = "App control / FRAME_SYNC"
XR = "XR loop"
GAME_LANE = "game render thread"
SHIPPER_LANE = "shipper"
INLINE_GAME = {
    "frame_sync": f"{GAME} / FRAME_SYNC",
    "swapchain_wait_l": f"{GAME} / swapchain wait L",
    "swapchain_wait_r": f"{GAME} / swapchain wait R",
    "drain": f"{GAME} / DXVK drain",
    "submit": f"{GAME} / bridge submit",
}
INLINE_SHIPPER = {"fence_wait": f"{SHIPPER} / fence wait", "send": f"{SHIPPER} / FRAME send"}
# Litep scope -> (stage, lane, parent stage or None)
APP_SCOPES = {
    "host.vr.xr.frame": (XR, "app xr thread", None),
    "host.vr.xr.wait_frame": (f"{XR} / xrWaitFrame", "app xr thread", XR),
    "host.vr.xr.input_locate": (f"{XR} / input + locate", "app xr thread", XR),
    "host.vr.projection.render": (f"{XR} / draw eyes", "app xr thread", XR),
    "host.vr.xr.end_frame": (f"{XR} / xrEndFrame", "app xr thread", XR),
    "host.vr.transport.frame": (TRANSPORT, "app transport thread", None),
    "host.vr.transport.acquire_wait": (f"{TRANSPORT} / ACQUIRE wait", "app transport thread", TRANSPORT),
    "vr.control.frame_sync": (CONTROL, "app control thread", None),
}
COLORS = {
    INLINE_GAME["frame_sync"]: "#f59e0b",
    INLINE_GAME["swapchain_wait_l"]: "#fbbf24",
    INLINE_GAME["swapchain_wait_r"]: "#fcd34d",
    INLINE_GAME["drain"]: "#f472b6",
    INLINE_GAME["submit"]: "#7ee0c0",
    INLINE_SHIPPER["fence_wait"]: "#c084fc",
    INLINE_SHIPPER["send"]: "#60a5fa",
    f"{XR} / xrWaitFrame": "#f59e0b",
    f"{XR} / input + locate": "#a3e635",
    f"{XR} / draw eyes": "#7ee0c0",
    f"{XR} / xrEndFrame": "#f8fafc",
    f"{TRANSPORT} / ACQUIRE wait": "#fda4af",
}
FRAMES = 6


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def iter_slices(document: Any) -> list[dict[str, Any]]:
    """Accept a find_top_slices response, a {"scope": [slices]} map, or a bare list."""
    if isinstance(document, list):
        return [dict(item) for item in document]
    if not isinstance(document, dict):
        return []
    if "slices" in document:
        return [dict(item) for item in document["slices"]]
    result: list[dict[str, Any]] = []
    for name, value in document.items():
        if isinstance(value, list):
            for item in value:
                if isinstance(item, dict) and "startMs" in item:
                    entry = dict(item)
                    entry.setdefault("name", name)
                    result.append(entry)
    return result


def counter_series(documents: list[Any]) -> dict[str, list[dict[str, int]]]:
    series: dict[str, list[dict[str, int]]] = {}
    for document in documents:
        for counter in document.get("counters", []):
            if counter.get("truncated"):
                raise ValueError(f'counter {counter["counterName"]!r} samples are truncated')
            series.setdefault(counter["counterName"], []).extend(
                {"time": int(sample["time"]), "value": int(sample["value"])}
                for sample in counter.get("samples", []))
    for name, samples in series.items():
        samples.sort(key=lambda sample: sample["time"])
        for previous, current in zip(samples, samples[1:]):
            if previous["time"] == current["time"]:
                raise ValueError(f"{name}: duplicate sample at {current['time']}")
    return series


def committed_tuples(series: dict[str, list[dict[str, int]]]) -> list[dict[str, Any]]:
    """One record per vr.g.fid commit; every field exactly once in (previous commit, commit]."""
    commits = series.get(COMMIT, [])
    if not commits:
        raise ValueError(f"no {COMMIT} samples: the capture lacks the game-frame tuple")
    names = {name for pair in SPANS.values() for name in pair} | set(VALUES.values())
    records: list[dict[str, Any]] = []
    previous = None
    for commit in commits:
        record: dict[str, Any] = {"fid": commit["value"], "commit_time": commit["time"]}
        for name in names:
            hits = [s["value"] for s in series.get(name, [])
                    if (previous is None or s["time"] > previous) and s["time"] <= commit["time"]]
            if len(hits) > 1:
                raise ValueError(f"{name}: {len(hits)} samples for fid {commit['value']}")
            if hits:
                record[name] = hits[0]
        records.append(record)
        previous = commit["time"]
    for left, right in zip(records, records[1:]):
        if right["fid"] != left["fid"] + 1:
            raise ValueError(f"fid sequence is not consecutive: {left['fid']} -> {right['fid']}")
    for record in records:
        for key, (begin, end) in SPANS.items():
            if (begin in record) != (end in record):
                raise ValueError(f"fid {record['fid']}: {key} has only one of its two boundaries")
    return records


def span(record: dict[str, Any], key: str, offset: int) -> tuple[int, int] | None:
    begin, end = SPANS[key]
    if begin not in record and end not in record:
        return None
    if begin not in record or end not in record:
        raise ValueError(f"fid {record['fid']}: {key} has only one of its two boundaries")
    start, stop = record[begin] - offset, record[end] - offset
    if stop < start:
        raise ValueError(f"fid {record['fid']}: {key} ends before it begins")
    return start, stop


def select(records: list[dict[str, Any]], fids: list[int] | None) -> list[dict[str, Any]]:
    complete = [r for r in records if all(name in r for key in REQUIRED_SPANS for name in SPANS[key])]
    if fids:
        chosen = [r for r in complete if r["fid"] in set(fids)]
        if [r["fid"] for r in chosen] != list(fids):
            raise ValueError(f"requested fids {fids} are not all complete in the capture")
    else:
        chosen = []
        for record in complete:
            if chosen and record["fid"] != chosen[-1]["fid"] + 1:
                chosen = []
            chosen.append(record)
            if len(chosen) == FRAMES + 1:
                break
    if len(chosen) != FRAMES + 1:
        raise ValueError(f"need {FRAMES + 1} consecutive complete game frames, found {len(chosen)}")
    return chosen


def build(manifest: dict[str, Any], base: Path) -> dict[str, Any]:
    offset = int(manifest["clock"]["mono_minus_trace_ns"])
    series = counter_series([load_json(base / path) for path in manifest.get("counters", [])])
    records = committed_tuples(series)
    # The app commits a tuple after it received the FRAME that carried it, so a converted send
    # start later than the commit means the clock offset does not describe this capture.
    for record in records:
        bounds = span(record, "send", offset)
        if bounds and bounds[0] > record["commit_time"]:
            raise ValueError(f"fid {record['fid']}: FRAME send starts {bounds[0] - record['commit_time']} ns "
                             "after the app committed it; clock.mono_minus_trace_ns is wrong for this capture")
    chosen = select(records, manifest.get("fids"))
    xr_period_ns = int(float(manifest.get("xr_period_ms", 13.889)) * 1_000_000)

    markers = [{"frame": f"fid {r['fid']}", "time_ns": span(r, "submit", offset)[1],
                "source": "vr.g.endframe.submit.end_ns (bridge submit end)", "boundary": "end"}
               for r in chosen]
    start, end = markers[0]["time_ns"], markers[-1]["time_ns"]
    intervals: list[dict[str, Any]] = []
    diagnostics: list[dict[str, Any]] = []

    # Game-process tuples. Keep frames whose spans reach into the window, including the frame
    # after the window, so work that straddles the outer boundary stays visible (it is clipped).
    tuples = [r for r in records if all(n in r for k in REQUIRED_SPANS for n in SPANS[k])]
    for record in tuples:
        token = record["fid"]
        frame_sync, submit = span(record, "frame_sync", offset), span(record, "submit", offset)
        if submit[1] < start - 200_000_000 or frame_sync[0] > end:
            continue
        intervals.append({"stage": GAME, "lane": GAME_LANE, "start_ns": frame_sync[0],
                          "end_ns": submit[1], "token": token, "id": f"fid {token}",
                          "source": "FRAME tuple vr.g.* (CLOCK_MONOTONIC)",
                          **({"note": f"snap {record['vr.g.snap']}"} if "vr.g.snap" in record else {})})
        for key, stage in INLINE_GAME.items():
            bounds = span(record, key, offset)
            if bounds:
                intervals.append({"stage": stage, "lane": GAME_LANE, "start_ns": bounds[0],
                                  "end_ns": bounds[1], "token": token, "color": COLORS[stage]})
        fence, send = span(record, "fence_wait", offset), span(record, "send", offset)
        intervals.append({"stage": SHIPPER, "lane": SHIPPER_LANE, "start_ns": fence[0],
                          "end_ns": send[1], "token": token, "id": f"fid {token}"})
        for key, stage in INLINE_SHIPPER.items():
            bounds = span(record, key, offset)
            intervals.append({"stage": stage, "lane": SHIPPER_LANE, "start_ns": bounds[0],
                              "end_ns": bounds[1], "token": token, "color": COLORS[stage]})

    ignored: dict[str, int] = {}
    for path in manifest.get("slices", []):
        for item in iter_slices(load_json(base / path)):
            name = str(item.get("name", ""))
            mapped = APP_SCOPES.get(name)
            if not mapped:
                ignored[name] = ignored.get(name, 0) + 1
                continue
            stage, lane, _ = mapped
            entry = {"stage": stage, "lane": lane, "start_ns": round(float(item["startMs"]) * 1e6),
                     "end_ns": round(float(item["endMs"]) * 1e6), "scope_name": name,
                     "source": f"Litep zone {name}"}
            if stage in COLORS:
                entry["color"] = COLORS[stage]
            intervals.append(entry)
    if ignored:
        diagnostics.append({"severity": "info", "code": "ScopeNotInLaneModel",
                            "message": "Slices outside the GameNative lane model were not drawn.",
                            "scopes": dict(sorted(ignored.items()))})

    stage_options: dict[str, dict[str, Any]] = {
        GAME: {"order": 10, "role": "root", "summary": True},
        SHIPPER: {"order": 20, "role": "detail", "count_for_concurrency": False, "summary": True},
        TRANSPORT: {"order": 30, "role": "detail", "count_for_concurrency": False, "summary": True},
        CONTROL: {"order": 40, "role": "detail", "count_for_concurrency": False, "summary": True},
        XR: {"order": 50, "role": "root", "summary": True},
    }
    for order, stage in enumerate(INLINE_GAME.values(), start=11):
        stage_options[stage] = {"order": order, "role": "detail", "parent": GAME, "layout": "inline",
                                "count_for_concurrency": False, "summary": True}
    for order, stage in enumerate(INLINE_SHIPPER.values(), start=21):
        stage_options[stage] = {"order": order, "role": "detail", "parent": SHIPPER, "layout": "inline",
                                "count_for_concurrency": False, "summary": True}
    child_order = {TRANSPORT: 31, XR: 51}
    for name, (stage, _, parent) in APP_SCOPES.items():
        if parent:
            stage_options[stage] = {"order": child_order[parent], "role": "detail", "parent": parent,
                                    "layout": "inline", "count_for_concurrency": False, "summary": True}
            child_order[parent] += 1

    alignment = "unknown"
    for order, entry in enumerate(manifest.get("gpu", []), start=60):
        stage = str(entry["stage"])
        calibrated = bool(entry.get("calibrated", False))
        alignment = "calibrated" if calibrated and alignment in ("unknown", "calibrated") else "uncalibrated"
        stage_options[stage] = {"order": order, "role": "detail", "count_for_concurrency": False,
                                "include_in_overlap": calibrated, "summary": True}
        for item in iter_slices(load_json(base / entry["file"])):
            intervals.append({"stage": stage, "lane": stage, "start_ns": round(float(item["startMs"]) * 1e6),
                              "end_ns": round(float(item["endMs"]) * 1e6),
                              "source": f"gpu {'calibrated' if calibrated else 'uncalibrated'}",
                              "count_for_concurrency": False, "include_in_overlap": calibrated})

    pins = [{"stage": XR, "time_ns": s["time"], "label": f"aser {s['value']}", "kind": "vblank",
             "source": ANDROID_SERIAL} for s in series.get(ANDROID_SERIAL, []) if start <= s["time"] <= end]

    annotations = []
    fresh = series.get(PRESENT_FRESH, [])
    shown_left, shown_right = (series.get(name, []) for name in PRESENT_FID)
    if len(shown_left) != len(shown_right):
        raise ValueError(f"{PRESENT_FID[0]} and {PRESENT_FID[1]} have different sample counts")
    for previous, current in zip(markers, markers[1:]):
        record = next(r for r in chosen if f"fid {r['fid']}" == current["frame"])
        duration = current["time_ns"] - previous["time_ns"]
        waits = sum(b - a for a, b in (span(record, k, offset) or (0, 0)
                                       for k in ("frame_sync", "swapchain_wait_l", "swapchain_wait_r", "drain")))
        fresh_eyes = sum(s["value"] for s in fresh if previous["time_ns"] < s["time"] <= current["time_ns"])
        ticks = duration / xr_period_ns
        badges = [f"{duration / 1e6:.1f} ms", f"{ticks:.1f} XR", f"wait {waits / 1e6:.1f}"]
        if fresh:
            badges.append(f"fresh {fresh_eyes}")
        metrics: dict[str, Any] = {"duration_ms": round(duration / 1e6, 3), "xr_ticks": round(ticks, 2),
                                   "waits_ms": round(waits / 1e6, 3), "fresh_eyes": fresh_eyes}
        if "vr.g.snap" in record:
            metrics["snap"] = record["vr.g.snap"]
        if "vr.g.endframe.drain.lock_ns" in record:
            metrics["drain_lock_ms"] = round((record["vr.g.endframe.drain.end_ns"]
                                              - record["vr.g.endframe.drain.lock_ns"]) / 1e6, 3)
        if shown_left:
            # Submit end to the first XR frame that draws this fid in the left eye; how many XR
            # frames show it; how many XR frames in this band show different fids per eye.
            fid = record["fid"]
            first = next((s["time"] for s in shown_left
                          if s["value"] >= fid and s["time"] >= current["time_ns"]), None)
            if first is not None:
                metrics["present_latency_ms"] = round((first - current["time_ns"]) / 1e6, 3)
                badges.append(f"lat {metrics['present_latency_ms']:.1f}")
            metrics["shown_xr_frames"] = sum(1 for s in shown_left if s["value"] == fid)
            metrics["eye_fid_mismatch"] = sum(
                1 for left, right in zip(shown_left, shown_right)
                if previous["time_ns"] < left["time"] <= current["time_ns"] and left["value"] != right["value"])
        annotations.append({"frame": current["frame"], "class": "slip" if ticks >= 3 else "",
                            "badges": badges, "metrics": metrics})

    fids = [r["fid"] for r in chosen[1:]]
    identity = dict(manifest.get("identity", {}))
    identity["clock_alignment"] = f"mono_minus_trace_ns={offset}"
    return {
        "schema_version": 2,
        "time_domain": "trace_cpu_ns",
        "gpu_cpu_alignment": alignment,
        "title": manifest.get("title", "GameNative XR stage concurrency"),
        "subtitle": manifest.get("subtitle", ""),
        "identity": identity,
        "window": {"start_ns": start, "end_ns": end},
        "frame_markers": markers,
        "intervals": intervals,
        "stage_options": stage_options,
        "markers": pins,
        "frame_annotations": annotations,
        # Every frame band contains its own drain and submit. The shipper of the last fid starts
        # at the window end, so it is not part of the in-window contract.
        "token_contracts": [{"name": "fid", "tokens": fids, "stages": {
            GAME: {"min": 1, "max": 1}, INLINE_GAME["drain"]: {"min": 1, "max": 1},
            INLINE_GAME["submit"]: {"min": 1, "max": 1}}}],
        "diagnostics": diagnostics,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        bundle = build(load_json(args.manifest), args.manifest.parent)
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f"{error}\n")
    if args.output.exists():
        parser.exit(1, f"{args.output} exists; choose a new path\n")
    args.output.write_text(json.dumps(bundle, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "intervals": len(bundle["intervals"]),
                      "frames": len(bundle["frame_markers"]) - 1}))


if __name__ == "__main__":
    sys.exit(main())
