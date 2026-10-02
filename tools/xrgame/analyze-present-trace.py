#!/usr/bin/env python3
"""Summarize bounded XRGamePresentTrace logcat captures; never call fence wait GPU time."""
import argparse
import json
import pathlib
import re
import statistics


def analyze(lines):
    frames = {}
    diagnostics = []
    for number, line in enumerate(lines, 1):
        if "XRGamePresentTrace" not in line or "event=" not in line:
            continue
        fields = dict(re.findall(r"([a-z_]+)=([^\s]+)", line))
        if "frame" not in fields:
            continue
        frame = frames.setdefault(fields["frame"], {})
        event = fields["event"]
        if event in frame:
            diagnostics.append({"frame": fields["frame"], "error": "duplicate " + event, "line": number})
        frame[event] = fields

    samples = []
    incomplete = []
    nonzero_wait_fences = 0
    for frame_id, events in frames.items():
        sampled = "sample_ready" in events or events.get("complete", {}).get("mode") == "sample"
        required = ("receive", "sample_ready", "complete", "sample_retired", "idle") if sampled else ("receive", "copy_done", "complete", "idle")
        asynchronous = events.get("complete", {}).get("mode") == "async"
        if asynchronous:
            required += ("enqueue_return", "copy_dequeue")
        missing = sorted(set(required) - events.keys())
        if missing:
            incomplete.append({"frame": frame_id, "missing": missing})
            continue
        try:
            receive, complete, idle = (events[e] for e in ("receive", "complete", "idle"))
            for event in (complete, idle):
                if any(receive[k] != event[k] for k in ("generation", "window", "pixmap", "serial")):
                    raise ValueError("frame identity changed")
            if sampled:
                times = [int(events[e]["mono_ns"]) for e in required]
                if times != sorted(times):
                    raise ValueError("sample Idle preceded GPU retirement or Complete")
                samples.append({"frame": frame_id, "generation": receive["generation"],
                                "window": receive["window"], "pixmap": receive["pixmap"], "path": "sample",
                                "sample_accept_ns": times[1] - times[0],
                                "lease_ns": times[3] - times[1], "receive_to_idle_ns": times[4] - times[0]})
                nonzero_wait_fences += int(int(receive["wait_fence"]) != 0)
                continue
            copy = events["copy_done"]
            nonzero_wait_fences += int(int(receive["wait_fence"]) != 0)
            times = [int(receive["mono_ns"]), int(copy["start_ns"]), int(copy["locked_ns"]),
                     int(copy["submitted_ns"]), int(copy["fence_done_ns"]), int(copy["mono_ns"]),
                     int(complete["mono_ns"]), int(idle["mono_ns"])]
            if times != sorted(times) or int(copy["result"]) != 0:
                raise ValueError("non-monotonic events or failed copy")
            for event in (complete, idle):
                if any(receive[k] != event[k] for k in ("generation", "window", "pixmap", "serial")):
                    raise ValueError("frame identity changed")
            sample = {"frame": frame_id, "generation": receive["generation"],
                            "window": receive["window"], "pixmap": receive["pixmap"],
                            "path": "async_copy" if asynchronous else "copy",
                            "copy_wall_ns": times[5] - times[1], "lock_wait_ns": times[2] - times[1],
                            "fence_wait_ns": times[4] - times[3], "receive_to_idle_ns": times[7] - times[0]}
            if asynchronous:
                returned = int(events["enqueue_return"]["mono_ns"])
                dequeued = int(events["copy_dequeue"]["mono_ns"])
                if returned < times[0] or not times[0] <= dequeued <= times[1]:
                    raise ValueError("invalid asynchronous admission timeline")
                # Dequeue can precede JNI's return; these are concurrent threads.
                sample["receive_to_enqueue_return_ns"] = returned - times[0]
                sample["receive_to_dequeue_ns"] = dequeued - times[0]
            samples.append(sample)
        except (KeyError, ValueError) as error:
            diagnostics.append({"frame": frame_id, "error": str(error)})

    summary = {}
    for key in ("copy_wall_ns", "lock_wait_ns", "fence_wait_ns", "sample_accept_ns", "lease_ns", "receive_to_idle_ns",
                "receive_to_enqueue_return_ns", "receive_to_dequeue_ns"):
        values = sorted(sample[key] for sample in samples if key in sample)
        if values:
            summary[key] = {"mean": statistics.mean(values), "p50": statistics.median(values),
                            "p95": values[min(len(values) - 1, (95 * len(values) + 99) // 100 - 1)], "max": max(values)}
    return {"clock": "host CLOCK_MONOTONIC; no GPU timestamps or scanout-completion proof",
            "framesSeen": len(frames), "completeFrames": len(samples), "incomplete": incomplete,
            "nonzeroWaitFences": nonzero_wait_fences, "diagnostics": diagnostics,
            "summary": summary, "samples": samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = analyze(args.log.read_text(errors="replace").splitlines())
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("samples", "incomplete")}, indent=2))
    return 1 if result["diagnostics"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
