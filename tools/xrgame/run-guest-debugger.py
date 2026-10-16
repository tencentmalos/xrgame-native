#!/usr/bin/env python3
"""Stage a private WineDbg and attach its loopback RSP proxy to one Wine process."""
import argparse
import hashlib
import json
import pathlib
import shlex
import subprocess
import sys
import time
import uuid

from debug_identity import Device, IdentityError

DEBUGBUS = "com.tencentmalos.xrgamenative/app.gamenative.xrgame.DebugBusService"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--app", default="com.tencentmalos.xrgamenative")
    parser.add_argument("--pid", required=True, type=int, help="Android game PID")
    parser.add_argument("--start-time-ticks", required=True, type=int)
    parser.add_argument("--boot-id", required=True, type=uuid.UUID)
    parser.add_argument("--uid", required=True, type=int)
    parser.add_argument("--evidence-dir", required=True, type=pathlib.Path,
                        help="Fresh private directory outside the repository; records failures too")
    parser.add_argument("--wine-pid", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--wine-loader", required=True, help="Absolute device path to the runtime bin/wine")
    parser.add_argument("--debugger", required=True, type=pathlib.Path)
    parser.add_argument("--port", type=int, default=41731)
    parser.add_argument("--fex-exceptions", action="store_true",
                        help="Pass first-chance exceptions other than breakpoints and single steps to guest handlers")
    parser.add_argument("--control-trace", action="store_true", help="Log Wine thread suspend/resume counts")
    parser.add_argument("--trace", action="store_true",
                        help="WINEDEBUG=err+all,+winedbg: debug events and replies")
    parser.add_argument("--experimental-software-breakpoints", action="store_true",
                        help="Experimental Z0 support (fixtures; Half-Life: Alyx on Swan); requires the "
                             "validated FEX callback and RET fixes")
    parser.add_argument("--launcher", choices=("debugbus", "run-as", "run-as-app-domain"), default="debugbus",
                        help="debugbus (default) starts WineDbg from the app process, in the game's SELinux "
                             "domain; run-as runs it as runas_app, which an enforcing device (Swan) cuts off "
                             "from the wineserver's sockets; run-as-app-domain needs adb root and the patched "
                             "run-as with --app-domain: the app's UID in the app's own domain, like debugbus")
    parser.add_argument("--run-as-path", default="/system/bin/run-as",
                        help="run-as-app-domain only: the patched run-as to use, for example a candidate "
                             "smoke-tested from /data/local/tmp before it replaces /system/bin/run-as")
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or min(args.pid, args.wine_pid, args.start_time_ticks, args.uid) <= 0:
        parser.error("Invalid port or process identity")
    if not args.wine_loader.startswith("/"):
        parser.error("--wine-loader must be absolute")
    repo = pathlib.Path(__file__).resolve().parents[2]
    evidence = args.evidence_dir.resolve()
    if evidence == repo or repo in evidence.parents:
        parser.error("--evidence-dir must be outside the repository")
    evidence.mkdir(parents=True, exist_ok=False, mode=0o700)
    device = Device(args.serial, args.app)
    adb = device.adb
    run_id = uuid.uuid4().hex
    expected = {"bootId": str(args.boot_id), "uid": args.uid, "pid": args.pid,
                "startTimeTicks": args.start_time_ticks}
    started = time.monotonic_ns()

    def event(kind, **fields):
        with (evidence / "journal.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps({"schemaVersion": 1, "runId": run_id, "event": kind,
                                     "elapsedNs": time.monotonic_ns() - started, **fields}) + "\n")

    event("requested", target=expected, winePid=args.wine_pid, port=args.port,
          winePidBinding="caller-supplied; provider verification required",
          fexExceptionPassThrough=args.fex_exceptions)

    def shell(*values, capture=False):
        command = shlex.join(["run-as", args.app, *values])
        if capture:
            return subprocess.check_output(adb + ["exec-out", command], timeout=15)
        subprocess.run(adb + ["shell", command], check=True,
                       timeout=None if values[0] == "env" else 30)

    def check_generation():
        return device.identity(args.pid, expected)

    def launch():
        return launch_checked(args, device, adb, shell, check_generation, event, run_id)

    try:
        target = check_generation()
        if target["tracerPid"]:
            raise IdentityError("Target already has a native tracer")
        event("identity-verified", target=target)
        launch()
        event("proxy-exited", cleanup="unverified; inspect controller sidecar/forward ownership")
    except (Exception, KeyboardInterrupt) as error:
        # CalledProcessError includes its command, which can contain private env values.
        event("failed", errorType=type(error).__name__,
              reason=str(error) if isinstance(error, IdentityError) else "command or proxy failed",
              cleanup="unverified")
        print(f"Guest debugger failed ({type(error).__name__}); inspect private journal", file=sys.stderr)
        return 1
    finally:
        try:
            event("target-after", target=check_generation(),
                  cleanup="not inferred from tracerPid; controller receipt still required")
        except (Exception, KeyboardInterrupt) as error:
            event("target-after-unavailable", errorType=type(error).__name__)
    return 0


