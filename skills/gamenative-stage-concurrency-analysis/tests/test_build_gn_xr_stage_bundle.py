"""Builder tests on deterministic synthetic Litep responses (not capture evidence)."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import analyze_stage_intervals as analyzer  # noqa: E402
import build_gn_xr_stage_bundle as builder  # noqa: E402
import deliver_stage_diagram as delivery  # noqa: E402

MS = 1_000_000
T0 = 1_000 * MS                 # trace clock origin of the first game frame
OFFSET = 5_000 * MS             # CLOCK_MONOTONIC - trace clock
FRAME = 25 * MS                 # 40 fps game
XR = 13_889_000                 # 72 Hz Pico loop
FIDS = list(range(100, 109))    # nine complete frames; the builder picks seven


def game_spans(index):
    base = T0 + index * FRAME
    return base, {
        'frame_sync': (0.0, 0.8), 'swapchain_wait.l': (13.6, 14.6), 'swapchain_wait.r': (15.8, 16.4),
        'endframe.drain': (17.6, 20.0), 'endframe.submit': (20.0, 21.0),
        's.fence_wait': (21.0, 22.8), 's.send': (22.8, 24.0),
    }


def shown_fid(time):
    """The newest fid whose bridge submit (trace clock) ended by an XR draw at this time."""
    done = [fid for index, fid in enumerate(FIDS) if T0 + index * FRAME + 21 * MS <= time]
    return done[-1] if done else FIDS[0] - 1


def counters(drop=None, duplicate=None, skip_fid=None, present=True, split_eye_at=None):
    series = {}

    def add(name, time, value):
        series.setdefault(name, []).append({'time': int(time), 'value': int(value)})

    for index, fid in enumerate(FIDS):
        if fid == skip_fid:
            continue
        base, spans = game_spans(index)
        commit = base + int(24.1 * MS)
        for key, (begin, end) in spans.items():
            prefix = 'vr.s.' + key[2:] if key.startswith('s.') else 'vr.g.' + key
            for suffix, offset_ms in (('begin_ns', begin), ('end_ns', end)):
                name = f'{prefix}.{suffix}'
                if name == drop:
                    continue
                add(name, commit - 50_000, base + int(offset_ms * MS) + OFFSET)
                if name == duplicate and index == 3:
                    add(name, commit - 40_000, base + int(offset_ms * MS) + OFFSET)
        add('vr.g.snap', commit - 50_000, fid * 2)
        add('vr.g.endframe.drain.lock_ns', commit - 50_000, base + int(19.5 * MS) + OFFSET)
        add('vr.g.fid', commit, fid)
    for k in range(20):
        add('vr.android.serial', T0 + k * XR + int(2.5 * MS), 500 + k)
        add('vr.present.fresh', T0 + k * XR + int(4.5 * MS), 2 if k % 2 == 0 else 0)
        if present:
            draw = T0 + k * XR + int(4.5 * MS)
            left = shown_fid(draw)
            add('vr.present.fid.l', draw, left)
            add('vr.present.fid.r', draw + 1_000, left - 1 if k == split_eye_at else left)
    return {'counters': [{'counterName': name, 'samples': samples, 'truncated': False}
                         for name, samples in sorted(series.items())]}


def app_slices():
    slices = []

    def zone(name, start, end, thread):
        slices.append({'name': name, 'startMs': start / MS, 'endMs': end / MS, 'threadName': thread})

    for k in range(20):
        t = T0 + k * XR
        zone('host.vr.xr.frame', t, t + 5 * MS, 'xr')
        zone('host.vr.xr.wait_frame', t, t + 2 * MS, 'xr')
        zone('host.vr.xr.input_locate', t + 2 * MS, t + 3 * MS, 'xr')
        zone('host.vr.projection.render', t + 3 * MS, t + int(4.5 * MS), 'xr')
        zone('host.vr.xr.end_frame', t + int(4.5 * MS), t + 5 * MS, 'xr')
    for index, _ in enumerate(FIDS):
        base, _ = game_spans(index)
        zone('host.vr.transport.frame', base + int(23.7 * MS), base + int(24.0 * MS), 'transport')
        zone('host.vr.transport.acquire_wait', base + int(13.6 * MS), base + int(14.6 * MS), 'transport')
        zone('vr.control.frame_sync', base, base + int(0.8 * MS), 'control')
    zone('host.vr.unmodelled', T0, T0 + MS, 'xr')
    return {'slices': slices}


class GameNativeBuilderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def write_inputs(self, **counter_options):
        (self.base / 'counters.json').write_text(json.dumps(counters(**counter_options)))
        (self.base / 'slices.json').write_text(json.dumps(app_slices()))
        manifest = {'title': 'Synthetic GameNative XR', 'identity': {'apk_sha256': 'synthetic'},
                    'clock': {'mono_minus_trace_ns': OFFSET},
                    'counters': ['counters.json'], 'slices': ['slices.json'], 'xr_period_ms': 13.889}
        (self.base / 'manifest.json').write_text(json.dumps(manifest))
        return manifest

    def bundle(self, **counter_options):
        return builder.build(self.write_inputs(**counter_options), self.base)

    def test_six_frames_end_at_bridge_submit_on_the_trace_clock(self):
        bundle = self.bundle()
        markers = bundle['frame_markers']
        self.assertEqual(len(markers), 7)
        self.assertEqual([m['frame'] for m in markers], [f'fid {f}' for f in FIDS[:7]])
        self.assertTrue(all(m['boundary'] == 'end' for m in markers))
        self.assertEqual(markers[0]['time_ns'], T0 + 21 * MS)   # converted from CLOCK_MONOTONIC
        self.assertEqual(bundle['window'], {'start_ns': markers[0]['time_ns'], 'end_ns': markers[-1]['time_ns']})
        stages = {i['stage'] for i in bundle['intervals']}
        for stage in ('Game thread', 'Game thread / DXVK drain', 'Game thread / swapchain wait L',
                      'Shipper / fence wait', 'XR loop', 'XR loop / draw eyes', 'App transport',
                      'App transport / ACQUIRE wait', 'App control / FRAME_SYNC'):
            self.assertIn(stage, stages)
        self.assertEqual(bundle['diagnostics'][0]['code'], 'ScopeNotInLaneModel')
        labels = [p['label'] for p in bundle['markers']]
        self.assertEqual(labels[0], 'aser 502')   # 501 is published before the first frame end
        self.assertTrue(all(bundle['window']['start_ns'] <= p['time_ns'] <= bundle['window']['end_ns']
                            for p in bundle['markers']))
        first = bundle['frame_annotations'][0]
        self.assertEqual(first['metrics']['duration_ms'], 25.0)
        self.assertAlmostEqual(first['metrics']['waits_ms'], 0.8 + 1.0 + 0.6 + 2.4, places=3)
        self.assertEqual(first['metrics']['snap'], 202)
        self.assertAlmostEqual(first['metrics']['drain_lock_ms'], 0.5, places=3)

    def test_present_metrics_follow_the_fid_shown_per_xr_frame(self):
        annotations = {a['frame']: a['metrics'] for a in self.bundle(split_eye_at=6)['frame_annotations']}
        fid = 103
        submit_end = T0 + FIDS.index(fid) * FRAME + 21 * MS
        draws = [T0 + k * XR + int(4.5 * MS) for k in range(20)]
        first = next(t for t in draws if t >= submit_end and shown_fid(t) >= fid)
        self.assertAlmostEqual(annotations[f'fid {fid}']['present_latency_ms'], (first - submit_end) / MS, places=3)
        self.assertEqual(annotations[f'fid {fid}']['shown_xr_frames'], sum(1 for t in draws if shown_fid(t) == fid))
        mismatched = [f for f, m in annotations.items() if m['eye_fid_mismatch']]
        band_end = {f'fid {f}': T0 + FIDS.index(f) * FRAME + 21 * MS for f in FIDS}
        expected = next(f for f in sorted(band_end, key=band_end.get) if band_end[f] >= draws[6])
        self.assertEqual(mismatched, [expected])

    def test_present_metrics_are_optional(self):
        metrics = self.bundle(present=False)['frame_annotations'][0]['metrics']
        self.assertNotIn('present_latency_ms', metrics)
        self.assertNotIn('shown_xr_frames', metrics)

    def test_strict_analysis_passes_and_counts_only_root_stages(self):
        path = self.base / 'bundle.json'
        path.write_text(json.dumps(self.bundle()))
        data, _ = analyzer.load_input(path)
        report = analyzer.analyze(data)
        errors = [d for d in report['diagnostics'] if d.get('severity') == 'error']
        self.assertEqual(errors, [])
        self.assertEqual(report['frames']['count'], 6)
        contract = report['token_contracts'][0]
        self.assertEqual((contract['name'], contract['status'], contract['token_count']), ('fid', 'pass', 6))
        roots = {stage for stage, stats in report['stage_stats'].items() if stats['role'] == 'root'}
        self.assertEqual(roots, {'Game thread', 'XR loop'})
        self.assertLessEqual(report['max_concurrent_stages'], 2)

    def test_missing_boundary_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'only one of its two boundaries'):
            self.bundle(drop='vr.g.endframe.drain.end_ns')

    def test_duplicate_field_inside_one_commit_is_rejected(self):
        with self.assertRaisesRegex(ValueError, '2 samples for fid 103'):
            self.bundle(duplicate='vr.g.endframe.submit.end_ns')

    def test_fid_gap_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'not consecutive'):
            self.bundle(skip_fid=104)

    def test_wrong_clock_offset_is_rejected(self):
        manifest = self.write_inputs()
        manifest['clock']['mono_minus_trace_ns'] = OFFSET - 10 * MS
        with self.assertRaisesRegex(ValueError, 'clock.mono_minus_trace_ns is wrong'):
            builder.build(manifest, self.base)

    def test_cli_refuses_to_overwrite(self):
        self.write_inputs()
        output = self.base / 'bundle.json'
        output.write_text('keep')
        sys.argv = ['build', '--manifest', str(self.base / 'manifest.json'), '--output', str(output)]
        with self.assertRaises(SystemExit):
            builder.main()
        self.assertEqual(output.read_text(), 'keep')

    def test_synthetic_delivery_through_archify(self):
        try:
            archify = delivery.find_archify(Path(os.environ['ARCHIFY_ROOT']) if 'ARCHIFY_ROOT' in os.environ else None)
        except ValueError as error:
            self.skipTest(str(error))
        path = self.base / 'bundle.json'
        path.write_text(json.dumps(self.bundle()))
        receipt = delivery.deliver(path, self.base / 'run', archify, evidence_kind='synthetic')
        self.assertEqual(receipt['frames'], 6)
        self.assertEqual(receipt['contract'], 'gamenative-stage-archify-delivery-v1')
        html = (self.base / 'run/stage.html').read_text(encoding='utf-8')
        self.assertIn('gamenative-stage-diagram', html)
        self.assertIn('XR loop', html)


if __name__ == '__main__':
    unittest.main()
