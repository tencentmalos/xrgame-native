#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Steam compatibility tool: hand off a game launch to Android's existing container launcher.

The request carries the app ID, this process ID and the game command Steam issued: its
executable and working directory relative to the game's install folder, plus its arguments.
Nothing outside that folder, no shell command and no credentials cross the channel. The Android
side independently checks the installed game and account, and resolves the command inside the
game's own drive. The bridge remains alive until the existing game session exits, so Steam does
not mark Play as finished immediately.
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


MAX_REQUEST = 16384  # Android's reader accepts requests up to this size
MAX_ARGUMENTS = 64


def steam_command(arguments, environment, cwd):
    """The game command Steam issued, relative to the install folder; None outside of it.

    arguments are the compatibility tool's own: verb, executable, then the game's arguments
    (its launch configuration followed by the user's launch options).
    """
    install = environment.get("STEAM_COMPAT_INSTALL_PATH", "")
    if len(arguments) < 2 or not install or not Path(install).is_absolute():
        return None
    root = Path(install).resolve()
    executable = Path(arguments[1]).resolve()
    if executable == root or not executable.is_relative_to(root):
        return None
    game_arguments = arguments[2:]
    if len(game_arguments) > MAX_ARGUMENTS or any("\0" in value for value in game_arguments):
        return None
    work = Path(cwd).resolve()
    return {
        "executable": executable.relative_to(root).as_posix(),
        "arguments": game_arguments,
        # Steam starts the game in its working directory; outside the game, Android picks one.
        "workingDirectory": work.relative_to(root).as_posix() if work.is_relative_to(root) else "",
    }


def request_body(appid, command):
    body = {"appId": appid, "pid": os.getpid()}
    if command is not None:
        body["command"] = command
    text = json.dumps(body)
    # Too long a command falls back to the installed game's own launch configuration.
    return text if len(text.encode()) <= MAX_REQUEST else json.dumps({"appId": appid, "pid": os.getpid()})


def launch(directory, appid, timeout=300, command=None):
    directory = Path(directory)
    request = directory / (uuid.uuid4().hex + ".request")
    response = request.with_suffix(".result")
    temporary = request.with_suffix(".tmp")
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary.write_text(request_body(appid, command))
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
    return launch(home.parent / "game-bridge", appid, command=steam_command(arguments, environment, os.getcwd()))


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:], os.environ))
    except (OSError, ValueError, KeyError):
        print("SteamPSP: game handoff unavailable", file=sys.stderr)
        sys.exit(1)
