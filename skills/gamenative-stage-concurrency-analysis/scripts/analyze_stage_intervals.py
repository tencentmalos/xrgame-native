#!/usr/bin/env python3
"""Analyze GameNative stage intervals and render a self-contained deterministic SVG.

Engine vendored from Azahar skills/azahar-stage-concurrency-analysis (87789aaad); see
references/provenance.md. Domain identifiers are GameNative; interval math is unchanged.

The input may contain normalized ``intervals`` in trace-relative nanoseconds, litep
``find_top_slices`` groups in ``ptracy_slices``, or both. See
``references/stage-bundle.schema.json`` for the version-2 contract.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import math
import os
import re
import tempfile
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


TOOL_VERSION = "3.0"

# Instant events drawn on a stage row (or across the chart when they carry no stage). The
# kind selects the glyph colour; anything else falls back to the neutral marker colour.
MARKER_COLORS = {
    "p3d": "#ff6b7a",
    "ppf": "#ffb84d",
    "vblank": "#52657f",
    "swap": "#7ee0c0",
    "deadline": "#f472b6",
}
FRAME_CLASS_FILLS = {
    "slip": ("#2a1420", "#3a1d2a"),
    "warn": ("#2a2412", "#3a3119"),
}
Interval = tuple[int, int]

SOURCE_ROLE_LABELS = {
    "record": "Record",
    "source_candidate": "Candidate",
    "replay": "Replay",
    "auxiliary": "Aux",
    "unknown": "Unknown",
}
SOURCE_ROLE_COLORS = {
    "record": "#39c6a3",
    "source_candidate": "#22d3ee",
    "replay": "#a78bfa",
    "auxiliary": "#64748b",
    "unknown": "#ffb84d",
}
DISPLAY_TARGET_LABELS = {
    "unknown": "Unknown",
    "top_left": "Top Left",
    "top_right": "Top Right",
    "bottom": "Bottom",
    "offscreen": "Offscreen",
}
SCREEN_LABELS = {
    "top": "Top",
    "bottom": "Bottom",
    "mixed": "Mixed",
    "unknown": "Unknown",
}

WORKLOAD_COUNTERS = {
    "command_lists": "GPU Stage Workload Command Lists",
    "draw_calls": "GPU Stage Workload Draw Calls",
    "indexed_draw_calls": "GPU Stage Workload Indexed Draw Calls",
    "vertices": "GPU Stage Workload Vertices",
}
OPTIONAL_WORKLOAD_COUNTERS = {
    "pica_frame_id": "GPU Stage Workload PICA Frame Id",
    "boundary_source": "GPU Stage Workload Boundary Source",
}
# VideoCore::RenderEpochBoundarySource. A generation that closed on the Guest's own staged top
# preset carries a different provenance from one the Host V-Blank fallback had to close, and the
# two are not interchangeable when reading frame attribution.
BOUNDARY_SOURCE_LABELS = {
    0: "none",
    1: "top-preset",
    2: "vblank-fallback",
}
WORKLOAD_COMMIT_COUNTER = "GPU Stage Workload Completed Sequence"
DRAW_COUNTERS = {
    "pica_frame_id": "PICA Active Frame Id",
    "cmdlist_execution_sequence": "PICA Active CmdList Execution Sequence",
    "submission_id": "PICA Active Submission Id",
    "segment_sequence": "PICA Active Segment Sequence",
    "command_word_offset": "PICA Active Command Word Offset",
    "metadata": "PICA Active Draw Metadata",
}
DRAW_COMMIT_COUNTER = "PICA Active Draw Event Sequence"

SEMANTIC_KEYS = {
    "schema",
    "stage_id",
    "lane",
    "eye",
    "source_role",
    "output_role",
    "pass",
}
# Per-interval measurements carried through to the detail panel. A stage lane on
# its own says when work ran but not what the work was; a command list without
# its draw count cannot be told apart from any other command list.
DETAIL_METRICS = (
    ("draws", "draws"),
    ("draw_count", "draws"),
    ("commands", "cmds"),
    ("register_writes", "reg writes"),
    ("captured_bytes", "B captured"),
    ("records", "records"),
    ("hits", "hits"),
    ("fallbacks", "fallbacks"),
    ("suffix_bytes", "suffix B"),
    ("pica_frame_id", "PICA frame"),
    ("screen", "screen"),
    ("display_targets", "targets"),
    ("target", "target"),
    ("merged_count", "zones merged"),
    ("merged_busy_ms", "ms busy"),
    ("note", ""),
)
EYE_ALIASES = {"L": "left", "R": "right", "left": "left", "right": "right"}
SUFFIX_PATTERN = re.compile(r"\s*\[([^\[\]]+)\]\s*$")


def parse_scope_semantics(name: str) -> tuple[str, dict[str, str]]:
    """Parse only versioned or recognized machine-key groups at the end of a scope name."""
    remaining = name.rstrip()
    semantics: dict[str, str] = {}
    while match := SUFFIX_PATTERN.search(remaining):
        payload = match.group(1).strip()
        tokens = payload.split()
        pairs: list[tuple[str, str]] = []
        recognized = False
        for token in tokens:
            if "=" not in token:
                pairs = []
                break
            key, value = token.split("=", 1)
            if not key or not value:
                pairs = []
                break
            pairs.append((key, value))
            recognized |= key in SEMANTIC_KEYS
        if not pairs or not recognized or any(key not in SEMANTIC_KEYS for key, _ in pairs):
            break
        for key, value in pairs:
            if key == "eye":
                value = EYE_ALIASES.get(value, value)
                if value not in {
                    "left",
                    "right",
                    "mono",
                    "stereo_pair",
                    "unknown",
                    "not_applicable",
                    "unscoped",
                }:
                    raise ValueError(f"invalid eye semantic {value!r} in scope {name!r}")
            if key in semantics:
                raise ValueError(f"duplicate semantic key {key!r} in scope {name!r}")
            semantics[key] = value
        remaining = remaining[: match.start()].rstrip()
    return remaining, semantics


def merge_scope_semantics(
    item: dict[str, Any], scope_name: str | None
) -> tuple[str | None, dict[str, str]]:
    scope_base: str | None = None
    parsed: dict[str, str] = {}
    if scope_name:
        scope_base, parsed = parse_scope_semantics(scope_name)
    result = dict(parsed)
    for key in SEMANTIC_KEYS - {"schema"}:
        if key not in item:
            continue
        value = str(item[key])
        if key == "eye":
            value = EYE_ALIASES.get(value, value)
        if key in result and result[key] != value:
            raise ValueError(
                f"scope semantic conflict for {key}: suffix={result[key]!r} explicit={value!r}"
            )
        result[key] = value
    return scope_base, result


def merge_intervals(intervals: Iterable[Interval]) -> list[Interval]:
    merged: list[list[int]] = []
    for start, end in sorted(intervals):
        if not merged or start > merged[-1][1]:
            merged.append([start, end])
        else:
            merged[-1][1] = max(merged[-1][1], end)
    return [(start, end) for start, end in merged]


def duration(intervals: Iterable[Interval]) -> int:
    return sum(end - start for start, end in intervals)


def intersection_duration(left: list[Interval], right: list[Interval]) -> int:
    total = 0
    i = 0
    j = 0
    while i < len(left) and j < len(right):
        start = max(left[i][0], right[j][0])
        end = min(left[i][1], right[j][1])
        total += max(0, end - start)
        if left[i][1] <= right[j][1]:
            i += 1
        else:
            j += 1
    return total


def percentile(values: list[int], probability: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = max(0, math.ceil(probability * len(ordered)) - 1)
    return ordered[index]


def ns_to_ms(value: int) -> float:
    return round(value / 1_000_000.0, 6)


def clamp_text(value: Any, length: int) -> str:
    text = str(value)
    return text if len(text) <= length else text[: max(1, length - 1)] + "…"


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def diagnostic(severity: str, code: str, message: str, **details: Any) -> dict[str, Any]:
    return {"severity": severity, "code": code, "message": message, **details}


def normalize_ptracy_slices(data: dict[str, Any]) -> list[dict[str, Any]]:
    intervals: list[dict[str, Any]] = []
    for group_index, group in enumerate(data.get("ptracy_slices", [])):
        stage = str(group.get("stage", "")).strip()
        if not stage:
            raise ValueError(f"ptracy_slices group {group_index} requires stage")
        exact_name = str(group.get("name_exact", "")).strip()
        base_name = str(group.get("name_base", "")).strip()
        response = group.get("response", group)
        slices = response.get("slices", []) if isinstance(response, dict) else []
        for slice_index, item in enumerate(slices):
            scope_name = str(item.get("name", "")).strip()
            scope_base, semantics = merge_scope_semantics({**group, **item}, scope_name or None)
            if exact_name and scope_name != exact_name:
                continue
            if base_name and scope_base != base_name:
                continue
            if "startMs" not in item or "endMs" not in item:
                raise ValueError(
                    f"ptracy_slices group {group_index} slice {slice_index} requires startMs/endMs"
                )
            lane = str(group.get("lane") or item.get("threadName") or item.get("threadId") or "unknown")
            interval = {
                "stage": stage,
                "lane": lane,
                "start_ns": round(float(item["startMs"]) * 1_000_000),
                "end_ns": round(float(item["endMs"]) * 1_000_000),
                "source": scope_name or group.get("source", "ptracy/find_top_slices"),
            }
            if scope_name:
                interval["scope_name"] = scope_name
                interval["scope_base"] = scope_base
            interval.update(semantics)
            # Measurements the slice carried. These were being dropped, so a
            # command list reached the chart as a bare coloured box even when the
            # trace had said how many draws it held.
            carried = ("token", "id", "kind", "target", "color", "cmdlist_execution_sequence",
                       "submission_id", "replay_group_id") + tuple(
                key for key, _ in DETAIL_METRICS
            )
            for key in carried:
                if key in item:
                    interval[key] = item[key]
                elif key in group:
                    interval[key] = group[key]
            for key in ("count_for_concurrency", "include_in_overlap"):
                if key in group:
                    interval[key] = bool(group[key])
            intervals.append(interval)
        gap = int(group.get("coalesce_gap_ns", 0) or 0)
        if gap > 0:
            merged = coalesce_intervals(intervals[len(intervals) - len(slices):], gap)
            del intervals[len(intervals) - len(slices):]
            intervals.extend(merged)
    return intervals


def coalesce_intervals(items: list[dict[str, Any]], gap_ns: int) -> list[dict[str, Any]]:
    """Merge same-lane intervals whose gaps are shorter than gap_ns.

    Hundreds of sub-millisecond GPU render passes are one lane of noise on a six-frame
    chart. The merged interval keeps the count and the busy sum so the detail panel still
    says how much work the block held; the union geometry is what the row shows.
    """
    by_lane: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for item in items:
        by_lane[str(item["lane"])].append(item)
    merged: list[dict[str, Any]] = []
    for lane_items in by_lane.values():
        lane_items.sort(key=lambda value: (value["start_ns"], value["end_ns"]))
        current: dict[str, Any] | None = None
        for item in lane_items:
            if current is not None and item["start_ns"] - current["end_ns"] <= gap_ns:
                current["end_ns"] = max(current["end_ns"], item["end_ns"])
                current["merged_count"] += 1
                current["_busy_ns"] += item["end_ns"] - item["start_ns"]
                continue
            if current is not None:
                merged.append(current)
            current = {
                **item,
                "merged_count": 1,
                "_busy_ns": item["end_ns"] - item["start_ns"],
            }
        if current is not None:
            merged.append(current)
    for item in merged:
        item["merged_busy_ms"] = ns_to_ms(item.pop("_busy_ns"))
        if item["merged_count"] > 1:
            item["source"] = f'{item.get("source", "")} (coalesced)'.strip()
    merged.sort(key=lambda value: value["start_ns"])
    return merged


def normalize_markers(data: dict[str, Any], start: int, end: int) -> list[dict[str, Any]]:
    markers: list[dict[str, Any]] = []
    for index, item in enumerate(data.get("markers", [])):
        if "time_ns" not in item:
            raise ValueError(f"markers item {index} requires time_ns")
        timestamp = int(item["time_ns"])
        if timestamp < start or timestamp > end:
            continue
        markers.append(
            {
                "stage": str(item.get("stage", "")).strip() or None,
                "lane": str(item.get("lane", "")).strip() or None,
                "time_ns": timestamp,
                "label": str(item.get("label", "")),
                "kind": str(item.get("kind", "")).strip().lower(),
                "source": str(item.get("source", "")),
            }
        )
    markers.sort(key=lambda value: value["time_ns"])
    return markers


def normalize_frame_annotations(data: dict[str, Any]) -> dict[str, dict[str, Any]]:
    annotations: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(data.get("frame_annotations", [])):
        if "frame" not in item:
            raise ValueError(f"frame_annotations item {index} requires frame")
        badges = [str(value) for value in item.get("badges", [])]
        annotations[str(item["frame"])] = {
            "frame": item["frame"],
            "class": str(item.get("class", "")).strip().lower(),
            "badges": badges,
            "note": str(item.get("note", "")),
            "metrics": dict(item.get("metrics", {})),
        }
    return annotations


def normalize_frame_markers(
    data: dict[str, Any], start: int, end: int
) -> list[dict[str, Any]]:
    markers: list[dict[str, Any]] = []
    boundaries: set[str] = set()
    for index, item in enumerate(data.get("frame_markers", [])):
        if "frame" not in item or "time_ns" not in item:
            raise ValueError(f"frame_markers item {index} requires frame and time_ns")
        timestamp = int(item["time_ns"])
        boundary = str(item.get("boundary", "begin"))
        if boundary not in {"begin", "end"}:
            raise ValueError(f"frame_markers item {index} boundary must be begin or end")
        boundaries.add(boundary)
        if start <= timestamp <= end:
            default_source = (
                "GPU Stage Workload Completed Sequence"
                if boundary == "end"
                else "GPU Stage VBlank Token"
            )
            markers.append(
                {
                    "frame": item["frame"],
                    "time_ns": timestamp,
                    "source": str(item.get("source", default_source)),
                    "boundary": boundary,
                }
            )
    markers.sort(key=lambda item: item["time_ns"])
    if len(markers) < 2:
        raise ValueError("frame_markers requires at least two boundaries inside the window")
    if len(boundaries) != 1:
        raise ValueError("all frame_markers must use the same boundary direction")
    for previous, current in zip(markers, markers[1:]):
        if current["time_ns"] <= previous["time_ns"]:
            raise ValueError("frame_markers time_ns values must be strictly increasing")
        if str(current["frame"]) == str(previous["frame"]):
            raise ValueError("adjacent frame_markers must have distinct frame identifiers")
    if markers[0]["time_ns"] != start or markers[-1]["time_ns"] != end:
        raise ValueError(
            "window.start_ns/end_ns must equal the first/last frame marker so only complete frames are charted"
        )
    return markers


def extract_ptracy_counter_series(data: dict[str, Any]) -> dict[str, list[dict[str, int]]]:
    series: dict[str, list[dict[str, int]]] = defaultdict(list)
    for query_index, query in enumerate(data.get("ptracy_counter_queries", [])):
        if query.get("missingCounterNames") or query.get("emptyCounterNames"):
            raise ValueError(
                f"ptracy_counter_queries[{query_index}] has missing or empty counter names"
            )
        for counter in query.get("counters", []):
            name = str(counter.get("counterName", ""))
            if not name:
                raise ValueError(
                    f"ptracy_counter_queries[{query_index}] contains an unnamed counter"
                )
            if counter.get("truncated"):
                raise ValueError(f"ptracy counter {name!r} is truncated")
            available = int(counter.get("availableSampleCount", len(counter.get("samples", []))))
            samples = counter.get("samples", [])
            if available != len(samples):
                raise ValueError(
                    f"ptracy counter {name!r} reports {available} samples but exports {len(samples)}"
                )
            for sample in samples:
                series[name].append(
                    {"time_ns": int(sample["time"]), "value": int(sample["value"])}
                )
    for name, samples in series.items():
        samples.sort(key=lambda item: item["time_ns"])
        if any(
            current["time_ns"] <= previous["time_ns"]
            for previous, current in zip(samples, samples[1:])
        ):
            raise ValueError(f"ptracy counter {name!r} sample times must be strictly increasing")
    return series


def reconstruct_committed_tuples(
    series: dict[str, list[dict[str, int]]],
    commit_name: str,
    fields: dict[str, str],
    start: int,
    end: int,
    *,
    allow_incomplete_first: bool = False,
) -> list[dict[str, int]]:
    commits = [
        sample
        for sample in series.get(commit_name, [])
        if start <= sample["time_ns"] <= end
    ]
    if not commits:
        raise ValueError(f"ptracy counter query does not contain commit counter {commit_name!r}")
    cursors = {name: 0 for name in fields}
    result: list[dict[str, int]] = []
    previous_commit_time = start - 1
    for commit_index, commit in enumerate(commits):
        values: dict[str, int] = {}
        failure: str | None = None
        for field, counter_name in fields.items():
            samples = series.get(counter_name, [])
            cursor = cursors[field]
            while cursor < len(samples) and samples[cursor]["time_ns"] <= previous_commit_time:
                cursor += 1
            begin = cursor
            while cursor < len(samples) and samples[cursor]["time_ns"] <= commit["time_ns"]:
                cursor += 1
            cursors[field] = cursor
            candidates = samples[begin:cursor]
            if len(candidates) != 1:
                failure = (
                    f"commit {commit_name}={commit['value']} requires exactly one {counter_name} "
                    f"sample after the previous commit; got {len(candidates)}"
                )
                break
            values[field] = candidates[0]["value"]
        if failure:
            if allow_incomplete_first and commit_index == 0:
                previous_commit_time = commit["time_ns"]
                continue
            raise ValueError(failure)
        result.append(
            {
                **values,
                "sequence": commit["value"],
                "time_ns": commit["time_ns"],
            }
        )
        previous_commit_time = commit["time_ns"]
    return result


def materialize_ptracy_counter_queries(data: dict[str, Any], start: int, end: int) -> None:
    if not data.get("ptracy_counter_queries"):
        return
    series = extract_ptracy_counter_series(data)
    workload_commits = [
        sample
        for sample in series.get(WORKLOAD_COMMIT_COUNTER, [])
        if start <= sample["time_ns"] <= end
    ]
    if not data.get("frame_markers"):
        data["frame_markers"] = [
            {
                "frame": sample["value"],
                "time_ns": sample["time_ns"],
                "source": WORKLOAD_COMMIT_COUNTER,
                "boundary": "end",
            }
            for sample in workload_commits
        ]
    if not data.get("frame_workloads"):
        workload_counters = dict(WORKLOAD_COUNTERS)
        workload_counters.update(
            {
                field: counter_name
                for field, counter_name in OPTIONAL_WORKLOAD_COUNTERS.items()
                if counter_name in series
            }
        )
        data["frame_workloads"] = [
            {
                "frame": item.pop("sequence"),
                **item,
                "source": "ptracy committed workload tuple",
            }
            for item in reconstruct_committed_tuples(
                series,
                WORKLOAD_COMMIT_COUNTER,
                workload_counters,
                start,
                end,
                allow_incomplete_first=True,
            )
        ]
    if not data.get("draw_events"):
        data["draw_events"] = [
            {**item, "source": "ptracy committed PICA draw tuple"}
            for item in reconstruct_committed_tuples(
                series, DRAW_COMMIT_COUNTER, DRAW_COUNTERS, start, end
            )
            if item["time_ns"] < end
        ]


def frame_intervals_from_markers(markers: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Build measured frames without assuming a counter sample begins a frame.

    `GPU Stage Workload Completed Sequence` is emitted after the workload snapshot, so sample F10 is
    the end of F10 and the band until sample F11 belongs to F11. Legacy begin markers retain the
    previous behavior.
    """
    boundary = str(markers[0].get("boundary", "begin"))
    result: list[dict[str, Any]] = []
    for marker, following in zip(markers, markers[1:]):
        identity = marker["frame"] if boundary == "begin" else following["frame"]
        result.append(
            {
                "frame": identity,
                "start_ns": marker["time_ns"],
                "end_ns": following["time_ns"],
                "duration_ms": ns_to_ms(following["time_ns"] - marker["time_ns"]),
                "boundary": boundary,
            }
        )
    return result


