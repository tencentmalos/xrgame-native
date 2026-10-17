#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Steam compatibility tool: hand off a game ID to Android's existing container launcher.

No shell commands, credentials, or Steam-provided executable arguments cross the channel.
The Android side independently checks the installed game and account. The bridge remains alive
until the existing game session exits, so Steam does not mark Play as finished immediately.
"""
import json
import os
from pathlib import Path
import sys
import time
import uuid


def app_id(environment):
    value = environment.get("SteamAppId", "0")
    if not value.isdecimal() or not 0 < int(value) < 0x80000000:
        raise ValueError("unsupported app")
    return int(value)


def launch(directory, appid, timeout=300):
    directory = Path(directory)
    request = directory / (uuid.uuid4().hex + ".request")
    response = request.with_suffix(".result")
    temporary = request.with_suffix(".tmp")
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary.write_text(json.dumps({"appId": appid, "pid": os.getpid()}))
    os.replace(temporary, request)
    started = False
    deadline = time.monotonic() + timeout
    try:
        while True:
            if response.is_file():
                state = json.loads(response.read_text())
                if state.get("state") == "running":
                    started = True
                elif state.get("state") == "finished":
                    return 0
                elif state.get("state") == "failed":
                    return 1
            if not started and time.monotonic() >= deadline:
                return 1
            # Android can expire a canceled Steam request without launching it later.
            request.touch(exist_ok=True)
            time.sleep(.5)
    finally:
        request.unlink(missing_ok=True)
        response.unlink(missing_ok=True)
        temporary.unlink(missing_ok=True)


def translate_path(verb, value, environment):
    """The Proton path verbs used by Steam AutoCloud, without starting a second Wine server."""
    if not value or any(ord(c) < 32 for c in value):
        raise ValueError("invalid path")
    compat = environment.get("STEAM_COMPAT_DATA_PATH", "")
    if not compat or not Path(compat).is_absolute():
        raise ValueError("missing game prefix")
    prefix = (Path(compat) / "pfx").resolve()
    if not prefix.is_dir():
        raise ValueError("shared game prefix is not ready")
    if verb == "getnativepath":
        path = value.replace("\\", "/")
        if path.startswith("/"):
            return str(Path(path).resolve())
        if len(path) < 3 or path[1:3] != ":/" or not path[0].isalpha():
            raise ValueError("unsupported Windows path")
        drive = path[0].lower()
        base = prefix / "drive_c" if drive == "c" else Path("/") if drive == "z" else prefix / "dosdevices" / (drive + ":")
        if not base.is_dir():
            raise ValueError("unmapped Windows drive")
        return str((base / path[3:]).resolve())
    path = Path(value).resolve()
    # Wine always exposes its Unix namespace on Z:, while C: denotes this game's own prefix.
    drive_c = (prefix / "drive_c").resolve()
    if path.is_relative_to(drive_c):
        return "C:\\" + str(path.relative_to(drive_c)).replace("/", "\\")
    return "Z:" + str(path).replace("/", "\\")


def main(arguments, environment):
    if not arguments:
        return 2
    verb = arguments[0]
    if verb in ("getcompatpath", "getnativepath"):
        if len(arguments) != 2:
            return 2
        print(translate_path(verb, arguments[1], environment))
        return 0
    if verb == "destroyprefix":
        return 0  # Steam uninstall must never delete the existing Android container/saves.
    if verb not in ("run", "waitforexitandrun"):
        return 2
    appid = app_id(environment)
    # Do not mistake Steam's graphics probes/install-script helpers for a user game launch.
    target = Path(arguments[1].replace("\\", "/")).name.lower() if len(arguments) > 1 else ""
    if target in ("d3ddriverquery.exe", "d3ddriverquery64.exe", "iscriptevaluator.exe"):
        return 1
    home = Path(environment["HOME"])
    return launch(home.parent / "game-bridge", appid)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:], os.environ))
    except (OSError, ValueError, KeyError):
        print("SteamPSP: game handoff unavailable", file=sys.stderr)
        sys.exit(1)
