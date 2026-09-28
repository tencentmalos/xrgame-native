#!/usr/bin/env python3
"""Read a bounded Android process identity without attaching or dumping its environment."""
import argparse
import json
import pathlib
import re
import shlex
import subprocess
import uuid


class IdentityError(RuntimeError):
    pass


def parse_stat(raw, pid):
    try:
        head, fields = raw.rsplit(")", 1)
        if int(head.split("(", 1)[0]) != pid:
            raise ValueError()
        fields = fields.split()
        start = int(fields[19])  # /proc/PID/stat field 22, following comm/state.
        if start <= 0 or fields[0] not in "RSDZTWtXxKWPIN":
            raise ValueError()
        return {"startTimeTicks": start, "state": fields[0]}
    except (ValueError, IndexError):
        raise IdentityError("Invalid process stat") from None


def parse_status(raw):
    try:
        fields = dict(line.split(":", 1) for line in raw.splitlines() if ":" in line)
        uids = [int(value) for value in fields["Uid"].split()]
        tracer = int(fields["TracerPid"].strip())
        if len(uids) != 4 or min(uids) < 0 or tracer < 0:
            raise ValueError()
        return uids, tracer
    except (KeyError, ValueError):
        raise IdentityError("Invalid process status") from None


def private_path(path, app):
    value = pathlib.PurePosixPath(path)
    roots = (pathlib.PurePosixPath(f"/data/user/0/{app}"),
             pathlib.PurePosixPath(f"/data/data/{app}"))
    if not value.is_absolute() or ".." in value.parts or not any(root in value.parents for root in roots):
        raise IdentityError("Path is outside the selected app")
    return str(value)


class Device:
    def __init__(self, serial, app):
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", app):
            raise IdentityError("Invalid package name")
        self.adb = ["adb", "-s", serial]
        self.app = app

    def read(self, *values):
        return subprocess.check_output(
            self.adb + ["exec-out", shlex.join(["run-as", self.app, *values])],
            stderr=subprocess.DEVNULL, timeout=15).decode()

    def resolved_private_path(self, path):
        private_path(path, self.app)
        return private_path(self.read("readlink", "-f", path).strip(), self.app)

    def identity(self, pid, expected=None):
        if pid <= 0:
            raise IdentityError("Invalid PID")
        boot = str(uuid.UUID(self.read("cat", "/proc/sys/kernel/random/boot_id").strip()))
        app_uid = int(self.read("id", "-u").strip())
        before = parse_stat(self.read("cat", f"/proc/{pid}/stat"), pid)
        uids, tracer = parse_status(self.read("cat", f"/proc/{pid}/status"))
        after = parse_stat(self.read("cat", f"/proc/{pid}/stat"), pid)
        boot_after = str(uuid.UUID(self.read("cat", "/proc/sys/kernel/random/boot_id").strip()))
        if before["startTimeTicks"] != after["startTimeTicks"] or boot != boot_after:
            raise IdentityError("Target generation changed during inspection")
        if any(uid != app_uid for uid in uids):
            raise IdentityError("Target UID does not match the selected app")
        if after["state"] in {"Z", "X", "x"}:
            raise IdentityError("Target has exited")
        result = {"bootId": boot, "uid": app_uid, "pid": pid,
                  "startTimeTicks": after["startTimeTicks"], "state": after["state"],
                  "tracerPid": tracer}
        if expected:
            for key in ("bootId", "uid", "pid", "startTimeTicks"):
                if result[key] != expected[key]:
                    raise IdentityError(f"Target identity mismatch: {key}")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--app", default="com.tencentmalos.xrgamenative")
    parser.add_argument("--pid", required=True, type=int)
    args = parser.parse_args()
    try:
        print(json.dumps(Device(args.serial, args.app).identity(args.pid), indent=2))
    except (IdentityError, ValueError, subprocess.SubprocessError) as error:
        # Commands and process environments are never included in failure output.
        parser.exit(1, f"Identity inspection failed ({type(error).__name__})\n")


if __name__ == "__main__":
    main()
