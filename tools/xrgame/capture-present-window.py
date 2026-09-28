#!/usr/bin/env python3
"""Bounded, read-only CPU/RSS and SurfaceFlinger window for flat-path comparisons."""
import argparse
import json
import pathlib
import shlex
import statistics
import subprocess
import time

from debug_identity import Device, IdentityError, parse_stat


def summarize(samples, ticks, page_bytes):
    if len(samples) < 2:
        return {"incomplete": True}
    duration = samples[-1]["monoNs"] - samples[0]["monoNs"]
    processes = []
    for index in range(len(samples[0]["processes"])):
        values = [row["processes"][index] for row in samples]
        if len({(p["pid"], p["startTimeTicks"]) for p in values}) != 1:
            raise IdentityError("Cannot summarize mixed process generations")
        processes.append({"pid": values[0]["pid"],
                          "cpuCoresMean": (values[-1]["cpuTicks"] - values[0]["cpuTicks"]) / ticks / (duration / 1e9),
                          "rssMiBMean": statistics.mean(p["rssPages"] for p in values) * page_bytes / 1048576,
                          "rssMiBMax": max(p["rssPages"] for p in values) * page_bytes / 1048576})
    # AOSP FrameTracker::dumpStats: desired, actual present, frame ready (ns).
    # Only join adjacent records in the SAME dump. Polling gaps are not jank.
    pairs = set()
    for row in samples:
        actual = sorted({int(line.split()[1]) for line in row.get("surfaceLatency", "").splitlines()
                         if len(line.split()) == 3 and 0 < int(line.split()[1]) < (1 << 63) - 1})
        pairs.update(zip(actual, actual[1:]))
    intervals = sorted((b - a) / 1e6 for a, b in pairs if b > a)
    surface = {"uniqueAdjacentIntervals": len(intervals), "coveredSeconds": sum(intervals) / 1000}
    if intervals:
        surface.update({"meanMs": statistics.mean(intervals), "p50Ms": statistics.median(intervals),
                        "p95Ms": intervals[min(len(intervals) - 1, (95 * len(intervals) + 99) // 100 - 1)],
                        "maxMs": max(intervals)})
    return {"durationSeconds": duration / 1e9, "processes": processes, "surfacePresentIntervals": surface}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--app", default="com.tencentmalos.xrgamenative")
    parser.add_argument("--app-pid", type=int, required=True)
    parser.add_argument("--guest-pid", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--surface", help="Exact SurfaceFlinger layer, if automatic selection is ambiguous")
    parser.add_argument("--apk-audit", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if repo == output or repo in output.parents or not 5 <= args.seconds <= 300:
        parser.error("Use a private output outside the repository and a 5–300 second window")
    output.parent.mkdir(parents=True, exist_ok=True)
    device = Device(args.serial, args.app)
    def shell(*values):
        return subprocess.check_output(device.adb + ["shell", shlex.join(values)], timeout=15).decode().strip()
    record = {"schemaVersion": 1, "samples": [], "status": "incomplete",
              "apkAudit": json.loads(args.apk_audit.read_text()),
              "limitations": "CPU counts are per process; RSS includes shared pages. KGSL intervals may be reset by other readers. SurfaceFlinger is host presentation, not guest simulation or GPU execution time."}
    # Reserve the output and preserve partial evidence even on a capture failure.
    with output.open("x") as stream:
        try:
            apks = [line.removeprefix("package:") for line in shell("pm", "path", args.app).splitlines()]
            if len(apks) != 1 or not apks[0].endswith("/base.apk"):
                raise IdentityError("Capture currently requires one installed base APK")
            record["installedApkSha256"] = shell("sha256sum", apks[0]).split()[0]
            if record["installedApkSha256"] != record["apkAudit"].get("apkSha256"):
                raise IdentityError("Installed APK does not match the supplied audit")
            identities = [device.identity(pid) for pid in (args.app_pid, args.guest_pid)]
            record["identities"] = identities
            record["deviceModel"] = shell("getprop", "ro.product.model")
            record["deviceBuild"] = shell("getprop", "ro.build.fingerprint")
            record["clkTck"] = ticks = int(shell("getconf", "CLK_TCK"))
            record["pageBytes"] = pages = int(shell("getconf", "PAGESIZE"))
            surfaces = shell("dumpsys", "SurfaceFlinger", "--list").splitlines()
            candidates = [name for name in surfaces if "SurfaceView[" in name and args.app in name and "(BLAST)" in name]
            if args.surface:
                if args.surface not in surfaces: raise IdentityError("Selected surface is absent")
                surface = args.surface
            else:
                if len(candidates) != 1: raise IdentityError("Select one exact app SurfaceFlinger layer")
                surface = candidates[0]
            record["surface"] = surface
            start = time.monotonic()
            for index in range(args.seconds + 1):
                row = {"monoNs": time.monotonic_ns(), "wallSeconds": time.time(), "processes": []}
                if device.read("cat", "/proc/sys/kernel/random/boot_id").strip() != identities[0]["bootId"]:
                    raise IdentityError("Device rebooted")
                for identity in identities:
                    pid = identity["pid"]
                    raw = device.read("cat", f"/proc/{pid}/stat")
                    state = parse_stat(raw, pid)
                    if state["startTimeTicks"] != identity["startTimeTicks"] or state["state"] in {"Z", "X", "x"}:
                        raise IdentityError("Process exited or generation changed")
                    fields = raw.rsplit(")", 1)[1].split()
                    row["processes"].append({"pid": pid, **state, "cpuTicks": int(fields[11]) + int(fields[12]),
                                             "rssPages": int(fields[21])})
                row["surfaceLatency"] = shell("dumpsys", "SurfaceFlinger", "--latency", surface)
                for key, path in (("gpuBusyRaw", "/sys/class/kgsl/kgsl-3d0/gpubusy"),
                                  ("gpuFreqRaw", "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq"),
                                  ("batteryTempRaw", "/sys/class/power_supply/battery/temp")):
                    try: row[key] = shell("cat", path)
                    except subprocess.SubprocessError: row[key] = None
                record["samples"].append(row)
                if index < args.seconds: time.sleep(max(0, start + index + 1 - time.monotonic()))
            for identity in identities: device.identity(identity["pid"], identity)
            record["summary"] = summarize(record["samples"], ticks, pages)
            record["status"] = "complete"
        except (IdentityError, ValueError, subprocess.SubprocessError) as error:
            record["failure"] = type(error).__name__
        finally:
            json.dump(record, stream, indent=2)
            stream.write("\n")
    print(json.dumps({"status": record["status"], "summary": record.get("summary")}, indent=2))
    return 0 if record["status"] == "complete" else 1


if __name__ == "__main__":
    raise SystemExit(main())