def normalize_frame_workloads(data: dict[str, Any], start: int, end: int) -> list[dict[str, Any]]:
    workloads: list[dict[str, Any]] = []
    for index, item in enumerate(data.get("frame_workloads", [])):
        if "frame" not in item or "time_ns" not in item or "draw_calls" not in item:
            raise ValueError(
                f"frame_workloads item {index} requires frame, time_ns, and draw_calls"
            )
        timestamp = int(item["time_ns"])
        if start <= timestamp <= end:
            workloads.append(
                {
                    **item,
                    "time_ns": timestamp,
                    "draw_calls": int(item["draw_calls"]),
                    "command_lists": int(item.get("command_lists", 0)),
                    "indexed_draw_calls": int(item.get("indexed_draw_calls", 0)),
                    "vertices": int(item.get("vertices", 0)),
                }
            )
    workloads.sort(key=lambda item: item["time_ns"])
    if len({str(item["frame"]) for item in workloads}) != len(workloads):
        raise ValueError("frame_workloads contains duplicate completed frame identities")
    return workloads


def normalize_draw_events(data: dict[str, Any], start: int, end: int) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    seen_sequences: set[int] = set()
    role_from_value = {0: "unknown", 1: "source_candidate", 2: "replay"}
    view_from_value = {0: "unknown", 1: "left", 2: "right"}
    target_from_value = {
        0: "unknown",
        1: "top_left",
        2: "top_right",
        3: "bottom",
        4: "offscreen",
    }
    for index, item in enumerate(data.get("draw_events", [])):
        required = {"sequence", "time_ns", "pica_frame_id", "cmdlist_execution_sequence"}
        missing = sorted(required - item.keys())
        if missing:
            raise ValueError(f"draw_events item {index} missing {', '.join(missing)}")
        sequence = int(item["sequence"])
        if sequence in seen_sequences:
            raise ValueError(f"draw_events contains duplicate sequence {sequence}")
        seen_sequences.add(sequence)
        timestamp = int(item["time_ns"])
        metadata = int(item.get("metadata", 0))
        role = str(item.get("source_role", role_from_value.get(metadata & 0xFF, "unknown")))
        if role not in SOURCE_ROLE_LABELS:
            raise ValueError(f"draw_events item {index} has unsupported source_role {role!r}")
        if start <= timestamp < end:
            events.append(
                {
                    **item,
                    "sequence": sequence,
                    "time_ns": timestamp,
                    "pica_frame_id": int(item["pica_frame_id"]),
                    "cmdlist_execution_sequence": int(item["cmdlist_execution_sequence"]),
                    "submission_id": int(item.get("submission_id", 0)),
                    "segment_sequence": int(item.get("segment_sequence", 0)),
                    "command_word_offset": int(item.get("command_word_offset", 0)),
                    "source_role": role,
                    "eye": str(item.get("eye", view_from_value.get((metadata >> 8) & 0xFF, "unknown"))),
                    "indexed": bool(item.get("indexed", (metadata >> 16) & 1)),
                    "display_target": str(
                        item.get(
                            "display_target",
                            target_from_value.get((metadata >> 24) & 0xFF, "unknown"),
                        )
                    ),
                    "draw_ordinal": int(item.get("draw_ordinal", metadata >> 32)),
                }
            )
    events.sort(key=lambda item: item["sequence"])
    if any(current["sequence"] <= previous["sequence"] for previous, current in zip(events, events[1:])):
        raise ValueError("draw_events sequences must be strictly increasing")
    return events


def enrich_command_list_intervals(
    intervals: list[dict[str, Any]], draw_events: list[dict[str, Any]]
) -> None:
    """Attach exact draw count and direct display-screen evidence to command-list intervals."""
    events_by_cmdlist: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for event in draw_events:
        events_by_cmdlist[int(event["cmdlist_execution_sequence"])].append(event)

    for item in intervals:
        if item.get("cmdlist_execution_sequence") is None:
            continue
        events = events_by_cmdlist.get(int(item["cmdlist_execution_sequence"]), [])
        if not events:
            continue
        if item.get("draws") is not None and int(item["draws"]) != len(events):
            raise ValueError(
                "command-list draw count mismatch for "
                f'{item["cmdlist_execution_sequence"]}: bundle={item["draws"]}, tuples={len(events)}'
            )
        item["draws"] = len(events)
        target_counts = Counter(str(event.get("display_target", "unknown")) for event in events)
        item["display_targets"] = ", ".join(
            f'{DISPLAY_TARGET_LABELS.get(target, target)}:{count}'
            for target, count in sorted(target_counts.items())
        )
        screens: set[str] = set()
        if target_counts["top_left"] or target_counts["top_right"]:
            screens.add("top")
        if target_counts["bottom"]:
            screens.add("bottom")
        service_screen = str(item.get("service_screen", "unknown"))
        if screens == {"top"}:
            item["screen"] = "top"
        elif screens == {"bottom"}:
            item["screen"] = "bottom"
        elif screens == {"top", "bottom"}:
            item["screen"] = "mixed"
        elif service_screen in {"top", "bottom", "mixed"}:
            # A command list may render only to an offscreen attachment while still
            # serving a known display output. Keep the attachment evidence above,
            # but do not erase an independently correlated service-screen role.
            item["screen"] = service_screen
        else:
            item["screen"] = "unknown"
        if screens:
            item["screen_proof"] = "direct_display_target"
        elif service_screen in {"top", "bottom", "mixed"}:
            item["screen_proof"] = "correlated_service_role"
        else:
            item["screen_proof"] = "unresolved"


def load_input(path: Path) -> tuple[dict[str, Any], bytes]:
    raw = path.read_bytes()
    data = json.loads(raw.decode("utf-8"))
    if data.get("schema_version") not in (1, 2):
        raise ValueError("schema_version must be 1 or 2")
    if not data.get("time_domain"):
        raise ValueError("time_domain is required")
    window = data.get("window", {})
    start = int(window.get("start_ns", 0))
    end = int(window.get("end_ns", 0))
    if end <= start:
        raise ValueError("window.end_ns must be greater than window.start_ns")

    diagnostics = list(data.get("diagnostics", []))
    materialize_ptracy_counter_queries(data, start, end)
    frame_markers = normalize_frame_markers(data, start, end)
    candidates = list(data.get("intervals", [])) + normalize_ptracy_slices(data)
    normalized: list[dict[str, Any]] = []
    zero_duration = 0
    outside_window = 0
    for index, item in enumerate(candidates):
        item = dict(item)
        scope_name = str(item.get("scope_name", "")).strip() or None
        scope_base, semantics = merge_scope_semantics(item, scope_name)
        item.update(semantics)
        if scope_name:
            item["scope_name"] = scope_name
            item["scope_base"] = scope_base
        stage = str(item.get("stage", "")).strip()
        lane = str(item.get("lane", "")).strip()
        if not stage or not lane:
            raise ValueError(f"interval {index} requires stage and lane")
        raw_start = int(item.get("start_ns", 0))
        raw_end = int(item.get("end_ns", 0))
        if raw_end <= raw_start:
            zero_duration += 1
            continue
        clipped_start = max(start, raw_start)
        clipped_end = min(end, raw_end)
        if clipped_end <= clipped_start:
            outside_window += 1
            continue
        normalized.append(
            {
                **item,
                "stage": stage,
                "lane": lane,
                "start_ns": clipped_start,
                "end_ns": clipped_end,
                "original_start_ns": item.get("original_start_ns", raw_start),
                "original_end_ns": item.get("original_end_ns", raw_end),
                "window_clipped": bool(item.get("window_clipped", False))
                or raw_start < start
                or raw_end > end,
            }
        )
    if zero_duration:
        diagnostics.append(
            diagnostic(
                "warning",
                "ZeroDurationIntervalDropped",
                f"Dropped {zero_duration} zero or negative duration intervals.",
                count=zero_duration,
            )
        )
    if outside_window:
        diagnostics.append(
            diagnostic(
                "info",
                "OutOfWindowIntervalDropped",
                f"Dropped {outside_window} intervals outside the selected window.",
                count=outside_window,
            )
        )
    if not normalized:
        raise ValueError("no positive-duration intervals overlap the selected window")

    data["schema_version"] = 2
    data["window"] = {"start_ns": start, "end_ns": end}
    data["frame_markers"] = frame_markers
    data["frame_workloads"] = normalize_frame_workloads(data, start, end)
    data["draw_events"] = normalize_draw_events(data, start, end)
    data["intervals"] = normalized
    data["markers"] = normalize_markers(data, start, end)
    data["frame_annotations"] = normalize_frame_annotations(data)
    enrich_command_list_intervals(data["intervals"], data["draw_events"])
    data["diagnostics"] = diagnostics
    return data, raw


