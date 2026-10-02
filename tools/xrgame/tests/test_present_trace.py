import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("present_trace", pathlib.Path(__file__).parents[1] / "analyze-present-trace.py")
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


class PresentTraceTest(unittest.TestCase):
    def lines(self):
        identity = " frame=1 generation=7 window=8 pixmap=9 serial=10"
        return [
            "XRGamePresentTrace event=receive mono_ns=100 wait_fence=0" + identity,
            "XRGamePresentTrace event=copy_done frame=1 mono_ns=160 start_ns=110 locked_ns=120 submitted_ns=130 fence_done_ns=150 result=0",
            "XRGamePresentTrace event=complete mono_ns=170" + identity,
            "XRGamePresentTrace event=idle mono_ns=180" + identity,
        ]

    def test_complete_timeline_keeps_wall_and_fence_wait_separate(self):
        result = trace.analyze(self.lines())
        self.assertEqual(1, result["completeFrames"])
        self.assertEqual(50, result["summary"]["copy_wall_ns"]["mean"])
        self.assertEqual(20, result["summary"]["fence_wait_ns"]["mean"])
        self.assertEqual([], result["diagnostics"])

    def test_rejects_idle_before_gpu_read_finishes(self):
        lines = self.lines()
        lines[-1] = lines[-1].replace("180", "140")
        result = trace.analyze(lines)
        self.assertEqual(0, result["completeFrames"])
        self.assertEqual(1, len(result["diagnostics"]))

    def test_truncated_capture_is_reported_not_fabricated(self):
        result = trace.analyze(self.lines()[:-1])
        self.assertEqual(0, result["completeFrames"])
        self.assertEqual(["idle"], result["incomplete"][0]["missing"])

    def test_sample_requires_retirement_proof_before_idle(self):
        lines = self.lines()
        lines[1] = "XRGamePresentTrace event=sample_ready frame=1 mono_ns=160"
        lines[2] += " mode=sample"
        self.assertEqual(["sample_retired"], trace.analyze(lines)["incomplete"][0]["missing"])
        lines.insert(3, "XRGamePresentTrace event=sample_retired frame=1 mono_ns=175")
        result = trace.analyze(lines)
        self.assertEqual(1, result["completeFrames"])
        self.assertEqual(15, result["summary"]["lease_ns"]["mean"])
        lines[3] = lines[3].replace("175", "190")
        self.assertEqual(1, len(trace.analyze(lines)["diagnostics"]))

    def test_reused_id_cannot_mix_generations(self):
        lines = self.lines()
        lines[-1] = lines[-1].replace("generation=7", "generation=8")
        self.assertEqual(1, len(trace.analyze(lines)["diagnostics"]))

    def test_async_dequeue_can_precede_admission_return(self):
        lines = self.lines()
        lines[2] += " mode=async"
        lines += ["XRGamePresentTrace event=copy_dequeue frame=1 mono_ns=105",
                  "XRGamePresentTrace event=enqueue_return frame=1 mono_ns=112"]
        result = trace.analyze(lines)
        self.assertEqual([], result["diagnostics"])
        self.assertEqual(12, result["summary"]["receive_to_enqueue_return_ns"]["mean"])
        self.assertEqual(5, result["summary"]["receive_to_dequeue_ns"]["mean"])
        lines[3] = lines[3].replace("180", "140")
        self.assertEqual(0, trace.analyze(lines)["completeFrames"])


if __name__ == "__main__":
    unittest.main()
