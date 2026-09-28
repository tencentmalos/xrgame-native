from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from debug_identity import Device, IdentityError, parse_stat, private_path

APP = "com.tencentmalos.xrgamenative"
BOOT = "958168a3-6dd6-4351-bffe-d04c2b227cd7"


def stat(start=987, state="S", pid=123):
    return f"{pid} (A:\\Ship\\game (test)).exe) {state} " + " ".join(["0"] * 18 + [str(start), "0"])


class FakeDevice(Device):
    def __init__(self, before=None, after=None, uids="10234 10234 10234 10234", boot_after=BOOT):
        super().__init__("test-device", APP)
        self.responses = iter([BOOT, "10234", before or stat(),
                               f"Uid:\t{uids}\nTracerPid:\t0\n",
                               after or stat(), boot_after])

    def read(self, *values):
        return next(self.responses)


class DebugIdentityTests(unittest.TestCase):
    def expected(self):
        return {"bootId": BOOT, "uid": 10234, "pid": 123, "startTimeTicks": 987}

    def test_wine_comm_with_spaces_and_parentheses(self):
        self.assertEqual(parse_stat(stat(), 123)["startTimeTicks"], 987)

    def test_reject_truncated_stat(self):
        with self.assertRaises(IdentityError):
            parse_stat("123 (game) S 0", 123)

    def test_reject_wrong_stat_pid(self):
        with self.assertRaises(IdentityError):
            parse_stat(stat(pid=124), 123)

    def test_stable_identity_allows_state_transition(self):
        value = FakeDevice(after=stat(state="R")).identity(123, self.expected())
        self.assertEqual(value["state"], "R")
        self.assertEqual(value["uid"], 10234)

    def test_pid_reused_while_reading(self):
        with self.assertRaisesRegex(IdentityError, "generation changed"):
            FakeDevice(after=stat(start=988)).identity(123)

    def test_device_reboot_same_pid_and_ticks(self):
        with self.assertRaisesRegex(IdentityError, "generation changed"):
            FakeDevice(boot_after="b942cf54-6b53-4539-99fb-90524cd3c706").identity(123)

    def test_stale_expected_identity(self):
        for key, value in [("bootId", "stale"), ("uid", 10235), ("pid", 124), ("startTimeTicks", 986)]:
            with self.subTest(key=key), self.assertRaisesRegex(IdentityError, key):
                FakeDevice().identity(123, {**self.expected(), key: value})

    def test_other_app_uid_or_effective_uid_rejected(self):
        for uids in ["10235 10235 10235 10235", "10234 0 10234 10234"]:
            with self.subTest(uids=uids), self.assertRaisesRegex(IdentityError, "UID"):
                FakeDevice(uids=uids).identity(123)

    def test_zombie_not_attachable(self):
        with self.assertRaisesRegex(IdentityError, "exited"):
            FakeDevice(after=stat(state="Z")).identity(123)

    def test_disappearance_does_not_return_partial_identity(self):
        device = FakeDevice()
        device.responses = iter([BOOT, "10234", stat()])
        with self.assertRaises(StopIteration):
            device.identity(123)

    def test_app_path_component_boundary(self):
        self.assertEqual(private_path(f"/data/user/0/{APP}/files/prefix", APP),
                         f"/data/user/0/{APP}/files/prefix")
        for path in [f"/data/user/0/{APP}.other/files", f"/data/user/0/{APP}/../other",
                     "relative/files", "/sdcard/other", f"/data/user/0/{APP}"]:
            with self.subTest(path=path), self.assertRaises(IdentityError):
                private_path(path, APP)

    def test_symlink_resolution_cannot_escape_app(self):
        device = FakeDevice()
        device.responses = iter(["/sdcard/other"])
        with self.assertRaises(IdentityError):
            device.resolved_private_path(f"/data/user/0/{APP}/files/prefix")

    def test_alias_resolution_within_app(self):
        device = FakeDevice()
        device.responses = iter([f"/data/user/0/{APP}/files/prefix"])
        self.assertEqual(device.resolved_private_path(f"/data/data/{APP}/files/prefix"),
                         f"/data/user/0/{APP}/files/prefix")


if __name__ == "__main__":
    unittest.main()
