import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).parents[1]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("present_window", ROOT / "capture-present-window.py")
window = importlib.util.module_from_spec(spec)
spec.loader.exec_module(window)


class PresentWindowTest(unittest.TestCase):
    def sample(self, mono, ticks, times):
        return {"monoNs": mono, "processes": [{"pid": 7, "startTimeTicks": 5, "cpuTicks": ticks, "rssPages": 256}],
                "surfaceLatency": "16666666\n" + "\n".join(f"1 {time} 1" for time in times)}

    def test_cpu_core_fraction_and_rss_units(self):
        result = window.summarize([self.sample(1, 100, []), self.sample(2_000_000_001, 200, [])], 100, 4096)
        self.assertEqual(0.5, result["processes"][0]["cpuCoresMean"])
        self.assertEqual(1, result["processes"][0]["rssMiBMax"])

    def test_overlapping_dumps_do_not_duplicate_frames_or_join_polling_gaps(self):
        samples = [self.sample(1, 1, [10, 20, 30]), self.sample(2, 2, [20, 30, 40]),
                   self.sample(3, 3, [1000, 1010])]
        result = window.summarize(samples, 100, 4096)["surfacePresentIntervals"]
        self.assertEqual(4, result["uniqueAdjacentIntervals"])
        self.assertAlmostEqual(10 / 1e6, result["maxMs"])

    def test_pending_and_zero_fences_are_not_presentations(self):
        samples = [self.sample(1, 1, [0, 10, 20, (1 << 63) - 1]), self.sample(2, 2, [])]
        self.assertEqual(1, window.summarize(samples, 100, 4096)["surfacePresentIntervals"]["uniqueAdjacentIntervals"])

    def test_refuses_pid_reuse(self):
        samples = [self.sample(1, 1, []), self.sample(2, 2, [])]
        samples[1]["processes"][0]["startTimeTicks"] = 6
        with self.assertRaises(window.IdentityError): window.summarize(samples, 100, 4096)


if __name__ == "__main__": unittest.main()
