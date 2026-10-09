import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

SCRIPTS = Path(__file__).parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import deliver_stage_diagram as delivery


def fixture(origin=0):
    ms = 1_000_000
    data = {'schema_version': 2, 'title': 'Synthetic six frames', 'time_domain': 'trace_cpu_ns',
            'window': {'start_ns': origin, 'end_ns': origin + 60 * ms},
            'frame_markers': [{'frame': i + 900, 'time_ns': origin + i * 10 * ms, 'boundary': 'end'} for i in range(7)],
            'stage_options': {'Guest': {'role': 'root'}, 'PICA': {'role': 'root'},
                              'Detail': {'role': 'detail', 'count_for_concurrency': False}}, 'intervals': []}
    for i in range(6):
        for stage, lane, start, end in [('Guest', 'Core0', 0, 6), ('Guest', 'Core1', 1, 5),
                                        ('PICA', 'Worker', 3, 7), ('Detail', 'Worker', 3, 4)]:
            data['intervals'].append({'stage': stage, 'lane': lane, 'start_ns': origin + (i * 10 + start) * ms,
                                      'end_ns': origin + (i * 10 + end) * ms,
                                      'token': str(2**60 + i), 'source': '</script><bad>& source'})
    return data


class StageDeliveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.archify = delivery.find_archify(Path(os.environ['ARCHIFY_ROOT']) if 'ARCHIFY_ROOT' in os.environ else None)
        except ValueError as error:
            raise unittest.SkipTest(str(error))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / 'bundle.json'
        self.source.write_text(json.dumps(fixture()))

    def run_delivery(self, name='result', **kwargs):
        return delivery.deliver(self.source, self.base / name, self.archify, evidence_kind='synthetic', **kwargs)

    def test_domain_data_and_stage_concurrency_survive_viewer_integration(self):
        receipt = self.run_delivery()
        report = json.loads((self.base / 'result/stage.report.json').read_text())
        self.assertEqual(report['frames']['count'], 6)
        self.assertEqual(report['max_concurrent_stages'], 2)  # Core0 + Core1 are one Guest stage.
        self.assertEqual(report['interval_count'], 24)
        self.assertEqual((self.base / 'result/stage.bundle.json').read_bytes(), self.source.read_bytes())
        svg = ET.parse(self.base / 'result/stage.svg')
        bars = [n for n in svg.iter() if n.get('class') == 'interval']
        self.assertEqual(len(bars), 24)
        for node in svg.iter():
            for attr in ['fill', 'stroke']:
                self.assertNotIn('var(', node.get(attr, ''))
            if node.tag.endswith('style'):
                self.assertNotIn('var(', node.text or '')
        self.assertEqual(bars[0].get('data-source'), '</script><bad>& source')
        self.assertEqual(bars[0].get('data-token'), str(2**60))
        self.assertEqual(receipt['validation']['archify_showcase'], 'not applicable to domain SVG')
        for name, record in receipt['artifacts'].items():
            self.assertEqual(record['sha256'], delivery.digest(self.base / 'result' / name))

    def test_palette_and_theme_change_only_presentation(self):
        geometries = []
        for palette, theme in [('studio', 'dark'), ('feishu', 'light')]:
            self.run_delivery(palette, palette=palette, theme=theme)
            tree = ET.parse(self.base / palette / 'stage.svg')
            geometries.append([{key: n.get(key) for key in ['x', 'y', 'width', 'height', 'data-stage', 'data-token']}
                               for n in tree.iter() if n.get('class') == 'interval'])
        self.assertEqual(*geometries)

    def test_huge_ns_origin_retains_clipped_original_endpoints(self):
        data = fixture(2**60)
        data['intervals'][0]['start_ns'] -= 123
        self.source.write_text(json.dumps(data))
        self.run_delivery()
        selected = json.loads((self.base / 'result/stage.selected.json').read_text())
        self.assertEqual(selected['intervals'][0]['original_start_ns'], 2**60 - 123)
        self.assertEqual(selected['intervals'][0]['start_ns'], 2**60)

    def test_invalid_palette_leaves_no_partial_delivery(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_delivery(palette='missing')
        self.assertFalse((self.base / 'result').exists())
        self.assertFalse(list(self.base.glob('.result.*')))

    def test_strict_failure_preserves_previous_artifact_in_both_entrypoints(self):
        data = fixture(); data['diagnostics'] = [{'severity': 'error', 'code': 'InvalidEvidence', 'message': 'test'}]
        self.source.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'strict stage analysis'):
            self.run_delivery()
        svg = self.base / 'existing.svg'; svg.write_text('last-good')
        result = subprocess.run([sys.executable, str(SCRIPTS / 'analyze_stage_intervals.py'), '--input',
                                 str(self.source), '--output-svg', str(svg), '--strict'], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(svg.read_text(), 'last-good')
        self.assertFalse((self.base / 'result').exists())

    def test_existing_run_is_not_replaced_and_measured_needs_identity(self):
        self.run_delivery()
        old = (self.base / 'result/delivery.receipt.json').read_bytes()
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.run_delivery()
        self.assertEqual(old, (self.base / 'result/delivery.receipt.json').read_bytes())
        with self.assertRaisesRegex(ValueError, 'identity'):
            delivery.deliver(self.source, self.base / 'measured', self.archify)

    def test_subpixel_interval_not_widened(self):
        data = fixture(); data['intervals'][0]['end_ns'] = 1
        self.source.write_text(json.dumps(data))
        self.run_delivery()
        tree = ET.parse(self.base / 'result/stage.svg')
        bar = next(n for n in tree.iter() if n.get('class') == 'interval' and n.get('data-stage') == 'Guest' and n.get('data-lane') == 'Core0')
        self.assertGreater(float(bar.get('width')), 0)
        self.assertLess(float(bar.get('width')), .001)


if __name__ == '__main__':
    unittest.main()
