import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "analyze_stage_intervals.py"
SPEC = importlib.util.spec_from_file_location("analyze_stage_intervals", SCRIPT)
assert SPEC and SPEC.loader
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


class ScopeSemanticTests(unittest.TestCase):
    def test_versioned_combined_and_legacy_split_suffixes(self):
        base, semantics = analyzer.parse_scope_semantics(
            "PICA draw [schema=azahar.stage.v1 stage_id=pica_draw lane=pica eye=left "
            "source_role=top_left output_role=not_applicable pass=draw]"
        )
        self.assertEqual(base, "PICA draw")
        self.assertEqual(semantics["eye"], "left")
        self.assertEqual(semantics["pass"], "draw")

        base, semantics = analyzer.parse_scope_semantics("FSR [eye=R] [pass=fsr1]")
        self.assertEqual(base, "FSR")
        self.assertEqual(semantics, {"pass": "fsr1", "eye": "right"})

    def test_nonsemantic_middle_bracket_is_not_guessed(self):
        base, semantics = analyzer.parse_scope_semantics("Worker [queue=7] tail")
        self.assertEqual(base, "Worker [queue=7] tail")
        self.assertEqual(semantics, {})

    def test_invalid_or_conflicting_eye_fails(self):
        with self.assertRaises(ValueError):
            analyzer.parse_scope_semantics("Draw [eye=upper]")
        with self.assertRaises(ValueError):
            analyzer.merge_scope_semantics({"eye": "right"}, "Draw [eye=left]")