def stage_options(data: dict[str, Any], stage: str) -> dict[str, Any]:
    options = data.get("stage_options", {}).get(stage, {})
    stage_items = [item for item in data["intervals"] if item["stage"] == stage]
    return {
        "order": int(options.get("order", 1000)),
        "role": str(options.get("role", "root")),
        "parent": options.get("parent"),
        "count_for_concurrency": bool(
            options.get(
                "count_for_concurrency",
                all(item.get("count_for_concurrency", True) for item in stage_items),
            )
        ),
        "include_in_overlap": bool(
            options.get(
                "include_in_overlap",
                all(item.get("include_in_overlap", True) for item in stage_items),
            )
        ),
        "raw_sum_label": str(options.get("raw_sum_label", "ms")),
        "source_role_layout": str(options.get("source_role_layout", "lane")),
        # "inline" draws the stage's intervals as thin phase bars inside its parent's row
        # instead of giving it a row of its own; statistics are unaffected.
        "layout": str(options.get("layout", "row")),
        # Detail stages are normally left out of the summary table; opt them in.
        "summary": bool(options.get("summary", options.get("role", "root") == "root")),
        # Stages at the tail of the pipeline display work produced earlier, so
        # the frame band they run in is not the frame they belong to.
        "consumes_gpu_output": bool(options.get("consumes_gpu_output", False)),
    }