def launch_checked(args, device, adb, shell, check_generation, event, run_id):

    check_generation()
    raw = shell("cat", f"/proc/{args.pid}/environ", capture=True)
    values = dict(item.decode().split("=", 1) for item in raw.split(b"\0") if b"=" in item)
    allowed = {"HOME", "USER", "TMPDIR", "NTSYNC_SHM", "DISPLAY", "PATH", "LD_LIBRARY_PATH", "WINEDLLPATH",
               "WINEPREFIX", "ANDROID_SYSVSHM_SERVER", "XDG_RUNTIME_DIR", "LD_PRELOAD",
               "EVSHIM_BASE_PATH", "PROTON_DISABLE_LSTEAMCLIENT", "WINEESYNC", "WINEFSYNC",
               "PREFIX", "FONTCONFIG_PATH", "PROTON_NO_NTSYNC"}
    env = {key: value for key, value in values.items() if key in allowed}
    prefix = env.get("WINEPREFIX", "")
    prefix = device.resolved_private_path(prefix)
    loader = device.resolved_private_path(args.wine_loader)
    env["WINEPREFIX"] = prefix
    check_generation()
    env["WINEDEBUG"] = "err+all,+winedbg" if args.trace else "-all"
    env["WINE_GDB_REMOTE_NTDLL_BREAKIN"] = "1"
    env["WINE_GDB_EXPECT_UNIX_PID"] = str(args.pid)
    if args.control_trace:
        env["WINE_GDB_CONTROL_TRACE"] = "1"
    if args.experimental_software_breakpoints:
        env["WINE_GDB_SOFTWARE_BREAKPOINTS"] = "1"
    if args.fex_exceptions:
        env["WINE_GDB_FEX_EXCEPTIONS"] = "1"
    digest = hashlib.sha256(args.debugger.read_bytes()).hexdigest()
    name = f"xrgame-winedbg-{digest[:16]}.exe"
    temp = f"/data/local/tmp/{run_id}-{name}"
    debug_dir = prefix + "/drive_c/xrgame-debugger"
    shell("mkdir", "-p", debug_dir)
    debug_dir = device.resolved_private_path(debug_dir)
    dest = device.resolved_private_path(debug_dir + "/" + name)
    try:
        subprocess.run(adb + ["push", str(args.debugger), temp], check=True, timeout=30)
        shell("cp", temp, dest)
    finally:
        subprocess.run(adb + ["shell", shlex.join(["rm", "-f", temp])], check=True, timeout=15)
    installed = shell("sha256sum", dest, capture=True).decode().split()[0]
    if installed != digest:
        raise IdentityError("Device debugger hash mismatch")
    check_generation()
    event("proxy-launch", debuggerSha256=digest, target=check_generation(),
          cleanupOwner="MCP guest controller")
    print(json.dumps({"pid": args.pid, "startTimeTicks": args.start_time_ticks,
                      "winePid": args.wine_pid, "port": args.port, "sha256": digest,
                      "fexExceptionPassThrough": args.fex_exceptions}), flush=True)
    # The MCP guest controller owns forwarding, RSP control and detach.
    if args.launcher == "run-as":
        shell("env", *[key + "=" + value for key, value in sorted(env.items())],
              "/system/bin/linker64", loader,
              "C:\\xrgame-debugger\\" + name, "--gdb", "--no-start", "--port", str(args.port),
              str(args.wine_pid))
        return
    if args.launcher == "run-as-app-domain":
        # The game's whole environment, as DebugBus uses: the loader starts through the same no-exec
        # path (WINELOADERNOEXEC, the exec shim in LD_PRELOAD). WINESERVERSOCKET is an fd the game
        # inherited; WineDbg connects to the wineserver itself.
        full = {key: value for key, value in values.items() if key != "WINESERVERSOCKET"}
        full.update({key: value for key, value in env.items() if key.startswith("WINE") or key == "WINEPREFIX"})
        command = shlex.join([args.run_as_path, args.app, "--app-domain", "env", "-i",
                              *[key + "=" + value for key, value in sorted(full.items())],
                              "/system/bin/linker64", loader,
                              "C:\\xrgame-debugger\\" + name, "--gdb", "--no-start", "--port",
                              str(args.port), str(args.wine_pid)])
        event("run-as-app-domain-started", runAs=args.run_as_path)
        try:
            subprocess.run(adb + ["shell", command], check=True)
        except KeyboardInterrupt:
            # WineDbg ignores SIGTERM; kill-on-exit is off, so the debuggee continues.
            subprocess.run(adb + ["shell", shlex.join(["pkill", "-9", "-f", name])], timeout=15)
            raise
        event("debugger-exited")
        return
    flags = ("f" if args.fex_exceptions else "") + ("c" if args.control_trace else "") + \
        ("s" if args.experimental_software_breakpoints else "") + ("d" if args.trace else "")
    reply = debugbus(adb, "guest_debugger", "start", f"target={args.pid},{args.start_time_ticks}",
                     f"wine={args.wine_pid}", f"dbg={digest[:16]},{args.port}" + (f",{flags}" if flags else ""))
    if "error" in reply:
        raise IdentityError("DebugBus guest_debugger: " + str(reply["error"]))
    event("debugbus-started", running=reply.get("running"), targetPid=reply.get("targetPid"))
    shown = 0
    try:
        while True:
            time.sleep(1)
            status = debugbus(adb, "guest_debugger", "status")
            output = shell("cat", "files/xrgame-debugger/winedbg.log", capture=True).decode(errors="replace")
            if len(output) > shown:
                sys.stdout.write(output[shown:])
                sys.stdout.flush()
                shown = len(output)
            if not status.get("running"):
                event("debugger-exited", exitCode=status.get("exitCode"))
                return
    except KeyboardInterrupt:
        debugbus(adb, "guest_debugger", "stop")
        raise


def debugbus(adb, *request):
    """One DebugBus request; returns its JSON reply (the last line starting with '{')."""
    result = subprocess.run(adb + ["shell", "dumpsys", "activity", "service", DEBUGBUS, *request],
                            capture_output=True, text=True, timeout=15, check=True)
    bodies = [line.strip() for line in result.stdout.splitlines() if line.lstrip().startswith("{")]
    if not bodies:
        raise IdentityError("DebugBus gave no reply; is the picoXr debug APK running?")
    return json.loads(bodies[-1])


if __name__ == "__main__":
    raise SystemExit(main())