class ConsecutiveFrameCoverageTests(unittest.TestCase):
    def _analyze(self):
        bundle = {
            "schema_version": 2,
            "title": "semantic-window",
            "time_domain": "trace_cpu_ns",
            "window": {"start_ns": 0, "end_ns": 30_000_000},
            "frame_markers": [
                {"frame": 10, "time_ns": 0},
                {"frame": 11, "time_ns": 10_000_000},
                {"frame": 12, "time_ns": 20_000_000},
                {"frame": 13, "time_ns": 30_000_000},
            ],
            "intervals": [
                {
                    "stage": "PICA",
                    "lane": "PicaWorker",
                    "start_ns": 5_000_000,
                    "end_ns": 25_000_000,
                    "scope_name": "PICA draw [eye=L] [pass=draw]",
                    "source_role": "top_left",
                },
                {
                    "stage": "StatusLayer",
                    "lane": "Host",
                    "start_ns": 10_000_000,
                    "end_ns": 20_000_000,
                },
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bundle.json"
            path.write_text(json.dumps(bundle), encoding="utf-8")
            data, _ = analyzer.load_input(path)
            return data, analyzer.analyze(data)

    def test_n_plus_one_markers_produce_n_ordered_frames(self):
        _, report = self._analyze()
        self.assertEqual(report["frames"]["count"], 3)
        self.assertEqual(report["axis_mode"], "frame_and_time")
        self.assertEqual([frame["frame"] for frame in report["frame_semantics"]], [10, 11, 12])

    def test_review_frame_count_gate_requires_exactly_three_frames(self):
        _, report = self._analyze()
        analyzer.require_frame_count(report, 3)
        with self.assertRaisesRegex(ValueError, "exactly 2 complete source frames; got 3"):
            analyzer.require_frame_count(report, 2)

    def test_frame_window_selection_preserves_internal_cross_frame_work(self):
        data, _ = self._analyze()
        selected = analyzer.select_frame_window(data, 2, "11")
        self.assertEqual([marker["frame"] for marker in selected["frame_markers"]], [11, 12, 13])
        self.assertEqual(selected["window"], {"start_ns": 10_000_000, "end_ns": 30_000_000})
        pica = next(item for item in selected["intervals"] if item["stage"] == "PICA")
        self.assertEqual((pica["start_ns"], pica["end_ns"]), (10_000_000, 25_000_000))
        self.assertTrue(pica["window_clipped"])

    def test_cross_frame_span_is_half_open_clipped_into_each_frame(self):
        data, report = self._analyze()
        pica = [
            next(item for item in frame["coverage"] if item["stage"] == "PICA")
            for frame in report["frame_semantics"]
        ]
        self.assertEqual([item["union_ms"] for item in pica], [5.0, 10.0, 5.0])
        self.assertTrue(all(item["eye"] == "left" for item in pica))
        self.assertTrue(all(item["pass"] == "draw" for item in pica))

        status_frames = [
            frame["frame"]
            for frame in report["frame_semantics"]
            if any(item["stage"] == "StatusLayer" for item in frame["coverage"])
        ]
        self.assertEqual(status_frames, [11])

        svg = analyzer.render_svg(data, report)
        self.assertIn("F10 · DC 0/?", svg)
        self.assertIn("F12 · DC 0/?", svg)
        self.assertNotIn("F13 · DC", svg)
        self.assertIn('data-relative-frame="N"', svg)
        self.assertIn('data-relative-frame="N+1"', svg)
        self.assertIn('data-relative-frame="N+2"', svg)
        self.assertNotIn('data-relative-frame="N+3"', svg)
        self.assertIn(">N<", svg)
        self.assertIn(">N+1<", svg)
        self.assertIn(">N+2<", svg)
        self.assertIn(">elapsed<", svg)
        self.assertIn(">+10 ms<", svg)
        self.assertIn(">+30 ms<", svg)
        self.assertIn('data-stage="PICA"', svg)
        self.assertIn('data-duration="20.000 ms"', svg)

    def test_missing_dimensions_remain_explicit_and_svg_is_deterministic(self):
        data, report = self._analyze()
        status = next(
            item
            for frame in report["frame_semantics"]
            for item in frame["coverage"]
            if item["stage"] == "StatusLayer"
        )
        self.assertEqual(status["eye"], "unscoped")
        self.assertEqual(status["pass"], "unscoped")
        svg = analyzer.render_svg(data, report)
        self.assertEqual(svg, analyzer.render_svg(data, report))
        self.assertNotIn("eye=unscoped", svg)
        self.assertNotIn("pass=unscoped", svg)
        self.assertIn(">Host<", svg)

    def test_pica_record_replay_roles_are_visible_and_use_distinct_colors(self):
        data, _ = self._analyze()
        data["axis_mode"] = "frame_and_time"
        data["stage_options"] = {"PICA": {"source_role_layout": "color"}}
        record = next(item for item in data["intervals"] if item["stage"] == "PICA")
        record["source_role"] = "record"
        replay = dict(record)
        replay.update(
            {
                "start_ns": 6_000_000,
                "end_ns": 9_000_000,
                "source_role": "replay",
                "token": 42,
            }
        )
        data["intervals"].append(replay)
        report = analyzer.analyze(data)
        svg = analyzer.render_svg(data, report)
        self.assertEqual(report["row_count"], 2)
        self.assertNotIn("src=Record", svg)
        self.assertNotIn("src=Replay", svg)
        self.assertIn("PICA · Record", svg)
        self.assertIn("PICA · Replay", svg)
        self.assertIn(">Record<", svg)
        self.assertIn(">Replay<", svg)
        self.assertIn('fill="#39c6a3"', svg)
        self.assertIn('fill="#a78bfa"', svg)
        self.assertIn(">+10 ms<", svg)

    def test_per_frame_correlation_requires_full_explicit_coverage(self):
        data, _ = self._analyze()
        data["frame_correlations"] = [
            {"source_frame": 10, "status": "exact_pica_work", "shared_pica_keys": ["1:10"]},
            {"source_frame": 11, "status": "neighborhood"},
        ]
        report = analyzer.analyze(data)
        codes = {item["code"] for item in report["diagnostics"]}
        self.assertIn("FrameCorrelationReasonMissing", codes)
        self.assertIn("FrameCorrelationCoverageMissing", codes)


class PtracyBaseNameTests(unittest.TestCase):
    def test_name_base_matches_semantic_suffix_without_changing_name_exact(self):
        data = {
            "ptracy_slices": [
                {
                    "stage": "PICA",
                    "name_base": "PICA draw",
                    "slices": [
                        {
                            "name": "PICA draw [eye=right] [pass=draw]",
                            "threadName": "PicaWorker",
                            "startMs": 1.0,
                            "endMs": 2.0,
                        },
                        {"name": "Other", "startMs": 2.0, "endMs": 3.0},
                    ],
                }
            ]
        }
        intervals = analyzer.normalize_ptracy_slices(data)
        self.assertEqual(len(intervals), 1)
        self.assertEqual(intervals[0]["scope_base"], "PICA draw")
        self.assertEqual(intervals[0]["eye"], "right")


class DetailPanelTests(unittest.TestCase):
    """A coloured box says when work ran, not what the work was."""

    def _render(self, extra=None):
        bundle = {
            "schema_version": 2,
            "title": "detail",
            "time_domain": "trace_cpu_ns",
            "window": {"start_ns": 0, "end_ns": 20_000_000},
            "frame_markers": [
                {"frame": 40, "time_ns": 0},
                {"frame": 41, "time_ns": 10_000_000},
                {"frame": 42, "time_ns": 20_000_000},
            ],
            "intervals": [
                {
                    "stage": "PICA CmdLists",
                    "lane": "PicaWorker",
                    "start_ns": 1_000_000,
                    "end_ns": 4_000_000,
                    **(extra or {}),
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bundle.json"
            path.write_text(json.dumps(bundle), encoding="utf-8")
            data, _ = analyzer.load_input(path)
            return analyzer.render_svg(data, analyzer.analyze(data))

    def test_measurements_reach_the_panel_with_the_owning_frame(self):
        svg = self._render({"draws": 285, "records": 2, "source_role": "record"})
        self.assertIn("285 draws \u00b7 2 records", svg)
        self.assertIn('data-frame="40"', svg)
        self.assertIn('id="hover-card"', svg)

    def test_absent_measurements_are_absent_not_invented(self):
        svg = self._render()
        self.assertIn('data-metrics=""', svg)
        self.assertNotIn(" draws ·", svg.split("<script>")[0])

    def test_command_lists_show_exact_draw_count_and_direct_screen(self):
        bundle = {
            "schema_version": 2,
            "title": "command-list-screen",
            "time_domain": "trace_cpu_ns",
            "window": {"start_ns": 0, "end_ns": 20_000_000},
            "frame_markers": [
                {"frame": 40, "time_ns": 0},
                {"frame": 41, "time_ns": 10_000_000},
                {"frame": 42, "time_ns": 20_000_000},
            ],
            "draw_events": [
                {
                    "sequence": 1,
                    "time_ns": 2_000_000,
                    "pica_frame_id": 40,
                    "cmdlist_execution_sequence": 7,
                    "metadata": 1 << 24,
                },
                {
                    "sequence": 2,
                    "time_ns": 3_000_000,
                    "pica_frame_id": 40,
                    "cmdlist_execution_sequence": 7,
                    "metadata": 4 << 24,
                },
                {
                    "sequence": 3,
                    "time_ns": 12_000_000,
                    "pica_frame_id": 41,
                    "cmdlist_execution_sequence": 8,
                    "metadata": 3 << 24,
                },
            ],
            "intervals": [
                {
                    "stage": "PICA",
                    "lane": "PicaWorker",
                    "start_ns": 1_000_000,
                    "end_ns": 9_000_000,
                    "cmdlist_execution_sequence": 7,
                },
                {
                    "stage": "PICA",
                    "lane": "PicaWorker",
                    "start_ns": 11_000_000,
                    "end_ns": 19_000_000,
                    "cmdlist_execution_sequence": 8,
                },
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bundle.json"
            path.write_text(json.dumps(bundle), encoding="utf-8")
            data, _ = analyzer.load_input(path)
            command_lists = data["intervals"]
            self.assertEqual(command_lists[0]["draws"], 2)
            self.assertEqual(command_lists[0]["screen"], "top")
            self.assertEqual(command_lists[1]["draws"], 1)
            self.assertEqual(command_lists[1]["screen"], "bottom")
            svg = analyzer.render_svg(data, analyzer.analyze(data))
            self.assertIn("2 DC · Top", svg)
            self.assertIn("1 DC · Bottom", svg)
            self.assertIn('data-screen="top"', svg)
            self.assertIn('data-screen="bottom"', svg)
            self.assertIn("Offscreen:1, Top Left:1", svg)

    def test_command_list_draw_count_mismatch_is_rejected(self):
        intervals = [{"cmdlist_execution_sequence": 7, "draws": 2}]
        events = [{"cmdlist_execution_sequence": 7, "display_target": "top_left"}]
        with self.assertRaisesRegex(ValueError, "command-list draw count mismatch"):
            analyzer.enrich_command_list_intervals(intervals, events)


class CompletedWorkloadIdentityTests(unittest.TestCase):
    def _load(self, draw_count=2):
        bundle = {
            "schema_version": 2,
            "title": "completed-workload",
            "time_domain": "trace_cpu_ns",
            "window": {"start_ns": 0, "end_ns": 30_000_000},
            "frame_markers": [
                {"frame": 99, "time_ns": 0, "boundary": "end"},
                {"frame": 100, "time_ns": 10_000_000, "boundary": "end"},
                {"frame": 101, "time_ns": 20_000_000, "boundary": "end"},
                {"frame": 102, "time_ns": 30_000_000, "boundary": "end"},
            ],
            "frame_workloads": [
                {"frame": 100, "time_ns": 10_000_000, "draw_calls": draw_count},
                {"frame": 101, "time_ns": 20_000_000, "draw_calls": 0},
                {"frame": 102, "time_ns": 30_000_000, "draw_calls": 0},
            ],
            "draw_events": [
                {
                    "sequence": 7,
                    "time_ns": 2_000_000,
                    "pica_frame_id": 500,
                    "cmdlist_execution_sequence": 30,
                    "metadata": 1 | (1 << 8) | (4 << 32),
                },
                {
                    "sequence": 8,
                    "time_ns": 5_000_000,
                    "pica_frame_id": 500,
                    "cmdlist_execution_sequence": 31,
                    "metadata": 2 | (2 << 8) | (1 << 16) | (5 << 32),
                },
            ],
            "intervals": [
                {
                    "stage": "PICA",
                    "lane": "PicaWorker",
                    "start_ns": 1_000_000,
                    "end_ns": 28_000_000,
                    "pica_frame_id": 500,
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bundle.json"
            path.write_text(json.dumps(bundle), encoding="utf-8")
            return analyzer.load_input(path)[0]

    def test_end_markers_name_the_frame_that_just_completed(self):
        data = self._load()
        report = analyzer.analyze(data)
        self.assertEqual([item["frame"] for item in report["frames"]["intervals"]], [100, 101, 102])
        first = report["draw_workload"][0]
        self.assertEqual(first["expected_draws"], 2)
        self.assertEqual(first["resolved_draws"], 2)
        self.assertEqual(first["role_counts"]["source_candidate"], 1)
        self.assertEqual(first["role_counts"]["replay"], 1)
        self.assertEqual(first["pica_frames"], [500])
        self.assertEqual(first["status"], "exact")
        svg = analyzer.render_svg(data, report)
        self.assertIn('data-source-frame="100"', svg)
        self.assertIn("F100 · DC 2/2", svg)
        self.assertIn("R0 C1 P1 A0 U0", svg)
        self.assertIn('data-frame="PICA:500"', svg)

    def test_draw_count_mismatch_is_a_hard_diagnostic(self):
        data = self._load(draw_count=3)
        report = analyzer.analyze(data)
        mismatch = [item for item in report["diagnostics"] if item["code"] == "DrawWorkloadMismatch"]
        self.assertEqual(len(mismatch), 1)
        self.assertEqual(mismatch[0]["expected_draws"], 3)
        self.assertEqual(mismatch[0]["resolved_draws"], 2)

    def test_selection_uses_measured_frame_not_prior_end_boundary(self):
        data = self._load()
        selected = analyzer.select_frame_window(data, 2, "101")
        report = analyzer.analyze(selected)
        self.assertEqual([item["frame"] for item in report["frames"]["intervals"]], [101, 102])
        self.assertEqual(selected["window"], {"start_ns": 10_000_000, "end_ns": 30_000_000})

    def test_raw_ptracy_queries_materialize_only_committed_tuples(self):
        workload_names = analyzer.WORKLOAD_COUNTERS
        draw_names = analyzer.DRAW_COUNTERS

        def counter(name, samples):
            return {
                "counterName": name,
                "availableSampleCount": len(samples),
                "truncated": False,
                "samples": [{"time": time, "value": value} for time, value in samples],
            }

        counters = [
            counter(analyzer.WORKLOAD_COMMIT_COUNTER, [(0, 99), (10_000_000, 100), (20_000_000, 101), (30_000_000, 102)])
        ]
        for offset, (field, name) in enumerate(workload_names.items(), start=1):
            value = 2 if field == "draw_calls" else 0
            counters.append(
                counter(name, [(9_000_000 + offset, value), (19_000_000 + offset, 0), (29_000_000 + offset, 0)])
            )
        counters.append(counter(analyzer.DRAW_COMMIT_COUNTER, [(2_000_000, 7), (5_000_000, 8)]))
        draw_values = {
            "pica_frame_id": [500, 500],
            "cmdlist_execution_sequence": [30, 31],
            "submission_id": [70, 71],
            "segment_sequence": [80, 81],
            "command_word_offset": [100, 200],
            "metadata": [1 | (1 << 8), 2 | (2 << 8)],
        }
        for offset, (field, name) in enumerate(draw_names.items(), start=1):
            counters.append(
                counter(
                    name,
                    [
                        (1_000_000 + offset, draw_values[field][0]),
                        (4_000_000 + offset, draw_values[field][1]),
                    ],
                )
            )
        bundle = {
            "schema_version": 2,
            "time_domain": "trace_cpu_ns",
            "window": {"start_ns": 0, "end_ns": 30_000_000},
            "ptracy_counter_queries": [{"counters": counters}],
            "intervals": [
                {"stage": "PICA", "lane": "PicaWorker", "start_ns": 1, "end_ns": 29_000_000}
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bundle.json"
            path.write_text(json.dumps(bundle), encoding="utf-8")
            data, _ = analyzer.load_input(path)
        self.assertEqual([item["frame"] for item in data["frame_markers"]], [99, 100, 101, 102])
        self.assertTrue(all(item["boundary"] == "end" for item in data["frame_markers"]))
        self.assertEqual([item["frame"] for item in data["frame_workloads"]], [100, 101, 102])
        self.assertEqual([item["sequence"] for item in data["draw_events"]], [7, 8])
        report = analyzer.analyze(data)
        self.assertEqual(report["draw_workload"][0]["status"], "exact")

class ConsumerFrameAttributionTests(unittest.TestCase):
    """A present cannot deliver a frame whose drawing has not finished."""

    def _render(self):
        bundle = {
            "schema_version": 2,
            "title": "attribution",
            "time_domain": "trace_cpu_ns",
            "window": {"start_ns": 0, "end_ns": 40_000_000},
            "frame_markers": [
                {"frame": 100, "time_ns": 0},
                {"frame": 101, "time_ns": 20_000_000},
                {"frame": 102, "time_ns": 40_000_000},
            ],
            "stage_options": {"Present": {"consumes_gpu_output": True}},
            "intervals": [
                # Generation 7 finishes early in band 100.
                {
                    "stage": "PICA",
                    "lane": "PicaWorker",
                    "start_ns": 1_000_000,
                    "end_ns": 5_000_000,
                    "pica_frame_id": 7,
                },
                # Generation 8 is still being drawn when the present below runs.
                {
                    "stage": "PICA",
                    "lane": "PicaWorker",
                    "start_ns": 12_000_000,
                    "end_ns": 30_000_000,
                    "pica_frame_id": 8,
                },
                # Lands in band 100 but can only be showing generation 7.
                {
                    "stage": "Present",
                    "lane": "Swapchain",
                    "start_ns": 15_000_000,
                    "end_ns": 16_000_000,
                },
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bundle.json"
            path.write_text(json.dumps(bundle), encoding="utf-8")
            data, _ = analyzer.load_input(path)
            return analyzer.render_svg(data, analyzer.analyze(data))

    def test_consumer_is_credited_to_the_generation_that_had_finished(self):
        svg = self._render()
        present = [tag for tag in svg.split("<rect ") if 'data-stage="Present"' in tag][0]
        self.assertIn('data-consumed="7"', present)
        self.assertNotIn('data-consumed="8"', present)

    def test_consumer_still_reports_the_band_it_ran_in(self):
        svg = self._render()
        present = [tag for tag in svg.split("<rect ") if 'data-stage="Present"' in tag][0]
        self.assertIn('data-band="100"', present)

    def test_producers_are_left_attributed_by_where_they_run(self):
        svg = self._render()
        pica = [tag for tag in svg.split("<rect ") if 'data-stage="PICA"' in tag]
        for tag in pica:
            self.assertIn('data-consumed=""', tag)


class GuestCommandTraceTests(unittest.TestCase):
    def test_manifest_joins_by_pica_frame_and_reaches_review(self):
        bundle = {
            "schema_version": 2,
            "time_domain": "trace_cpu_ns",
            "window": {"start_ns": 0, "end_ns": 10_000_000},
            "frame_markers": [
                {"frame": 10, "time_ns": 0},
                {"frame": 11, "time_ns": 10_000_000},
            ],
            "intervals": [
                {"stage": "PICA", "lane": "PicaWorker", "start_ns": 1, "end_ns": 9_000_000}
            ],
            "draw_events": [
                {
                    "sequence": 1,
                    "time_ns": 1_000_000,
                    "pica_frame_id": 70,
                    "cmdlist_execution_sequence": 4,
                    "metadata": 0,
                }
            ],
            "guest_command_traces": [
                {
                    "schema": "xr3ds.guest-command-trace.manifest.v1",
                    "capture_id": 3,
                    "program_id": "00040000000B1D00",
                    "extension": ".gcmdtrace.3ds",
                    "source": "test.gcmdtrace.3ds",
                    "counts": {"frames": 1},
                    "capabilities": {"gpu_ordered_probe": False},
                    "completeness": {"dropped_records": 0},
                    "join_keys": [
                        "frame_id",
                        "pica_submission_id",
                        "pica_segment_sequence",
                        "command_word_offset",
                        "action_id",
                    ],
                    "frames": [
                        {
                            "frame_id": 70,
                            "submissions": 2,
                            "segments": 3,
                            "draws": 1,
                            "resources": 8,
                            "pipelines": ["1234"],
                            "producer_proof": {"submission_context_only": 2},
                            "resource_owners": {"guest_mapped": 7, "physical_only": 1},
                            "dropped_records": 0,
                        }
                    ],
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bundle.json"
            path.write_text(json.dumps(bundle), encoding="utf-8")
            data, _ = analyzer.load_input(path)
            report = analyzer.analyze(data)
            joined = report["guest_command_trace"]["frames"][0]
            self.assertEqual(joined["status"], "exact")
            self.assertEqual(joined["submissions"], 2)
            self.assertEqual(joined["guest_mapped_resources"], 7)
            svg = analyzer.render_svg(data, report)
            self.assertIn("GC S2 D1 R8", svg)
            self.assertIn('data-gcmdtrace-status="exact"', svg)
            review = analyzer.render_review_markdown(
                data, report, path, Path("out.svg"), Path("out.json")
            )
            self.assertIn("Guest command trace correlation", review)
            self.assertIn("watch=0 prof=0 context=2 unknown=0", review)


class XrLaneModelTests(unittest.TestCase):
    """Inline phases, instant markers, coalesced GPU zones and frame annotations."""

    def _bundle(self):
        return {
            "schema_version": 2,
            "title": "xr-lanes",
            "time_domain": "trace_cpu_ns",
            "window": {"start_ns": 0, "end_ns": 60_000_000},
            "frame_markers": [
                {"frame": 100, "time_ns": 0, "boundary": "end"},
                {"frame": 101, "time_ns": 30_000_000, "boundary": "end"},
                {"frame": 102, "time_ns": 60_000_000, "boundary": "end"},
            ],
            "frame_annotations": [
                {"frame": 101, "class": "", "badges": ["2 tick"], "metrics": {"ticks": 2}},
                {"frame": 102, "class": "slip", "badges": ["3 tick", "P3D last +19.0"],
                 "metrics": {"ticks": 3}, "note": "late"},
            ],
            "markers": [
                {"stage": "Guest", "time_ns": 12_000_000, "label": "P3D publish", "kind": "p3d"},
                {"time_ns": 16_700_000, "label": "VBlank", "kind": "vblank"},
                {"stage": "Guest", "time_ns": 99_000_000_000, "label": "outside", "kind": "p3d"},
            ],
            "stage_options": {
                "Guest": {"order": 10, "role": "root"},
                "XR Loop": {"order": 50, "role": "root", "count_for_concurrency": False},
                "XR Loop / wait": {"order": 51, "role": "detail", "parent": "XR Loop",
                                   "layout": "inline", "summary": True},
                "GPU HW": {"order": 80, "role": "detail", "include_in_overlap": False,
                           "count_for_concurrency": False},
            },
            "intervals": [
                {"stage": "Guest", "lane": "cores", "start_ns": 1_000_000, "end_ns": 15_000_000},
                {"stage": "XR Loop", "lane": "EmuMain", "start_ns": 0, "end_ns": 60_000_000},
                {"stage": "XR Loop / wait", "lane": "EmuMain", "start_ns": 20_000_000,
                 "end_ns": 34_000_000, "color": "#60a5fa"},
            ],
            "ptracy_slices": [
                {
                    "stage": "GPU HW",
                    "lane": "GPU",
                    "coalesce_gap_ns": 250_000,
                    "slices": [
                        {"name": "Vulkan RenderPass", "startMs": 5.0, "endMs": 5.1},
                        {"name": "Vulkan RenderPass", "startMs": 5.2, "endMs": 5.3},
                        {"name": "Vulkan RenderPass", "startMs": 5.35, "endMs": 9.0},
                        {"name": "Vulkan RenderPass", "startMs": 40.0, "endMs": 41.0},
                    ],
                }
            ],
        }

    def _load(self, bundle):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bundle.json"
            path.write_text(json.dumps(bundle), encoding="utf-8")
            data, _ = analyzer.load_input(path)
            data = analyzer.select_frame_window(data, 2)
            report = analyzer.analyze(data)
            svg = analyzer.render_svg(data, report)
            review = analyzer.render_review_markdown(
                data, report, path, Path("out.svg"), Path("out.json")
            )
            return data, report, svg, review

    def test_inline_stage_shares_its_parent_row_and_keeps_statistics(self):
        data, report, svg, _ = self._load(self._bundle())
        # Guest, XR Loop and GPU HW are rows; the inline phase is not.
        self.assertEqual(report["row_count"], 3)
        self.assertIn("XR Loop / wait", report["stage_stats"])
        self.assertAlmostEqual(report["stage_stats"]["XR Loop / wait"]["union_ms"], 14.0)
        self.assertIn('data-stage="XR Loop / wait"', svg)
        self.assertIn('fill="#60a5fa"', svg)
        self.assertNotIn("XR Loop / wait · detail", svg)

    def test_inline_stage_without_row_parent_is_rejected(self):
        bundle = self._bundle()
        bundle["stage_options"]["XR Loop / wait"]["parent"] = "XR Loop / wait"
        with self.assertRaises(ValueError):
            self._load(bundle)

    def test_coalesced_gpu_zones_keep_count_and_busy_sum(self):
        data, report, svg, _ = self._load(self._bundle())
        merged = [item for item in data["intervals"] if item["stage"] == "GPU HW"]
        self.assertEqual(len(merged), 2)
        first = min(merged, key=lambda item: item["start_ns"])
        self.assertEqual(first["merged_count"], 3)
        self.assertAlmostEqual(first["merged_busy_ms"], 3.85)
        self.assertEqual(first["end_ns"], 9_000_000)
        self.assertIn("zones merged", svg)

    def test_markers_are_drawn_on_their_row_and_clipped_to_the_window(self):
        data, report, svg, _ = self._load(self._bundle())
        self.assertEqual(len(report["markers"]), 2)
        self.assertIn("P3D publish +12.000 ms", svg)
        self.assertIn('stroke-dasharray="1 3"', svg)
        self.assertNotIn("outside", svg)

    def test_frame_annotations_tint_the_band_and_reach_the_review(self):
        _, report, svg, review = self._load(self._bundle())
        self.assertEqual([item["class"] for item in report["frame_annotations"]], ["", "slip"])
        self.assertIn(analyzer.FRAME_CLASS_FILLS["slip"][1], svg)
        self.assertIn("SLIP · 3 tick · P3D last +19.0", svg)
        self.assertIn("## Frame annotations", review)
        self.assertIn("| 102 | slip | 3 tick · P3D last +19.0 | 3 | late |", review)


if __name__ == "__main__":
    unittest.main()