def validate_token_contracts(
    data: dict[str, Any],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    results: list[dict[str, Any]] = []
    diagnostics: list[dict[str, Any]] = []
    counts: dict[tuple[str, str], int] = Counter()
    for item in data["intervals"]:
        if item.get("token") is not None:
            counts[(str(item["token"]), item["stage"])] += 1

    for contract_index, contract in enumerate(data.get("token_contracts", [])):
        name = str(contract.get("name", f"contract-{contract_index}"))
        stages = contract.get("stages", {})
        tokens = [str(token) for token in contract.get("tokens", [])]
        if not tokens:
            tokens = sorted({token for token, stage in counts if stage in stages})
        failures: list[dict[str, Any]] = []
        for token in tokens:
            for stage, cardinality in stages.items():
                minimum = int(cardinality.get("min", 1))
                maximum = int(cardinality.get("max", minimum))
                actual = counts.get((token, stage), 0)
                if actual < minimum or actual > maximum:
                    failures.append(
                        {
                            "token": token,
                            "stage": stage,
                            "expected_min": minimum,
                            "expected_max": maximum,
                            "actual": actual,
                        }
                    )
        status = "pass" if not failures else "fail"
        results.append(
            {"name": name, "status": status, "token_count": len(tokens), "failures": failures}
        )
        if failures:
            diagnostics.append(
                diagnostic(
                    "error",
                    "TokenContractFailure",
                    f"{name}: {len(failures)} token/stage cardinality failures.",
                    contract=name,
                    failure_count=len(failures),
                )
            )
    return results, diagnostics


def semantic_value(item: dict[str, Any], key: str) -> str:
    return str(item.get(key, "unscoped"))


def layout_source_role(item: dict[str, Any], options: dict[str, dict[str, Any]]) -> str:
    if options[item["stage"]]["source_role_layout"] == "color":
        return "mixed"
    return semantic_value(item, "source_role")


def summarize_frame_semantics(
    data: dict[str, Any], frame_intervals: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    per_frame: list[dict[str, Any]] = []
    presence: dict[tuple[str, str, str, str, str], set[str]] = defaultdict(set)
    frame_names = [str(frame["frame"]) for frame in frame_intervals]
    for frame in frame_intervals:
        groups: dict[tuple[str, str, str, str, str], list[Interval]] = defaultdict(list)
        occurrences: Counter[tuple[str, str, str, str, str]] = Counter()
        for item in data["intervals"]:
            start = max(frame["start_ns"], item["start_ns"])
            end = min(frame["end_ns"], item["end_ns"])
            if end <= start:
                continue
            key = (
                item["stage"],
                semantic_value(item, "eye"),
                semantic_value(item, "pass"),
                semantic_value(item, "source_role"),
                semantic_value(item, "output_role"),
            )
            groups[key].append((start, end))
            occurrences[key] += 1
        coverage: list[dict[str, Any]] = []
        frame_ns = frame["end_ns"] - frame["start_ns"]
        for key in sorted(groups):
            union_ns = duration(merge_intervals(groups[key]))
            stage, eye, pass_name, source_role, output_role = key
            coverage.append(
                {
                    "stage": stage,
                    "eye": eye,
                    "pass": pass_name,
                    "source_role": source_role,
                    "output_role": output_role,
                    "occurrences": occurrences[key],
                    "union_ms": ns_to_ms(union_ns),
                    "frame_coverage_percent": round(100.0 * union_ns / frame_ns, 3),
                }
            )
            presence[key].add(str(frame["frame"]))
        per_frame.append({**frame, "coverage": coverage})

    matrix: list[dict[str, Any]] = []
    for key in sorted(presence):
        present = presence[key]
        stage, eye, pass_name, source_role, output_role = key
        matrix.append(
            {
                "stage": stage,
                "eye": eye,
                "pass": pass_name,
                "source_role": source_role,
                "output_role": output_role,
                "frames_present": [frame for frame in frame_names if frame in present],
                "frames_missing": [frame for frame in frame_names if frame not in present],
            }
        )
    return per_frame, matrix


def validate_frame_correlations(
    data: dict[str, Any], frame_intervals: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    correlations = data.get("frame_correlations", [])
    if not correlations:
        return []
    diagnostics: list[dict[str, Any]] = []
    expected = {str(frame["frame"]) for frame in frame_intervals}
    seen: set[str] = set()
    allowed = {"exact_source_frame", "exact_pica_work", "neighborhood", "unavailable"}
    for index, item in enumerate(correlations):
        frame = str(item.get("source_frame", ""))
        status = str(item.get("status", ""))
        if not frame or frame not in expected:
            diagnostics.append(
                diagnostic(
                    "error",
                    "FrameCorrelationOutsideWindow",
                    f"frame_correlations[{index}] does not name a source frame in the window.",
                )
            )
        elif frame in seen:
            diagnostics.append(
                diagnostic(
                    "error",
                    "DuplicateFrameCorrelation",
                    f"Source frame {frame} has more than one correlation record.",
                )
            )
        seen.add(frame)
        if status not in allowed:
            diagnostics.append(
                diagnostic(
                    "error",
                    "InvalidFrameCorrelationStatus",
                    f"frame_correlations[{index}] has unsupported status {status!r}.",
                )
            )
        if status in {"neighborhood", "unavailable"} and not item.get("reason"):
            diagnostics.append(
                diagnostic(
                    "error",
                    "FrameCorrelationReasonMissing",
                    f"Source frame {frame} status {status} requires a reason.",
                )
            )
        if status in {"exact_source_frame", "exact_pica_work"} and not item.get(
            "shared_pica_keys"
        ):
            diagnostics.append(
                diagnostic(
                    "error",
                    "ExactFrameCorrelationKeyMissing",
                    f"Source frame {frame} exact status requires shared_pica_keys.",
                )
            )
    missing = sorted(expected - seen)
    if missing:
        diagnostics.append(
            diagnostic(
                "error",
                "FrameCorrelationCoverageMissing",
                f"Missing correlation records for source frames: {', '.join(missing)}.",
            )
        )
    return diagnostics


def summarize_guest_command_traces(
    data: dict[str, Any], draw_workload: list[dict[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Join compact .gcmdtrace.3ds manifests to source frames through PICA frame ids."""
    traces = data.get("guest_command_traces", [])
    if not traces:
        return {"captures": [], "frames": []}, []
    if not isinstance(traces, list):
        raise ValueError("guest_command_traces must be an array of manifest objects")

    diagnostics: list[dict[str, Any]] = []
    captures: list[dict[str, Any]] = []
    by_pica_frame: dict[str, list[tuple[dict[str, Any], dict[str, Any]]]] = defaultdict(list)
    for index, trace in enumerate(traces):
        if not isinstance(trace, dict):
            raise ValueError(f"guest_command_traces[{index}] must be an object")
        schema = str(trace.get("schema", ""))
        if schema != "xr3ds.guest-command-trace.manifest.v1":
            raise ValueError(
                f"guest_command_traces[{index}] has unsupported schema {schema!r}"
            )
        if trace.get("extension") != ".gcmdtrace.3ds":
            raise ValueError(
                f"guest_command_traces[{index}] must identify a .gcmdtrace.3ds source"
            )
        capture_id = trace.get("capture_id", "unknown")
        completeness = trace.get("completeness", {})
        dropped_records = int(completeness.get("dropped_records", 0))
        required_join_keys = {
            "frame_id",
            "pica_submission_id",
            "pica_segment_sequence",
            "command_word_offset",
            "action_id",
        }
        missing_join_keys = sorted(required_join_keys - set(trace.get("join_keys", [])))
        if missing_join_keys:
            diagnostics.append(
                diagnostic(
                    "error",
                    "GuestCommandTraceJoinKeyMissing",
                    f"gcmdtrace capture {capture_id} omits stable join key(s): "
                    f"{', '.join(missing_join_keys)}.",
                    capture_id=capture_id,
                    missing_join_keys=missing_join_keys,
                )
            )
        captures.append(
            {
                "capture_id": capture_id,
                "program_id": trace.get("program_id", "unknown"),
                "source": trace.get("source", "unknown"),
                "counts": trace.get("counts", {}),
                "capabilities": trace.get("capabilities", {}),
                "completeness": completeness,
                "join_keys": trace.get("join_keys", []),
            }
        )
        if dropped_records:
            diagnostics.append(
                diagnostic(
                    "warning",
                    "GuestCommandTraceDroppedRecords",
                    f"gcmdtrace capture {capture_id} dropped {dropped_records} records.",
                    capture_id=capture_id,
                    dropped_records=dropped_records,
                )
            )
        for frame in trace.get("frames", []):
            if not isinstance(frame, dict) or frame.get("frame_id") is None:
                raise ValueError(
                    f"guest_command_traces[{index}].frames requires object records with frame_id"
                )
            by_pica_frame[str(frame["frame_id"])].append((trace, frame))

    joined_frames: list[dict[str, Any]] = []
    for workload in draw_workload:
        pica_frames = [str(value) for value in workload.get("pica_frames", [])]
        matches = [match for frame in pica_frames for match in by_pica_frame.get(frame, [])]
        missing = [frame for frame in pica_frames if frame not in by_pica_frame]
        ambiguous = [
            frame for frame in pica_frames if len(by_pica_frame.get(frame, [])) > 1
        ]
        if missing:
            diagnostics.append(
                diagnostic(
                    "warning",
                    "GuestCommandTraceFrameMissing",
                    f"Source frame F{workload['frame']} has no gcmdtrace record for PICA frame(s) "
                    f"{', '.join(missing)}.",
                    frame=workload["frame"],
                    pica_frames=missing,
                )
            )
        if ambiguous:
            diagnostics.append(
                diagnostic(
                    "warning",
                    "GuestCommandTraceFrameAmbiguous",
                    f"Source frame F{workload['frame']} matches multiple gcmdtrace captures for "
                    f"PICA frame(s) {', '.join(ambiguous)}.",
                    frame=workload["frame"],
                    pica_frames=ambiguous,
                )
            )
        producer = Counter()
        owner_mapped = 0
        owner_physical = 0
        pipelines: set[str] = set()
        capture_ids: set[str] = set()
        for trace, frame in matches:
            capture_ids.add(str(trace.get("capture_id", "unknown")))
            producer.update(
                {
                    key: int(value)
                    for key, value in frame.get("producer_proof", {}).items()
                }
            )
            owners = frame.get("resource_owners", {})
            owner_mapped += int(owners.get("guest_mapped", 0))
            owner_physical += int(owners.get("physical_only", 0))
            pipelines.update(str(value) for value in frame.get("pipelines", []))
        status = "unavailable"
        if pica_frames and matches:
            status = "exact" if not missing and not ambiguous else "partial"
        joined_frames.append(
            {
                "source_frame": workload["frame"],
                "pica_frames": [int(value) if value.isdigit() else value for value in pica_frames],
                "capture_ids": sorted(capture_ids),
                "status": status,
                "submissions": sum(int(frame.get("submissions", 0)) for _, frame in matches),
                "segments": sum(int(frame.get("segments", 0)) for _, frame in matches),
                "draws": sum(int(frame.get("draws", 0)) for _, frame in matches),
                "resources": sum(int(frame.get("resources", 0)) for _, frame in matches),
                "guest_mapped_resources": owner_mapped,
                "physical_only_resources": owner_physical,
                "producer_proof": dict(producer),
                "pipeline_count": len(pipelines),
                "pipelines": sorted(pipelines),
                "dropped_records": sum(
                    int(frame.get("dropped_records", 0)) for _, frame in matches
                ),
                "mapping_changed_records": sum(
                    int(frame.get("mapping_changed_records", 0)) for _, frame in matches
                ),
            }
        )
    return {"captures": captures, "frames": joined_frames}, diagnostics


def summarize_draw_workload(
    data: dict[str, Any], frame_intervals: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Compare the exact StatusLayer workload counter with committed PICA draw events."""
    diagnostics: list[dict[str, Any]] = []
    workloads = {str(item["frame"]): item for item in data.get("frame_workloads", [])}
    events = data.get("draw_events", [])
    summaries: list[dict[str, Any]] = []
    for frame in frame_intervals:
        frame_name = str(frame["frame"])
        workload = workloads.get(frame_name)
        frame_events = [
            item
            for item in events
            if frame["start_ns"] <= item["time_ns"] < frame["end_ns"]
        ]
        roles = Counter(str(item["source_role"]) for item in frame_events)
        resolved = len(frame_events)
        expected = int(workload["draw_calls"]) if workload else None
        status = "unavailable"
        coverage = None
        if expected is not None:
            coverage = round(100.0 * resolved / expected, 3) if expected else (100.0 if resolved == 0 else 0.0)
            status = "exact" if resolved == expected else "mismatch"
            if resolved != expected:
                diagnostics.append(
                    diagnostic(
                        "error",
                        "DrawWorkloadMismatch",
                        f"Completed workload F{frame_name} reports {expected} draws but "
                        f"{resolved} committed PICA draw events fall inside its exact band.",
                        frame=frame["frame"],
                        expected_draws=expected,
                        resolved_draws=resolved,
                    )
                )
        elif frame_events:
            diagnostics.append(
                diagnostic(
                    "warning",
                    "FrameWorkloadMissing",
                    f"F{frame_name} has {resolved} draw events but no completed workload tuple.",
                    frame=frame["frame"],
                )
            )
        boundary_value = workload.get("boundary_source") if workload else None
        boundary_label = (
            BOUNDARY_SOURCE_LABELS.get(int(boundary_value), str(boundary_value))
            if boundary_value is not None
            else None
        )
        # A completed stereo output replays its second eye. A generation that carries draws but no
        # replayed eye is auxiliary work the Guest published on its own boundary, not a top output,
        # and drawing it as an equal frame band hides that difference.
        workload_class = "unknown"
        if resolved:
            workload_class = "output" if roles.get("replay", 0) else "auxiliary"
        elif expected == 0:
            workload_class = "empty"
        summaries.append(
            {
                "frame": frame["frame"],
                "expected_draws": expected,
                "resolved_draws": resolved,
                "coverage_percent": coverage,
                "status": status,
                "workload_class": workload_class,
                "boundary_source": boundary_label,
                "command_lists": workload.get("command_lists") if workload else None,
                "indexed_draw_calls": workload.get("indexed_draw_calls") if workload else None,
                "vertices": workload.get("vertices") if workload else None,
                "role_counts": {role: roles.get(role, 0) for role in SOURCE_ROLE_LABELS},
                "pica_frames": sorted({item["pica_frame_id"] for item in frame_events}),
                "cmdlist_execution_sequences": sorted(
                    {item["cmdlist_execution_sequence"] for item in frame_events}
                ),
            }
        )
    if summaries and any(item["resolved_draws"] for item in summaries):
        unreachable = [
            role
            for role in ("record", "auxiliary")
            if not any(item["role_counts"].get(role) for item in summaries)
        ]
        if unreachable:
            diagnostics.append(
                diagnostic(
                    "info",
                    "SourceRoleNotEmitted",
                    "The runtime draw-role enum only emits source_candidate and replay, so "
                    f"{', '.join(unreachable)} stay zero here. Read them as not emitted, not as "
                    "measured absence; the workload_class column carries the auxiliary split.",
                    roles=unreachable,
                )
            )
    boundary_counts = Counter(
        str(item["boundary_source"]) for item in summaries if item["boundary_source"]
    )
    if boundary_counts:
        diagnostics.append(
            diagnostic(
                "info",
                "WorkloadBoundarySource",
                "Generation closure provenance in this window: "
                + ", ".join(f"{name} x{count}" for name, count in sorted(boundary_counts.items())),
                boundary_sources=dict(boundary_counts),
            )
        )
    return summaries, diagnostics


def analyze(data: dict[str, Any]) -> dict[str, Any]:
    window = data["window"]
    window_ns = window["end_ns"] - window["start_ns"]
    by_stage: dict[str, list[Interval]] = defaultdict(list)
    occurrence_durations: dict[str, list[int]] = defaultdict(list)
    for item in data["intervals"]:
        interval = (item["start_ns"], item["end_ns"])
        by_stage[item["stage"]].append(interval)
        if not item.get("window_clipped", False):
            occurrence_durations[item["stage"]].append(interval[1] - interval[0])

    options = {stage: stage_options(data, stage) for stage in by_stage}
    unions = {stage: merge_intervals(intervals) for stage, intervals in by_stage.items()}
    stage_stats: dict[str, dict[str, Any]] = {}
    for stage in sorted(by_stage, key=lambda value: (options[value]["order"], value)):
        raw_ns = duration(by_stage[stage])
        union_ns = duration(unions[stage])
        values = occurrence_durations[stage]
        stage_stats[stage] = {
            "occurrences": len(by_stage[stage]),
            "complete_occurrences": len(values),
            "raw_sum_ms": ns_to_ms(raw_ns),
            "raw_sum_label": options[stage]["raw_sum_label"],
            "union_ms": ns_to_ms(union_ns),
            "window_coverage_percent": round(100.0 * union_ns / window_ns, 3),
            "p50_ms": ns_to_ms(percentile(values, 0.50)),
            "p90_ms": ns_to_ms(percentile(values, 0.90)),
            "p99_ms": ns_to_ms(percentile(values, 0.99)),
            **options[stage],
        }

    overlap_stages = [stage for stage in stage_stats if options[stage]["include_in_overlap"]]
    overlaps = []
    for left_index, left in enumerate(overlap_stages):
        left_ns = duration(unions[left])
        for right in overlap_stages[left_index + 1 :]:
            right_ns = duration(unions[right])
            overlap_ns = intersection_duration(unions[left], unions[right])
            overlaps.append(
                {
                    "left": left,
                    "right": right,
                    "overlap_ms": ns_to_ms(overlap_ns),
                    "of_left_percent": round(100.0 * overlap_ns / left_ns, 3) if left_ns else 0.0,
                    "of_right_percent": round(100.0 * overlap_ns / right_ns, 3) if right_ns else 0.0,
                    "of_shorter_percent": round(
                        100.0 * overlap_ns / min(left_ns, right_ns), 3
                    )
                    if min(left_ns, right_ns)
                    else 0.0,
                }
            )

    events: list[tuple[int, int, str]] = []
    for stage, intervals in unions.items():
        if not options[stage]["count_for_concurrency"]:
            continue
        for interval_start, interval_end in intervals:
            events.append((interval_start, 1, stage))
            events.append((interval_end, -1, stage))
    events.sort(key=lambda event: (event[0], event[1]))
    histogram_ns: dict[int, int] = defaultdict(int)
    active: set[str] = set()
    cursor = window["start_ns"]
    for timestamp, delta, stage in events:
        if timestamp > cursor:
            histogram_ns[len(active)] += timestamp - cursor
            cursor = timestamp
        if delta < 0:
            active.discard(stage)
        else:
            active.add(stage)
    if cursor < window["end_ns"]:
        histogram_ns[len(active)] += window["end_ns"] - cursor

    token_results, token_diagnostics = validate_token_contracts(data)
    diagnostics = list(data.get("diagnostics", [])) + token_diagnostics
    if data.get("gpu_cpu_alignment") == "uncalibrated":
        diagnostics.append(
            diagnostic(
                "warning",
                "GpuCpuAlignmentUncalibrated",
                "GPU hardware timestamps must not be intersected with CPU intervals; use CPU-submit-time for placement.",
            )
        )
    histogram = [
        {
            "active_stages": count,
            "duration_ms": ns_to_ms(value),
            "window_percent": round(100.0 * value / window_ns, 3),
        }
        for count, value in sorted(histogram_ns.items())
    ]
    active_overlap_ns = sum(value for count, value in histogram_ns.items() if count >= 2)
    frame_intervals = frame_intervals_from_markers(data["frame_markers"])
    frame_durations = [
        item["end_ns"] - item["start_ns"] for item in frame_intervals
    ]
    frame_semantics, semantic_presence = summarize_frame_semantics(data, frame_intervals)
    diagnostics.extend(validate_frame_correlations(data, frame_intervals))
    draw_workload, draw_diagnostics = summarize_draw_workload(data, frame_intervals)
    diagnostics.extend(draw_diagnostics)
    guest_commands, guest_command_diagnostics = summarize_guest_command_traces(
        data, draw_workload
    )
    diagnostics.extend(guest_command_diagnostics)
    return {
        "schema_version": 2,
        "tool_version": TOOL_VERSION,
        "title": data.get("title", "Stage concurrency"),
        "subtitle": data.get("subtitle", ""),
        "time_domain": data["time_domain"],
        "gpu_cpu_alignment": data.get("gpu_cpu_alignment", "unknown"),
        "axis_mode": data.get("axis_mode", "frame_and_time"),
        "identity": data.get("identity", {}),
        "metrics": data.get("metrics", []),
        "frame_correlations": data.get("frame_correlations", []),
        "guest_frame_chains": data.get("guest_frame_chains", []),
        "guest_command_trace": guest_commands,
        "guest_symbol_bindings": data.get("guest_symbol_bindings", []),
        "window": {**window, "duration_ms": ns_to_ms(window_ns)},
        "frames": {
            "count": len(frame_intervals),
            "first": frame_intervals[0]["frame"],
            "last": frame_intervals[-1]["frame"],
            "end_boundary": data["frame_markers"][-1]["frame"],
            "boundary": data["frame_markers"][0].get("boundary", "begin"),
            "p50_ms": ns_to_ms(percentile(frame_durations, 0.50)),
            "p90_ms": ns_to_ms(percentile(frame_durations, 0.90)),
            "p99_ms": ns_to_ms(percentile(frame_durations, 0.99)),
            "intervals": frame_intervals,
        },
        "frame_semantics": frame_semantics,
        "draw_workload": draw_workload,
        "semantic_presence": semantic_presence,
        "interval_count": len(data["intervals"]),
        "row_count": len(
            {
                (
                    item["stage"],
                    item["lane"],
                    semantic_value(item, "eye"),
                    semantic_value(item, "pass"),
                    layout_source_role(item, options),
                    semantic_value(item, "output_role"),
                )
                for item in data["intervals"]
                if options[item["stage"]]["layout"] != "inline"
            }
        ),
        "markers": data.get("markers", []),
        "frame_annotations": [
            data["frame_annotations"][str(frame["frame"])]
            for frame in frame_intervals
            if str(frame["frame"]) in data.get("frame_annotations", {})
        ],
        "stage_stats": stage_stats,
        "pairwise_overlaps": overlaps,
        "concurrency_histogram": histogram,
        "concurrent_2plus_ms": ns_to_ms(active_overlap_ns),
        "concurrent_2plus_percent": round(100.0 * active_overlap_ns / window_ns, 3),
        "max_concurrent_stages": max(histogram_ns, default=0),
        "token_contracts": token_results,
        "diagnostics": diagnostics,
    }


def svg_text(x: float, y: float, value: Any, css_class: str, anchor: str = "start") -> str:
    return (
        f'<text x="{x:.2f}" y="{y:.2f}" class="{css_class}" text-anchor="{anchor}">'
        f"{html.escape(str(value))}</text>"
    )


def svg_attr(value: Any) -> str:
    return html.escape(str(value), quote=True)


def command_list_visible_label(item: dict[str, Any], width: float) -> str:
    if item.get("cmdlist_execution_sequence") is None or item.get("draws") is None:
        return ""
    draws = int(item["draws"])
    screen = str(item.get("screen", "unknown"))
    screen_label = SCREEN_LABELS.get(screen, screen.title())
    candidates = (
        f"{draws:,} DC · {screen_label}",
        f"{draws:,}{screen_label[:1]}",
        f"{draws:,}",
    )
    for label in candidates:
        if width >= len(label) * 4.7 + 8.0:
            return label
    return ""


def require_frame_count(report: dict[str, Any], expected: int) -> None:
    actual = int(report["frames"]["count"])
    if actual != expected:
        raise ValueError(
            f"review SVG requires exactly {expected} complete source frames; got {actual}"
        )


def select_frame_window(
    data: dict[str, Any], frame_count: int, start_frame: str | None = None
) -> dict[str, Any]:
    if frame_count <= 0:
        raise ValueError("selected frame count must be positive")
    markers = data["frame_markers"]
    measured_frames = frame_intervals_from_markers(markers)
    available = len(measured_frames)
    if available < frame_count:
        raise ValueError(
            f"cannot select {frame_count} complete source frames from {available} available"
        )
    start_index = 0
    if start_frame is not None:
        matches = [
            index
            for index, frame in enumerate(measured_frames)
            if str(frame["frame"]) == start_frame
        ]
        if not matches:
            raise ValueError(f"selected start frame {start_frame!r} is not a complete source frame")
        start_index = matches[0]
    if start_index + frame_count >= len(markers):
        raise ValueError(
            f"selected start frame {measured_frames[start_index]['frame']} does not have "
            f"{frame_count} complete source frames available"
        )

    selected_markers = markers[start_index : start_index + frame_count + 1]
    start = int(selected_markers[0]["time_ns"])
    end = int(selected_markers[-1]["time_ns"])
    selected_intervals: list[dict[str, Any]] = []
    for item in data["intervals"]:
        clipped_start = max(start, int(item["start_ns"]))
        clipped_end = min(end, int(item["end_ns"]))
        if clipped_end <= clipped_start:
            continue
        selected_intervals.append(
            {
                **item,
                "start_ns": clipped_start,
                "end_ns": clipped_end,
                "window_clipped": bool(item.get("window_clipped", False))
                or int(item["start_ns"]) < start
                or int(item["end_ns"]) > end,
            }
        )
    if not selected_intervals:
        raise ValueError("no positive-duration intervals overlap the selected frame window")

    selected_measured_frames = frame_intervals_from_markers(selected_markers)
    selected_frames = {str(frame["frame"]) for frame in selected_measured_frames}
    data["window"] = {"start_ns": start, "end_ns": end}
    data["frame_markers"] = selected_markers
    data["intervals"] = selected_intervals
    data["frame_workloads"] = [
        item
        for item in data.get("frame_workloads", [])
        if str(item.get("frame")) in selected_frames
    ]
    data["draw_events"] = [
        item
        for item in data.get("draw_events", [])
        if start <= int(item.get("time_ns", -1)) < end
    ]
    data["frame_correlations"] = [
        item
        for item in data.get("frame_correlations", [])
        if str(item.get("source_frame")) in selected_frames
    ]
    data["markers"] = [
        item for item in data.get("markers", []) if start <= int(item["time_ns"]) <= end
    ]
    data["frame_selection"] = {
        "count": frame_count,
        "start_frame": selected_measured_frames[0]["frame"],
        "end_boundary_frame": selected_markers[-1]["frame"],
    }
    return data


def markdown_cell(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def render_review_markdown(
    data: dict[str, Any],
    report: dict[str, Any],
    input_path: Path,
    svg_path: Path,
    report_path: Path | None,
) -> str:
    lines = [
        f'# {report["title"]}',
        "",
    ]
    if report.get("subtitle"):
        lines.extend([str(report["subtitle"]), ""])
    lines.extend(
        [
            "## Evidence",
            "",
            f'- Source bundle: `{input_path}`',
            f'- Interactive SVG: `{svg_path}`',
            f'- Analysis JSON: `{report_path}`' if report_path else "- Analysis JSON: not emitted",
            f'- Time domain: `{report["time_domain"]}`',
            f'- GPU/CPU alignment: `{report["gpu_cpu_alignment"]}`',
            f'- Window: {report["frames"]["count"]} complete source frames, '
            f'{report["window"]["duration_ms"]:.3f} ms wall time, '
            f'F{report["frames"]["first"]}–F{report["frames"]["last"]} '
            f'({report["frames"]["boundary"]}-boundary markers)',
            "",
        ]
    )
    if report.get("identity"):
        lines.extend(["### Binary and capture identity", ""])
        for key, value in report["identity"].items():
            lines.append(f'- {key}: `{value}`')
        lines.append("")
    lines.extend(
        [
            "## Headline",
            "",
            f'- Intervals / lanes: {report["interval_count"]} / {report["row_count"]}',
            f'- Two-or-more root-stage overlap: {report["concurrent_2plus_ms"]:.3f} ms '
            f'({report["concurrent_2plus_percent"]:.1f}%)',
            f'- Maximum distinct root-stage concurrency: {report["max_concurrent_stages"]}',
            f'- Source-frame p50 / p90 / p99: {report["frames"]["p50_ms"]:.3f} / '
            f'{report["frames"]["p90_ms"]:.3f} / {report["frames"]["p99_ms"]:.3f} ms',
        ]
    )
    for metric in report.get("metrics", []):
        lines.append(
            f'- {metric.get("label", "Metric")}: {metric.get("value", "—")}'
            f'{metric.get("unit", "")}'
        )
    if report.get("frame_annotations"):
        keys: list[str] = []
        for item in report["frame_annotations"]:
            for key in item.get("metrics", {}):
                if key not in keys:
                    keys.append(key)
        lines.extend(
            [
                "",
                "## Frame annotations",
                "",
                "Per-frame classification supplied by the bundle builder from the same trace "
                "(ticks, completion latency, GPU busy); the class tints the frame band in the SVG.",
                "",
                "| Frame | Class | Badges | " + " | ".join(keys) + " | Note |",
                "| --- | --- | --- | " + " | ".join("---:" for _ in keys) + " | --- |",
            ]
        )
        for item in report["frame_annotations"]:
            values = " | ".join(markdown_cell(item["metrics"].get(key, "—")) for key in keys)
            lines.append(
                f'| {markdown_cell(item["frame"])} | {item.get("class") or "—"} | '
                f'{markdown_cell(" · ".join(item.get("badges", [])))} | {values} | '
                f'{markdown_cell(item.get("note", ""))} |'
            )
    # Azahar's draw-workload model has no GameNative producer; print it only when a bundle has data.
    draw_workload = [item for item in report["draw_workload"]
                     if item["expected_draws"] is not None or item["resolved_draws"]]
    if draw_workload:
        lines.extend(
            [
                "",
                "## Draw workload coverage",
                "",
                "The expected count is the exact completed Guest top-output workload used by StatusLayer. "
                "Resolved draws are committed PICA draw events inside that same half-open band.",
                "",
                "| Frame | Class | Closed by | Expected | Resolved | Coverage | Record | Candidate | Replay | Aux | Unknown | PICA frames |",
                "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
            ]
        )
    for item in draw_workload:
        expected = "—" if item["expected_draws"] is None else item["expected_draws"]
        coverage = "—" if item["coverage_percent"] is None else f'{item["coverage_percent"]:.1f}%'
        roles = item["role_counts"]
        workload_class = item.get("workload_class") or "unknown"
        closed_by = item.get("boundary_source") or "—"
        lines.append(
            f'| {markdown_cell(item["frame"])} | {workload_class} | {closed_by} | '
            f'{expected} | {item["resolved_draws"]} | '
            f'{coverage} | {roles["record"]} | {roles["source_candidate"]} | '
            f'{roles["replay"]} | {roles["auxiliary"]} | {roles["unknown"]} | '
            f'{markdown_cell(item["pica_frames"])} |'
        )
    lines.extend(
        [
            "",
            "## Semantic coverage by frame",
            "",
            "These rows describe which paths appeared in the stable consecutive-frame window. "
            "Their durations come from the existing profiler spans; they are not a frame-ranking or "
            "new timing model.",
            "",
            "| Frame | Stage | Eye | Source role | Output role | Pass | Count | Union ms |",
            "| --- | --- | --- | --- | --- | --- | ---: | ---: |",
        ]
    )
    for frame in report["frame_semantics"]:
        for coverage in frame["coverage"]:
            lines.append(
                f'| {markdown_cell(frame["frame"])} | {markdown_cell(coverage["stage"])} | '
                f'{markdown_cell(coverage["eye"])} | {markdown_cell(coverage["source_role"])} | '
                f'{markdown_cell(coverage["output_role"])} | {markdown_cell(coverage["pass"])} | '
                f'{coverage["occurrences"]} | {coverage["union_ms"]:.3f} |'
            )
    if not any(frame["coverage"] for frame in report["frame_semantics"]):
        lines.append("| — | — | unscoped | unscoped | unscoped | unscoped | 0 | 0 |")

    lines.extend(["", "### Missing semantic coverage", ""])
    missing = [item for item in report["semantic_presence"] if item["frames_missing"]]
    if missing:
        for item in missing:
            lines.append(
                f'- `{item["stage"]}` eye=`{item["eye"]}` pass=`{item["pass"]}` missing in '
                f'{", ".join("F" + frame for frame in item["frames_missing"])}'
            )
    else:
        lines.append("- Every observed semantic path is present in every captured frame.")

    lines.extend(["", "## Guest command trace correlation", ""])
    guest_command_frames = report["guest_command_trace"]["frames"]
    if guest_command_frames:
        lines.extend(
            [
                "| Source | PICA frames | State | Submit | Segments | Draws | Resources | Guest-mapped / physical-only | Producer proof | Pipelines | Drops |",
                "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: |",
            ]
        )
        for frame in guest_command_frames:
            producer = frame["producer_proof"]
            proof = (
                f'watch={producer.get("debugger_watchpoint", 0)} '
                f'prof={producer.get("guest_profiler_boundary", 0)} '
                f'context={producer.get("submission_context_only", 0)} '
                f'unknown={producer.get("unknown", 0)}'
            )
            lines.append(
                f'| F{markdown_cell(frame["source_frame"])} | '
                f'{markdown_cell(frame["pica_frames"])} | {frame["status"]} | '
                f'{frame["submissions"]} | {frame["segments"]} | {frame["draws"]} | '
                f'{frame["resources"]} | {frame["guest_mapped_resources"]} / '
                f'{frame["physical_only_resources"]} | {proof} | '
                f'{frame["pipeline_count"]} | {frame["dropped_records"]} |'
            )
        lines.extend(
            [
                "",
                "`submission_context_only` identifies the thread/PC/LR at submission; it is not "
                "proof of the exact CPU writer. Exact producer attribution still requires a Guest "
                "watchpoint or a calibrated profiler boundary. Resource bytes are PICA-consumption "
                "snapshots, not GPU-ordered probes.",
            ]
        )
    else:
        lines.append("- Not supplied. Draws cannot be joined to Guest owners or producer proof.")

    lines.extend(["", "## Guest Frame Debugger correlation", ""])
    if report["guest_frame_chains"]:
        for chain in report["guest_frame_chains"]:
            if isinstance(chain, dict):
                lines.append(
                    f'- chain=`{chain.get("frame_chain_id", "unknown")}` '
                    f'outputs=`{chain.get("output_start", chain.get("output_id", "unknown"))}..'
                    f'{chain.get("output_end", chain.get("output_id", "unknown"))}` '
                    f'pica_frames=`{chain.get("pica_frame_start", chain.get("pica_frame_id", "unknown"))}..'
                    f'{chain.get("pica_frame_end", chain.get("pica_frame_id", "unknown"))}` '
                    f'state=`{chain.get("state", "unknown")}` '
                    f'path=`{chain.get("path", "not-recorded")}`'
                )
            else:
                lines.append(f'- `{chain}`')
    else:
        lines.append("- Not supplied. Guest call semantics remain unavailable for this window.")

    lines.extend(["", "## Guest symbol bindings", ""])
    if report["guest_symbol_bindings"]:
        lines.extend(
            [
                "| Section | Guest anchor | Symbol | Confidence | SDK relation |",
                "| --- | --- | --- | --- | --- |",
            ]
        )
        for binding in report["guest_symbol_bindings"]:
            lines.append(
                f'| {markdown_cell(binding.get("section_name", binding.get("id", "—")))} | '
                f'{markdown_cell(binding.get("anchor_guest_va", "unresolved"))} | '
                f'{markdown_cell(binding.get("symbol", "unresolved"))} | '
                f'{markdown_cell(binding.get("confidence", "unresolved"))} | '
                f'{markdown_cell(binding.get("sdk_relation", "unresolved"))} |'
            )
    else:
        lines.append("- No identity-gated Guest symbol binding table was supplied.")

    lines.extend(["", "## Per-frame source correlation", ""])
    if report["frame_correlations"]:
        lines.extend(
            [
                "| Source frame | Status | Shared PICA keys | Reason |",
                "| --- | --- | --- | --- |",
            ]
        )
        for correlation in report["frame_correlations"]:
            lines.append(
                f'| {markdown_cell(correlation.get("source_frame", "—"))} | '
                f'{markdown_cell(correlation.get("status", "unavailable"))} | '
                f'{markdown_cell(correlation.get("shared_pica_keys", []))} | '
                f'{markdown_cell(correlation.get("reason", ""))} |'
            )
    else:
        lines.append("- No profiler/PICA/RenderDoc frame correlation records were supplied.")
    lines.extend(
        [
            "",
            "## Stage durations",
            "",
            "| Stage | Role | Count | Union ms | Coverage | p50 ms | p90 ms | p99 ms |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for stage, stats in report["stage_stats"].items():
        lines.append(
            f'| {markdown_cell(stage)} | {markdown_cell(stats["role"])} | '
            f'{stats["occurrences"]} | {stats["union_ms"]:.3f} | '
            f'{stats["window_coverage_percent"]:.1f}% | {stats["p50_ms"]:.3f} | '
            f'{stats["p90_ms"]:.3f} | {stats["p99_ms"]:.3f} |'
        )

    overlaps = sorted(
        report["pairwise_overlaps"], key=lambda item: item["overlap_ms"], reverse=True
    )
    lines.extend(
        [
            "",
            "## Pairwise overlaps",
            "",
            "| Left | Right | Overlap ms | of left | of right | of shorter |",
            "| --- | --- | ---: | ---: | ---: | ---: |",
        ]
    )
    if overlaps:
        for item in overlaps:
            lines.append(
                f'| {markdown_cell(item["left"])} | {markdown_cell(item["right"])} | '
                f'{item["overlap_ms"]:.3f} | {item["of_left_percent"]:.1f}% | '
                f'{item["of_right_percent"]:.1f}% | {item["of_shorter_percent"]:.1f}% |'
            )
    else:
        lines.append("| — | — | 0 | 0% | 0% | 0% |")

    lines.extend(["", "## Token contracts", ""])
    if report["token_contracts"]:
        for contract in report["token_contracts"]:
            lines.append(
                f'- **{contract["status"].upper()}** {contract["name"]}: '
                f'{contract["token_count"]} tokens, {len(contract["failures"])} failures'
            )
    else:
        lines.append("- No token contracts were supplied.")

    lines.extend(["", "## Diagnostics", ""])
    if report["diagnostics"]:
        for item in report["diagnostics"]:
            lines.append(
                f'- **{str(item.get("severity", "unknown")).upper()} '
                f'{item.get("code", "Diagnostic")}**: {item.get("message", "")}'
            )
    else:
        lines.append("- No diagnostics.")
    lines.extend(
        [
            "",
            "## Review note",
            "",
            "The SVG is self-contained and interactive when opened directly in a browser: select an "
            "interval to pin its stage, lane, source-frame token, duration, and source. Codex and "
            "document previews retain the static fallback but may block scripts. A Feishu "
            "document should use a rendered PNG preview plus these tables; attach the original SVG "
            "for full-detail inspection because document sanitization may disable SVG scripts.",
            "",
            f'_Generated by gamenative-stage-concurrency-analysis v{TOOL_VERSION}._',
            "",
        ]
    )
    return "\n".join(lines)


def render_svg(data: dict[str, Any], report: dict[str, Any]) -> str:
    window = data["window"]
    start = window["start_ns"]
    span = window["end_ns"] - start
    options = {
        stage: stage_options(data, stage) for stage in {item["stage"] for item in data["intervals"]}
    }
    inline_stages = {stage for stage in options if options[stage]["layout"] == "inline"}
    inline_children: dict[str, list[str]] = defaultdict(list)
    for stage in sorted(inline_stages, key=lambda value: (options[value]["order"], value)):
        parent = options[stage].get("parent")
        if parent is None or parent not in options or parent in inline_stages:
            raise ValueError(
                f"inline stage {stage!r} needs a row-layout parent stage; got {parent!r}"
            )
        inline_children[parent].append(stage)
    rows = sorted(
        {
            (
                item["stage"],
                item["lane"],
                semantic_value(item, "eye"),
                semantic_value(item, "pass"),
                layout_source_role(item, options),
                semantic_value(item, "output_role"),
            )
            for item in data["intervals"]
            if item["stage"] not in inline_stages
        },
        key=lambda row: (options[row[0]]["order"], *row),
    )
    markers = data.get("markers", [])
    annotations = data.get("frame_annotations", {})
    # Keep stage and lane identities readable without stealing horizontal resolution
    # from the frame timeline. They are rendered on separate lines below.
    left, right, width = 420, 30, 1840
    plot_width = width - left - right
    header_height = 190
    warning_height = 42 if report["diagnostics"] else 0
    chart_top = header_height + warning_height
    frame_header_height = 54
    rows_top = chart_top + frame_header_height
    row_height = 38
    chart_bottom = rows_top + row_height * len(rows)
    show_time_axis = data.get("axis_mode", "frame_and_time") == "frame_and_time"
    selection_top = chart_bottom + (78 if show_time_axis else 56)
    summary_top = selection_top + 70
    root_stats = [
        (stage, stats)
        for stage, stats in report["stage_stats"].items()
        if stats.get("summary", stats["role"] == "root")
    ][:14]
    legend_stages = [stage for stage in options if stage in inline_stages]
    summary_height = 64 + 25 * max(len(root_stats), 4) + (26 if legend_stages else 0)
    height = summary_top + summary_height + 32
    palette = [
        "#4f8cff",
        "#39c6a3",
        "#ffb84d",
        "#a78bfa",
        "#ff6b7a",
        "#22d3ee",
        "#a3e635",
        "#fb923c",
        "#f472b6",
    ]
    ordered_stages = sorted(options, key=lambda value: (options[value]["order"], value))
    stage_colors = {stage: palette[index % len(palette)] for index, stage in enumerate(ordered_stages)}
    by_row: dict[tuple[str, str, str, str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for item in data["intervals"]:
        by_row[
            (
                item["stage"],
                item["lane"],
                semantic_value(item, "eye"),
                semantic_value(item, "pass"),
                layout_source_role(item, options),
                semantic_value(item, "output_role"),
            )
        ].append(item)

    def enclosing_frame(timestamp: int) -> Any:
        for frame in report["frames"]["intervals"]:
            if frame["start_ns"] <= timestamp < frame["end_ns"]:
                return frame["frame"]
        return "—"

    def consumes_gpu_output(stage: str) -> bool:
        return bool(options.get(stage, {}).get("consumes_gpu_output", False))

    def source_frame_for(item: dict[str, Any]) -> tuple[Any, Any]:
        """Return the band this work runs in, plus the generation it consumed.

        Producers are attributed by where they run. For consumers at the tail of
        the pipeline the band is not the answer: a present cannot be delivering
        a frame whose drawing has not finished yet, so it is credited to the
        newest GPU generation that had actually completed by the time it began.
        The generation is reported as-is rather than folded back into a frame
        number, because a generation can span a frame boundary and picking one
        side would invent precision the trace does not have.
        """
        band = enclosing_frame(item["start_ns"])
        if not consumes_gpu_output(str(item.get("stage", ""))):
            if item.get("source_frame") is not None:
                return item["source_frame"], None
            if item.get("pica_frame_id") is not None:
                return f'PICA:{item["pica_frame_id"]}', None
            return band, None
        finished = [
            other
            for other in data["intervals"]
            if other.get("pica_frame_id") is not None
            and other["end_ns"] <= item["start_ns"]
        ]
        if not finished:
            return band, None
        newest = max(finished, key=lambda other: other["end_ns"])
        return band, newest["pica_frame_id"]

    identity = list(report.get("identity", {}).items())[:4]
    metrics = list(report.get("metrics", []))[:4]
    cards: list[tuple[str, str]] = [
        (
            "Frames / wall",
            f'{report["frames"]["count"]} / {report["window"]["duration_ms"]:.1f} ms',
        ),
        ("Intervals / lanes", f'{report["interval_count"]} / {report["row_count"]}'),
        ("2+ stage overlap", f'{report["concurrent_2plus_percent"]:.1f}%'),
        ("Max concurrency", str(report["max_concurrent_stages"])),
    ]
    for metric in metrics:
        cards.append(
            (
                str(metric.get("label", "Metric")),
                f'{metric.get("value", "—")}{metric.get("unit", "")}',
            )
        )
    cards = cards[:8]

    output = [
        f'<svg id="gamenative-stage-diagram" xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
        "<style>",
        "text{font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}",
        ".title{fill:#f8fafc;font-size:24px;font-weight:700}.subtitle{fill:#94a3b8;font-size:12px}",
        ".card-label{fill:#94a3b8;font-size:11px}.card-value{fill:#f8fafc;font-size:18px;font-weight:650}",
        ".lane-stage{fill:#dbe4f0;font-size:11.5px;font-weight:600}",
        ".lane-thread{fill:#8290a6;font-size:10.5px;font-family:ui-monospace,monospace}",
        ".axis{fill:#8290a6;font-size:10px;font-family:ui-monospace,monospace}",
        ".frame-relative{fill:#f8fafc;font-size:12px;font-weight:700;font-family:ui-monospace,monospace}",
        ".frame-workload{fill:#9fb1c9;font-size:10px;font-family:ui-monospace,monospace}",
        ".frame-class{fill:#7f8ea6;font-size:9px;font-family:ui-monospace,monospace}",
        ".frame-class-aux{fill:#d8b4fe;font-size:9px;font-weight:600;"
        "font-family:ui-monospace,monospace}",
        ".summary-head{fill:#94a3b8;font-size:10px;font-weight:600}.summary{fill:#dbe4f0;font-size:11px}",
        ".warning{fill:#ffd38a;font-size:11px}.meta{fill:#8290a6;font-size:10px;font-family:ui-monospace,monospace}",
        ".selection-hint{fill:#8290a6;font-size:10px}.selection-detail{fill:#e2e8f0;font-size:11px;font-family:ui-monospace,monospace}",
        ".interval-label{fill:#f8fafc;font-size:8.5px;font-weight:650;font-family:ui-monospace,monospace;pointer-events:none}",
        ".hover-title{fill:#e9f0fb;font-size:12px;font-weight:600}"
        ".hover-line{fill:#a8bad4;font-size:11px;font-family:ui-monospace,monospace}"
        ".hover-metric{fill:#7ee0c0;font-size:11px;font-family:ui-monospace,monospace}",
        ".interval{cursor:pointer;transition:opacity .12s ease,stroke-width .12s ease}",
        ".interval:hover,.interval:focus,.interval.selected{opacity:1;stroke:#f8fafc;stroke-width:2;outline:none}",
        "</style>",
        '<rect width="100%" height="100%" fill="#08111f"/>',
        f'<rect x="14" y="14" width="{width - 28}" height="154" rx="12" fill="#0e1a2d" stroke="#1d2b43"/>',
        f'<title id="title">{html.escape(str(report["title"]))}</title>',
        f'<desc id="desc">GameNative XR stage concurrency timeline over {report["window"]["duration_ms"]:.3f} milliseconds.</desc>',
        svg_text(32, 48, clamp_text(report["title"], 96), "title"),
        svg_text(32, 69, clamp_text(report.get("subtitle", ""), 160), "subtitle"),
        svg_text(width - 32, 46, report["time_domain"], "meta", "end"),
        svg_text(width - 32, 64, f'GPU/CPU: {report["gpu_cpu_alignment"]}', "meta", "end"),
    ]
    card_width, card_gap, card_y = 190, 10, 88
    for index, (label, value) in enumerate(cards):
        x = 32 + index * (card_width + card_gap)
        output.append(
            f'<rect x="{x}" y="{card_y}" width="{card_width}" height="62" rx="8" fill="#13223a"/>'
        )
        output.append(svg_text(x + 12, card_y + 21, clamp_text(label, 25), "card-label"))
        output.append(svg_text(x + 12, card_y + 47, clamp_text(value, 22), "card-value"))
    for index, (key, value) in enumerate(identity):
        output.append(svg_text(32 + index * 400, 180, f"{key}={clamp_text(value, 45)}", "meta"))

    if report["diagnostics"]:
        errors = sum(item.get("severity") == "error" for item in report["diagnostics"])
        warnings = sum(item.get("severity") == "warning" for item in report["diagnostics"])
        first_message = clamp_text(report["diagnostics"][0].get("message", ""), 160)
        output.append(
            f'<rect x="14" y="{header_height - 4}" width="{width - 28}" height="34" rx="7" '
            'fill="#34250f" stroke="#6b4d16"/>'
        )
        output.append(
            svg_text(
                30,
                header_height + 18,
                f"Diagnostics: {errors} error / {warnings} warning · {first_message}",
                "warning",
            )
        )

    frame_marker_list = data["frame_markers"]
    frames = report["frames"]["intervals"]
    frame_count = len(frames)
    relative_frame_labels = ["N" if index == 0 else f"N+{index}" for index in range(frame_count)]
    average_frame_width = plot_width / max(1, frame_count)
    label_every = max(1, math.ceil(68.0 / average_frame_width))
    workload_by_frame = {str(item["frame"]): item for item in report.get("draw_workload", [])}
    guest_commands_by_frame = {
        str(item["source_frame"]): item
        for item in report.get("guest_command_trace", {}).get("frames", [])
    }
    for index, frame in enumerate(frames):
        x = left + (frame["start_ns"] - start) * plot_width / span
        next_x = left + (frame["end_ns"] - start) * plot_width / span
        frame_width = next_x - x
        frame_duration_ms = frame["duration_ms"]
        draw_summary = workload_by_frame.get(str(frame["frame"]), {})
        expected_draws = draw_summary.get("expected_draws")
        resolved_draws = draw_summary.get("resolved_draws", 0)
        roles = draw_summary.get("role_counts", {})
        workload_class = str(draw_summary.get("workload_class") or "unknown")
        boundary_source = draw_summary.get("boundary_source")
        expected_text = "?" if expected_draws is None else f"{expected_draws:,}"
        dc_text = f"DC {resolved_draws:,}/{expected_text}"
        role_text = (
            f'R{roles.get("record", 0)} C{roles.get("source_candidate", 0)} '
            f'P{roles.get("replay", 0)} A{roles.get("auxiliary", 0)} U{roles.get("unknown", 0)}'
        )
        guest_summary = guest_commands_by_frame.get(str(frame["frame"]), {})
        guest_text = (
            f'GC S{guest_summary.get("submissions", 0)} '
            f'D{guest_summary.get("draws", 0)} R{guest_summary.get("resources", 0)}'
            if guest_summary.get("status") in {"exact", "partial"}
            else role_text
        )
        annotation = annotations.get(str(frame["frame"]), {})
        class_fills = FRAME_CLASS_FILLS.get(annotation.get("class", ""))
        if class_fills:
            fill = class_fills[index % 2]
        else:
            fill = "#0d192a" if index % 2 else "#0a1525"
        output.append(
            f'<g class="frame-band" data-relative-frame="{relative_frame_labels[index]}" '
            f'data-source-frame="{svg_attr(frame["frame"])}" data-draws="{resolved_draws}" '
            f'data-gcmdtrace-status="{svg_attr(guest_summary.get("status", "unavailable"))}" '
            f'data-gcmdtrace-submissions="{guest_summary.get("submissions", 0)}" '
            f'data-gcmdtrace-resources="{guest_summary.get("resources", 0)}">'
            f'<rect x="{x:.2f}" y="{chart_top}" width="{frame_width:.2f}" '
            f'height="{chart_bottom - chart_top + 3}" fill="{fill}" stroke="#24334a" '
            f'stroke-width="1"><title>{relative_frame_labels[index]} · '
            f'F{html.escape(str(frame["frame"]))}: {frame_duration_ms:.3f} ms · '
            f'{html.escape(dc_text)} · {html.escape(role_text)} · '
            f'{html.escape(guest_text)}</title></rect>'
        )
        frame_label_width = max(54.0, min(88.0, frame_width - 16.0))
        frame_label_x = x + (frame_width - frame_label_width) / 2.0
        output.append(
            f'<rect x="{frame_label_x:.2f}" y="{chart_top + 5}" '
            f'width="{frame_label_width:.2f}" height="22" rx="6" '
            f'fill="#162944" stroke="#355173"/>'
        )
        output.append(
            svg_text(
                x + frame_width / 2.0,
                chart_top + 39,
                f'F{frame["frame"]} · {dc_text}',
                "frame-workload",
                "middle",
            )
        )
        output.append(
            svg_text(
                x + frame_width / 2.0,
                chart_top + 51,
                guest_text,
                "frame-workload",
                "middle",
            )
        )
        header_class_text = workload_class.upper() if workload_class != "output" else ""
        if boundary_source:
            header_class_text = (
                f"{header_class_text} · {boundary_source}" if header_class_text
                else boundary_source
            )
        if annotation.get("badges"):
            badge_text = " · ".join(annotation["badges"])
            if annotation.get("class"):
                badge_text = f'{annotation["class"].upper()} · {badge_text}'
            header_class_text = badge_text
            workload_class = "auxiliary" if annotation.get("class") == "slip" else workload_class
        if header_class_text:
            # 9 px monospace ≈ 5.6 px per glyph; keep the badge line inside its own band.
            header_class_text = clamp_text(header_class_text, max(6, int(frame_width / 5.6)))
            output.append(
                svg_text(
                    x + frame_width / 2.0,
                    chart_top + 63,
                    header_class_text,
                    "frame-class-aux" if workload_class == "auxiliary" else "frame-class",
                    "middle",
                )
            )
        output.append(
            svg_text(
                x + frame_width / 2.0,
                chart_top + 20,
                relative_frame_labels[index],
                "frame-relative",
                "middle",
            )
        )
        output.append("</g>")
    for index, marker in enumerate(frame_marker_list):
        x = left + (marker["time_ns"] - start) * plot_width / span
        output.append(
            f'<line x1="{x:.2f}" y1="{chart_top}" x2="{x:.2f}" y2="{chart_bottom + 3}" '
            'stroke="#24334a" stroke-width="1"/>'
        )
    # Chart-wide instant markers (no stage): thin dashed lines, e.g. Guest VBlank ticks.
    for marker in markers:
        if marker.get("stage"):
            continue
        x = left + (marker["time_ns"] - start) * plot_width / span
        color = MARKER_COLORS.get(marker["kind"], "#6b7f9a")
        output.append(
            f'<line x1="{x:.2f}" y1="{rows_top}" x2="{x:.2f}" y2="{chart_bottom}" '
            f'stroke="{color}" stroke-width="0.8" stroke-dasharray="1 3" opacity="0.7">'
            f'<title>{html.escape(marker["label"] or marker["kind"])} +'
            f'{ns_to_ms(marker["time_ns"] - start):.3f} ms</title></line>'
        )

    if show_time_axis:
        span_ms = span / 1_000_000.0
        step_ms = 2.0 if span_ms <= 20.0 else 5.0 if span_ms <= 60.0 else 10.0
        time_ticks = [0.0]
        value = step_ms
        while value < span_ms:
            if span_ms - value >= step_ms * 0.6:
                time_ticks.append(value)
            value += step_ms
        time_ticks.append(span_ms)
        for index, elapsed_ms in enumerate(time_ticks):
            x = left + elapsed_ms * 1_000_000.0 * plot_width / span
            if index not in (0, len(time_ticks) - 1):
                output.append(
                    f'<line x1="{x:.2f}" y1="{rows_top}" x2="{x:.2f}" '
                    f'y2="{chart_bottom + 3}" stroke="#1c2d45" stroke-width="1" '
                    f'stroke-dasharray="2 4"/>'
                )
        axis_y = chart_bottom + 7
        output.append(
            f'<line x1="{left}" y1="{axis_y}" x2="{left + plot_width}" y2="{axis_y}" '
            'stroke="#52657f" stroke-width="1.2"/>'
        )
        output.append(svg_text(left - 12, axis_y + 4, "elapsed", "axis", "end"))
        for index, elapsed_ms in enumerate(time_ticks):
            x = left + elapsed_ms * 1_000_000.0 * plot_width / span
            output.append(
                f'<line x1="{x:.2f}" y1="{axis_y - 4}" x2="{x:.2f}" y2="{axis_y + 4}" '
                'stroke="#52657f" stroke-width="1"/>'
            )
            label = f'+{elapsed_ms:.0f} ms' if elapsed_ms.is_integer() else f'+{elapsed_ms:.3f} ms'
            anchor = "start" if index == 0 else "end" if index == len(time_ticks) - 1 else "middle"
            output.append(svg_text(x, chart_bottom + 22, label, "axis", anchor))

    for row_index, row in enumerate(rows):
        y = rows_top + row_index * row_height
        role_suffix = " · detail" if options[row[0]]["role"] != "root" else ""
        semantic_parts = [
            f"{label}={value}"
            for label, value in (
                ("eye", row[2]),
                ("pass", row[3]),
                ("src", SOURCE_ROLE_LABELS.get(row[4], row[4])),
                ("out", row[5]),
            )
            if value not in ("", "unscoped", "unknown", "mixed")
        ]
        lane_label = row[1]
        if semantic_parts:
            lane_label += " · " + " · ".join(semantic_parts)
        output.append(
            f'<rect data-stage-lane="{svg_attr(row[0])}" x="18" y="{y}" width="{width - 48}" '
            f'height="{row_height - 2}" rx="6" fill="none" stroke="#24334a" stroke-width="0.8"/>'
        )
        output.append(
            f'<rect x="18" y="{y}" width="{left - 30}" height="{row_height - 2}" '
            'rx="6" fill="#162944" stroke="#355173" stroke-width="0.8"/>'
        )
        output.append(svg_text(left - 14, y + 15, row[0] + role_suffix, "lane-stage", "end"))
        output.append(
            svg_text(left - 14, y + 30, clamp_text(lane_label, 68),
                     "lane-thread", "end")
        )
        output.append(
            f'<line x1="{left}" y1="{y + row_height - 2}" x2="{left + plot_width}" '
            f'y2="{y + row_height - 2}" stroke="#152239" stroke-width="1"/>'
        )
        opacity = 1.0 if options[row[0]]["role"] == "root" else 0.76
        for item in sorted(by_row[row], key=lambda value: value["start_ns"]):
            x = left + (item["start_ns"] - start) * plot_width / span
            rect_width = (item["end_ns"] - item["start_ns"]) * plot_width / span
            source_role = semantic_value(item, "source_role")
            role_label = SOURCE_ROLE_LABELS.get(source_role, source_role)
            title = f'{row[0]}'
            if source_role not in ("", "unscoped"):
                title += f' · {role_label}'
            title += f' {ns_to_ms(item["end_ns"] - item["start_ns"]):.3f} ms'
            screen = str(item.get("screen", "unknown"))
            if item.get("cmdlist_execution_sequence") is not None:
                title += f' · {SCREEN_LABELS.get(screen, screen.title())}'
            if item.get("token") is not None:
                title += f' token={item["token"]}'
            frame_identity, consumed_generation = source_frame_for(item)
            token_identity = item.get("token", item.get("id", "—"))
            source = item.get("source", "unknown")
            # Whatever measurements the bundle carried for this interval. Without
            # them a command list is just a coloured box: the panel could say when
            # it ran but not how much work it was.
            metrics = []
            for key, unit in DETAIL_METRICS:
                if key not in item or item[key] in (None, ""):
                    continue
                value = item[key]
                if key == "screen":
                    value = SCREEN_LABELS.get(str(value), str(value).title())
                if isinstance(value, (int, float)) and not isinstance(value, bool):
                    # Identifiers are labels, not quantities: grouping a frame id
                    # into "13,235" reads as a count of something.
                    if key.endswith("_id") or key == "pica_frame_id":
                        value = f"F{int(value)}"
                    elif float(value).is_integer():
                        value = f"{int(value):,}"
                    else:
                        value = f"{value:.3f}"
                metrics.append(f"{value} {unit}".strip())
            output.append(
                f'<rect class="interval" tabindex="0" role="button" '
                f'aria-label="{svg_attr(title)}" data-stage="{svg_attr(row[0])}" '
                f'data-lane="{svg_attr(row[1])}" data-frame="{svg_attr(frame_identity)}" '
                f'data-eye="{svg_attr(row[2])}" data-pass="{svg_attr(row[3])}" '
                f'data-source-role="{svg_attr(source_role)}" data-output-role="{svg_attr(row[5])}" '
                f'data-screen="{svg_attr(screen)}" '
                f'data-role-label="{svg_attr(role_label if source_role not in ("", "unscoped") else "")}" '
                f'data-metrics="{svg_attr(" · ".join(metrics))}" '
                f'data-consumed="{svg_attr("" if consumed_generation is None else consumed_generation)}" '
                f'data-band="{svg_attr(enclosing_frame(item["start_ns"]))}" '
                f'data-start="{ns_to_ms(item["start_ns"] - start):.3f}" '
                f'data-token="{svg_attr(token_identity)}" '
                f'data-duration="{ns_to_ms(item["end_ns"] - item["start_ns"]):.3f} ms" '
                f'data-source="{svg_attr(source)}" x="{x:.2f}" y="{y + 4}" '
                f'width="{rect_width:.6f}" height="18" rx="3" '
                f'fill="{item.get("color") or SOURCE_ROLE_COLORS.get(source_role, stage_colors[row[0]])}" '
                f'opacity="{opacity:.2f}">'
                f'<title>{html.escape(title)}</title></rect>'
            )
            visible_label = command_list_visible_label(item, rect_width)
            if visible_label:
                output.append(
                    svg_text(
                        x + rect_width / 2.0,
                        y + 16.5,
                        visible_label,
                        "interval-label",
                        "middle",
                    )
                )
            elif item.get("cmdlist_execution_sequence") is not None and item.get("draws") is not None:
                # Very short auxiliary command lists still need a readable static
                # identity. Put the compact label beside the interval; the
                # interactive card retains the full evidence and duration.
                draws = int(item["draws"])
                screen_label = SCREEN_LABELS.get(screen, screen.title())
                output.append(
                    svg_text(
                        x + rect_width + 3.0,
                        y + 16.5,
                        f"{draws:,}{screen_label[:1]}",
                        "interval-label",
                        "start",
                    )
                )
        # Inline phases: the children of this stage drawn as a thin strip under the
        # parent's bars, so one thread's loop reads as one row with coloured phases.
        parent_rows = [other for other in rows if other[0] == row[0]]
        for child in inline_children.get(row[0], []):
            child_items = [
                item
                for item in data["intervals"]
                if item["stage"] == child
                and (item["lane"] == row[1] or len(parent_rows) == 1)
            ]
            for item in sorted(child_items, key=lambda value: value["start_ns"]):
                x = left + (item["start_ns"] - start) * plot_width / span
                rect_width = (item["end_ns"] - item["start_ns"]) * plot_width / span
                item_ms = ns_to_ms(item["end_ns"] - item["start_ns"])
                title = f"{child} {item_ms:.3f} ms"
                if item.get("note"):
                    title += f' · {item["note"]}'
                metrics = [
                    str(item[key])
                    for key, _ in DETAIL_METRICS
                    if key in item and item[key] not in (None, "")
                ]
                output.append(
                    f'<rect class="interval" tabindex="0" role="button" '
                    f'aria-label="{svg_attr(title)}" data-stage="{svg_attr(child)}" '
                    f'data-lane="{svg_attr(item["lane"])}" '
                    f'data-frame="{svg_attr(enclosing_frame(item["start_ns"]))}" '
                    f'data-band="{svg_attr(enclosing_frame(item["start_ns"]))}" '
                    f'data-metrics="{svg_attr(" · ".join(metrics))}" '
                    f'data-start="{ns_to_ms(item["start_ns"] - start):.3f}" '
                    f'data-token="{svg_attr(item.get("token", item.get("id", "—")))}" '
                    f'data-duration="{item_ms:.3f} ms" '
                    f'data-source="{svg_attr(item.get("source", "unknown"))}" '
                    f'x="{x:.2f}" y="{y + 24}" width="{rect_width:.2f}" height="9" rx="2" '
                    f'fill="{item.get("color") or stage_colors[child]}" opacity="0.95">'
                    f'<title>{html.escape(title)}</title></rect>'
                )
        # Instant events on this row (e.g. P3D publish) as small pins.
        for marker in markers:
            if marker.get("stage") != row[0]:
                continue
            if marker.get("lane") and marker["lane"] != row[1]:
                continue
            x = left + (marker["time_ns"] - start) * plot_width / span
            color = MARKER_COLORS.get(marker["kind"], "#cbd5e1")
            label = marker["label"] or marker["kind"] or "marker"
            output.append(
                f'<path d="M{x:.2f},{y + 2} l-4,-6 l8,0 z" fill="{color}" stroke="#08111f" '
                f'stroke-width="0.6"><title>{html.escape(label)} +'
                f'{ns_to_ms(marker["time_ns"] - start):.3f} ms</title></path>'
            )
            output.append(
                f'<line x1="{x:.2f}" y1="{y + 2}" x2="{x:.2f}" y2="{y + row_height - 4}" '
                f'stroke="{color}" stroke-width="1" opacity="0.8"/>'
            )

    output.append(
        f'<rect x="14" y="{selection_top - 22}" width="{width - 28}" height="58" rx="8" '
        'fill="#0e1a2d" stroke="#1d2b43"/>'
    )
    output.append(
        svg_text(
            30,
            selection_top - 5,
            "Interactive detail · hover to inspect · click/Enter to pin · Esc to clear",
            "selection-hint",
        )
    )
    output.append(
        svg_text(30, selection_top + 19, "Select an interval to inspect it.", "selection-detail")
        .replace("<text ", '<text id="selection-detail" ')
    )
    # Floating panel. The strip above stays as the static fallback, but reading a
    # selection there means looking away from the row that was clicked, and it
    # scrolls out of view on a chart this tall.
    output.append(
        '<g id="hover-card" style="display:none;pointer-events:none">'
        f'<rect id="hover-bg" x="0" y="0" width="330" height="96" rx="8" '
        'fill="#0d1a2e" stroke="#31507d" stroke-width="1.2" opacity="0.98"/>'
        '<text id="hover-0" class="hover-title" x="12" y="20"></text>'
        '<text id="hover-1" class="hover-line" x="12" y="38"></text>'
        '<text id="hover-2" class="hover-line" x="12" y="54"></text>'
        '<text id="hover-3" class="hover-metric" x="12" y="72"></text>'
        '<text id="hover-4" class="hover-line" x="12" y="88"></text>'
        "</g>"
    )
    visible_source_roles = [
        role
        for role in ("record", "source_candidate", "replay", "auxiliary", "unknown")
        if any(semantic_value(item, "source_role") == role for item in data["intervals"])
        or any(
            summary.get("role_counts", {}).get(role, 0)
            for summary in report.get("draw_workload", [])
        )
    ]
    legend_x = width - 32 - len(visible_source_roles) * 108
    for index, role in enumerate(visible_source_roles):
        x = legend_x + index * 108
        output.append(
            f'<rect x="{x}" y="{selection_top - 14}" width="12" height="12" rx="2" '
            f'fill="{SOURCE_ROLE_COLORS[role]}"/>'
        )
        output.append(
            svg_text(x + 18, selection_top - 4, SOURCE_ROLE_LABELS[role], "selection-hint")
        )

    output.append(svg_text(24, summary_top, "Stage summary", "title"))
    columns = [
        ("stage", 24),
        ("count", 370),
        ("union", 470),
        ("coverage", 600),
        ("p50", 760),
        ("p90", 880),
        ("p99", 1000),
    ]
    for label, x in columns:
        output.append(svg_text(x, summary_top + 25, label.upper(), "summary-head"))
    for index, (stage, stats) in enumerate(root_stats):
        y = summary_top + 48 + index * 25
        output.append(f'<circle cx="28" cy="{y - 4}" r="4" fill="{stage_colors[stage]}"/>')
        output.append(svg_text(40, y, clamp_text(stage, 40), "summary"))
        output.append(svg_text(370, y, stats["occurrences"], "summary"))
        output.append(svg_text(470, y, f'{stats["union_ms"]:.3f} ms', "summary"))
        output.append(svg_text(600, y, f'{stats["window_coverage_percent"]:.1f}%', "summary"))
        output.append(svg_text(760, y, f'{stats["p50_ms"]:.3f}', "summary"))
        output.append(svg_text(880, y, f'{stats["p90_ms"]:.3f}', "summary"))
        output.append(svg_text(1000, y, f'{stats["p99_ms"]:.3f}', "summary"))
    if legend_stages or any(marker["kind"] for marker in markers):
        legend_y = summary_top + 48 + max(len(root_stats), 4) * 25 + 4
        cursor_x = 24
        output.append(svg_text(cursor_x, legend_y, "Inline phases / markers:", "summary-head"))
        cursor_x += 150
        for stage in legend_stages:
            output.append(
                f'<rect x="{cursor_x}" y="{legend_y - 9}" width="12" height="9" rx="2" '
                f'fill="{stage_colors[stage]}"/>'
            )
            label = clamp_text(stage.split(" / ")[-1], 26)
            output.append(svg_text(cursor_x + 16, legend_y, label, "summary"))
            cursor_x += 26 + 6.2 * len(label)
        for kind in sorted({marker["kind"] for marker in markers if marker["kind"]}):
            color = MARKER_COLORS.get(kind, "#cbd5e1")
            output.append(
                f'<path d="M{cursor_x + 6},{legend_y - 1} l-4,-7 l8,0 z" fill="{color}"/>'
            )
            output.append(svg_text(cursor_x + 16, legend_y, kind, "summary"))
            cursor_x += 26 + 6.2 * len(kind)

    histogram = report["concurrency_histogram"]
    bar_x, bar_y, bar_width = 1160, summary_top + 28, 470
    output.append(svg_text(bar_x, summary_top + 5, "Distinct root-stage concurrency", "summary-head"))
    cursor = bar_x
    histogram_colors = ["#26354d", "#3b82f6", "#39c6a3", "#ffb84d", "#ff6b7a"]
    for item in histogram:
        segment = bar_width * item["window_percent"] / 100.0
        color = histogram_colors[min(item["active_stages"], len(histogram_colors) - 1)]
        if segment > 0:
            output.append(
                f'<rect x="{cursor:.2f}" y="{bar_y}" width="{segment:.2f}" height="18" fill="{color}">'
                f'<title>{item["active_stages"]} stages: {item["window_percent"]:.2f}%</title></rect>'
            )
            cursor += segment
    for index, item in enumerate(histogram):
        y = bar_y + 42 + index * 20
        color = histogram_colors[min(item["active_stages"], len(histogram_colors) - 1)]
        output.append(f'<rect x="{bar_x}" y="{y - 10}" width="10" height="10" rx="2" fill="{color}"/>')
        output.append(
            svg_text(
                bar_x + 18,
                y,
                f'{item["active_stages"]} active: {item["duration_ms"]:.3f} ms ({item["window_percent"]:.1f}%)',
                "summary",
            )
        )
    output.append(
        svg_text(
            width - 24,
            height - 12,
            f"gamenative-stage-concurrency-analysis v{TOOL_VERSION}",
            "meta",
            "end",
        )
    )
    output.extend(
        [
            "<script><![CDATA[",
            "(() => {",
            "  const intervals = Array.from(document.querySelectorAll('.interval'));",
            "  const detail = document.getElementById('selection-detail');",
            "  const card = document.getElementById('hover-card');",
            "  const bg = document.getElementById('hover-bg');",
            "  const L = [0,1,2,3,4].map((i) => document.getElementById('hover-' + i));",
            "  const svg = document.getElementById('gamenative-stage-diagram');",
            "  let selected = null;",
            "  const lines = (d) => {",
            "    const role = d.roleLabel ? ` · ${d.roleLabel}` : '';",
            "    const token = d.token === '—' ? '' : ` · token ${d.token}`;",
            # A consumer runs inside one frame band while presenting an earlier
            # one. Showing only the resolved frame would hide that skew.
            "    const frame = d.consumed",
            "      ? `presents PICA gen ${d.consumed} · runs in band F${d.band}${token}`",
            "      : `source frame F${d.frame}${token}`;",
            "    return [",
            "      `${d.stage}${role}`,",
            "      frame,",
            "      `${d.lane} · +${d.start} ms · ${d.duration}`,",
            "      d.metrics || '',",
            "      d.source,",
            "    ];",
            "  };",
            "  const show = (item, ev) => {",
            "    const text = lines(item.dataset);",
            "    L.forEach((el, i) => el.textContent = text[i] || '');",
            "    const used = text.filter((t) => t).length;",
            "    bg.setAttribute('height', 24 + used * 17);",
            "    card.style.display = '';",
            # Measure the drawn text: a fixed width clips long lane and source
            # names, and the longest line is not always the same one.
            "    let w = 0;",
            "    L.forEach((el) => { if (el.textContent) w = Math.max(w, el.getBBox().width); });",
            "    bg.setAttribute('width', w + 24);",
            "    const box = item.getBoundingClientRect();",
            "    const host = svg.getBoundingClientRect();",
            "    const scale = svg.viewBox.baseVal.width / host.width || 1;",
            "    let x = (box.left - host.left) * scale + 14;",
            "    let y = (box.top - host.top) * scale - (24 + used * 17) - 8;",
            # Keep it inside the canvas: an interval near the right edge or in
            # the top row would otherwise put the card half outside.
            "    const cw = w + 24, ch = 24 + used * 17;",
            "    const vw = svg.viewBox.baseVal.width, vh = svg.viewBox.baseVal.height;",
            "    if (x + cw > vw - 8) x = Math.max(8, (box.left - host.left) * scale - cw - 10);",
            "    if (y < 8) y = (box.bottom - host.top) * scale + 10;",
            "    if (y + ch > vh - 8) y = vh - ch - 8;",
            "    card.setAttribute('transform', `translate(${x.toFixed(1)},${y.toFixed(1)})`);",
            "  };",
            "  const hide = () => { if (!selected) card.style.display = 'none'; };",
            "  const clear = () => {",
            "    if (selected) selected.classList.remove('selected');",
            "    selected = null;",
            "    card.style.display = 'none';",
            "    detail.textContent = 'Select an interval to inspect it.';",
            "  };",
            "  const select = (item) => {",
            "    if (selected === item) { clear(); return; }",
            "    if (selected) selected.classList.remove('selected');",
            "    selected = item; item.classList.add('selected');",
            "    show(item);",
            "    detail.textContent = lines(item.dataset).filter((t) => t).join(' · ');",
            "  };",
            "  intervals.forEach((item) => {",
            "    item.addEventListener('mouseenter', () => { if (!selected) show(item); });",
            "    item.addEventListener('mouseleave', hide);",
            "    item.addEventListener('click', () => select(item));",
            "    item.addEventListener('focus', () => { if (!selected) show(item); });",
            "    item.addEventListener('blur', hide);",
            "    item.addEventListener('keydown', (event) => {",
            "      if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); select(item); }",
            "    });",
            "  });",
            "  document.addEventListener('keydown', (event) => { if (event.key === 'Escape') clear(); });",
            "})();",
            "]]></script>",
        ]
    )
    output.append("</svg>")
    return "\n".join(output) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="version-1/2 stage bundle JSON")
    parser.add_argument("--output-svg", required=True, type=Path, help="self-contained SVG output")
    parser.add_argument("--output-json", type=Path, help="optional analysis report JSON")
    parser.add_argument(
        "--output-review-md",
        type=Path,
        help="optional Markdown review report suitable for Feishu document import",
    )
    parser.add_argument(
        "--receipt",
        type=Path,
        help="delivery receipt (default: <output-svg>.receipt.json)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail when analysis diagnostics contain errors",
    )
    parser.add_argument(
        "--require-frame-count",
        type=int,
        default=6,
        help="fail before writing artifacts unless the window contains this many complete source frames",
    )
    parser.add_argument(
        "--select-frame-count",
        type=int,
        default=6,
        help="select this many consecutive complete source frames before analysis",
    )
    parser.add_argument(
        "--select-frame-start",
        help="source-frame token at which --select-frame-count begins (default: first frame)",
    )
    args = parser.parse_args()

    data, input_bytes = load_input(args.input)
    if args.select_frame_start is not None and args.select_frame_count is None:
        parser.error("--select-frame-start requires --select-frame-count")
    if args.select_frame_count is not None:
        data = select_frame_window(data, args.select_frame_count, args.select_frame_start)
    report = analyze(data)
    if args.require_frame_count is not None:
        require_frame_count(report, args.require_frame_count)
    if args.strict and any(item.get("severity") == "error" for item in report["diagnostics"]):
        raise SystemExit("strict analysis failed before output: " + json.dumps(report["diagnostics"], ensure_ascii=False))
    svg_bytes = render_svg(data, report).encode("utf-8")
    report_bytes = (json.dumps(report, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    review_bytes = render_review_markdown(
        data, report, args.input, args.output_svg, args.output_json
    ).encode("utf-8")
    receipt_path = args.receipt or args.output_svg.with_suffix(
        args.output_svg.suffix + ".receipt.json"
    )
    atomic_write(args.output_svg, svg_bytes)
    if args.output_json:
        atomic_write(args.output_json, report_bytes)
    if args.output_review_md:
        atomic_write(args.output_review_md, review_bytes)
    receipt = {
        "schema_version": 1,
        "tool": "gamenative-stage-concurrency-analysis",
        "tool_version": TOOL_VERSION,
        "input": {
            "path": str(args.input),
            "bytes": len(input_bytes),
            "sha256": sha256_bytes(input_bytes),
        },
        "svg": {
            "path": str(args.output_svg),
            "bytes": len(svg_bytes),
            "sha256": sha256_bytes(svg_bytes),
        },
        "report": (
            {
                "path": str(args.output_json),
                "bytes": len(report_bytes),
                "sha256": sha256_bytes(report_bytes),
            }
            if args.output_json
            else None
        ),
        "review_markdown": (
            {
                "path": str(args.output_review_md),
                "bytes": len(review_bytes),
                "sha256": sha256_bytes(review_bytes),
            }
            if args.output_review_md
            else None
        ),
        "interval_count": report["interval_count"],
        "row_count": report["row_count"],
        "frame_count": report["frames"]["count"],
        "diagnostic_counts": dict(
            Counter(item.get("severity", "unknown") for item in report["diagnostics"])
        ),
    }
    receipt_bytes = (json.dumps(receipt, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    atomic_write(receipt_path, receipt_bytes)
    print(
        json.dumps(
            {
                "svg": str(args.output_svg),
                "report": str(args.output_json) if args.output_json else None,
                "review_markdown": (
                    str(args.output_review_md) if args.output_review_md else None
                ),
                "receipt": str(receipt_path),
                "intervals": report["interval_count"],
                "rows": report["row_count"],
                "diagnostics": receipt["diagnostic_counts"],
            },
            ensure_ascii=False,
        )
    )
    if args.strict and any(
        item.get("severity") == "error" for item in report["diagnostics"]
    ):
        raise SystemExit(2)


if __name__ == "__main__":
    main()
