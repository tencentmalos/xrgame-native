#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Own a software X server for the official desktop client, never for games.

Steam's VGUI requires GLX even with CEF GPU acceleration disabled. Xephyr
supplies GLX/core fonts/Render inside this userspace; the existing Android X
server carries its final image and input. Only a private Unix socket is opened.
"""
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import time


def screen_size(value):
    width, height = map(int, value.split('x'))
    if not (320 <= width <= 1920 and 320 <= height <= 1920):
        raise ValueError('Invalid client display size')
    return f'{width}x{height}'


GPU_ROOT = '/opt/steampsp-gpu'
GPU_ICD = f'{GPU_ROOT}/share/vulkan/icd.d/freedreno_icd.aarch64.json'
GPU_GL_VENDOR = 'steampsp'  # libGLX_steampsp.so.0 from build-gpu.sh


def gpu_available():
    return Path(GPU_ICD).is_file()


def gpu_environment(library_path):
    """Steam's web helper (CEF and Steam's compositor) draws with the bundled Zink and Turnip.

    Their frames go to the Android X server, stacked as their windows are in
    Xephyr, which keeps the client's windows, input and fonts. The client shim
    applies the GL vendor to the web helper; VGUI's GLX stays on llvmpipe.
    """
    return {
        'XRGAME_WEBHELPER_GL_VENDOR': GPU_GL_VENDOR,
        'VK_DRIVER_FILES': GPU_ICD,
        'VK_ICD_FILENAMES': GPU_ICD,
        'XRGAME_X11_HOST_DISPLAY': ':0',
        'LD_LIBRARY_PATH': ':'.join(filter(None, [f'{GPU_ROOT}/lib', library_path])),
    }


def client_window_ready(tree):
    # Match CEF's main window, including the sign-in screen, never VGUI's updater.
    # Do not persist window titles (which can contain user information).
    return any(int(w) >= 400 and int(h) >= 200 for w, h in re.findall(
        r'\("steamwebhelper" "steam"\)\s+(\d+)x(\d+)', tree))


def main():
    if len(sys.argv) < 2:
        return 2
    # Bounded, account-free evidence from the actual app/PRoot mount namespace.
    library = Path(os.environ['HOME']) / 'Steam/steamapps'
    try:
        manifests = list(library.glob('appmanifest_*.acf'))
        readable = sum(1 for path in manifests if path.open('rb').read(1))
        print(f'SteamPSP: shared library manifests={len(manifests)} readable={readable} '
              f'folders_bytes={(library / "libraryfolders.vdf").stat().st_size}', flush=True)
    except OSError as error:
        print(f'SteamPSP: shared library inaccessible errno={error.errno}', flush=True)
    software = dict(os.environ, LIBGL_ALWAYS_SOFTWARE="1", GALLIUM_DRIVER="llvmpipe", LP_NUM_THREADS="2")
    nested_env = dict(software, DISPLAY=":1")
    if os.environ.get('XRGAME_STEAM_GPU') == '1':
        if gpu_available():
            nested_env.update(gpu_environment(nested_env.get('LD_LIBRARY_PATH', '')))
            print('SteamPSP: GPU compositor drawing requested', flush=True)
        else:
            print('SteamPSP: GPU component missing; the client keeps software drawing', flush=True)
    if (Path(os.environ['HOME']).parent / 'library-trace').is_file():
        nested_env['XRGAME_STEAM_LIBRARY_TRACE'] = '1'
    server_env = dict(software)
    server_env.pop("LD_LIBRARY_PATH", None)  # Xorg uses the pinned userspace libraries, not Valve's copies.
    server_env.pop("XRGAME_STEAM_GPU", None)
    probe_env = dict(server_env, DISPLAY=":1")
    screen = screen_size(os.environ.get('XRGAME_STEAM_SCREEN', '1280x800'))
    children = []
    ready_file = os.environ.get('XRGAME_STEAM_READY_FILE')
    if ready_file:
        Path(ready_file).unlink(missing_ok=True)

    def interrupted(_signum, _frame):
        raise SystemExit(143)

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        # No glamor: the host server intentionally has no GLX/DRI3.
        xserver = subprocess.Popen([
            "/usr/bin/Xephyr", ":1", "-screen", screen, "-sw-cursor",
            "-no-host-grab", "-nolisten", "tcp", "-ac", "-noreset",
        ], env=server_env)
        children.append(xserver)
        ready = False
        deadline = time.monotonic() + 30
        while xserver.poll() is None and time.monotonic() < deadline:
            try:
                probe = subprocess.run(["/usr/bin/xdpyinfo", "-display", ":1"], env=probe_env,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2)
                if probe.returncode == 0:
                    ready = True
                    break
            except subprocess.TimeoutExpired:
                pass
            time.sleep(0.2)
        if not ready:
            print("SteamPSP: nested client display did not become ready", flush=True)
            return 70
        print("SteamPSP: software client display ready", flush=True)
        steam = subprocess.Popen(sys.argv[1:], env=nested_env)
        children.append(steam)
        next_probe = 0.0
        while steam.poll() is None:
            if ready_file and time.monotonic() >= next_probe:
                next_probe = time.monotonic() + 1
                try:
                    tree = subprocess.run(['/usr/bin/xwininfo', '-root', '-tree'], env=probe_env,
                                          capture_output=True, text=True, timeout=2)
                    if tree.returncode == 0 and client_window_ready(tree.stdout):
                        Path(ready_file).write_text('ready\n')
                        ready_file = None
                except subprocess.TimeoutExpired:
                    pass
            if xserver.poll() is not None:
                print("SteamPSP: nested client display exited", flush=True)
                return 71
            time.sleep(0.2)
        return steam.returncode
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
        for child in reversed(children):
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


if __name__ == "__main__":
    sys.exit(main())
