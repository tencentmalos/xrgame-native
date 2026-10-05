#!/usr/bin/env python3
"""Record graphics API traces with GFXReconstruct on an Android device and replay them on Windows.

P0 tool for docs/specs/xrgame-native-api-replay-v1.md. It deploys the GFXR Vulkan layer into one
container of an installed picoXr debug APK (run-as, no APK change), triggers trimmed captures by
creating a trigger file, pulls traces and replays them with gfxrecon-replay.

    mode     --serial S --container STEAM_1446780 --mode d3d12|vulkan|off [--frames 3]   (APK with GFXR)
    deploy   --serial S --container STEAM_1446780 --layer libVkLayer_gfxreconstruct.so [--frames 3]
    start / stop / status --serial S --container C
    pull     --serial S --container C --out DIR
    restore  --serial S --container C
    replay   --capture FILE.gfxr --out DIR [--gfxrecon-replay PATH] [-- extra replay arguments]
"""
import argparse
import hashlib
import json
import os
import shlex
import struct
import subprocess
import sys
import tempfile
import time

PACKAGE = 'com.tencentmalos.xrgamenative'
APP_DATA = f'/data/user/0/{PACKAGE}'
LAYER_NAME = 'VK_LAYER_LUNARG_gfxreconstruct'
LAYER_PATH = f'{APP_DATA}/files/xrgame-gfxr/libVkLayer_gfxreconstruct.so'
MANIFEST_PATH = 'files/imagefs/usr/share/vulkan/explicit_layer.d/VkLayer_gfxreconstruct.json'
CAPTURE_ROOT = 'files/imagefs/xrgame-captures'
STAGING = '/data/local/tmp'


def adb(serial, *args, binary=False, check=True, timeout=120):
    result = subprocess.run(['adb', '-s', serial, *args], capture_output=True, timeout=timeout)
    if check and result.returncode != 0:
        raise RuntimeError(f'adb {" ".join(args)} failed: {result.stderr.decode(errors="replace").strip()}')
    return result.stdout if binary else result.stdout.decode(errors='replace')


def run_as(serial, script, **kwargs):
    """Run one sh script as the app UID. The script is quoted once for the device shell."""
    return adb(serial, 'shell', f'run-as {PACKAGE} sh -c {shlex.quote(script)}', **kwargs)


def container_paths(container):
    if not container.replace('_', '').isalnum():
        raise ValueError('container must be an identifier such as STEAM_1446780')
    home = f'files/imagefs/home/xuser-{container}'
    captures = f'{CAPTURE_ROOT}/{container}'
    return {
        'config': f'{home}/.container',
        'backup': f'{captures}/container.before-api-replay',
        'captures': captures,
        'trigger': f'{APP_DATA}/{captures}/trigger',
        'file': f'{APP_DATA}/{captures}/capture.gfxr',
        'log': f'{APP_DATA}/{captures}/gfxrecon.log',
        'sidecar': f'{captures}/capture.json',
    }


def wine_processes(serial):
    uid = adb(serial, 'shell', f'stat -c %u {APP_DATA}').strip()
    rows = adb(serial, 'shell', 'ps -A -o UID,PID,ARGS').splitlines()
    return [row.strip() for row in rows[1:] if row.split()[:1] == [uid] and ('.exe' in row or 'wineserver' in row)]


def require_stopped(serial, force_stop):
    if wine_processes(serial):
        raise RuntimeError('a Wine process is running; close the game first')
    if force_stop:
        adb(serial, 'shell', f'am force-stop {PACKAGE}')
    elif adb(serial, 'shell', f'pidof {PACKAGE}', check=False).strip():
        raise RuntimeError('XRGame Native is running and may rewrite the container; pass --force-stop')


def push_text(serial, text, target):
    with tempfile.NamedTemporaryFile('w', delete=False, encoding='utf-8', newline='\n') as tmp:
        tmp.write(text)
    staged = f'{STAGING}/xrgame-api-replay-{os.getpid()}'
    try:
        adb(serial, 'push', tmp.name, staged)
        run_as(serial, f'cat {shlex.quote(staged)} > {shlex.quote(target)}')
    finally:
        os.unlink(tmp.name)
        adb(serial, 'shell', f'rm -f {shlex.quote(staged)}', check=False)


def edit_env(env_string, updates, removals=()):
    """GameNative stores container variables as space-separated KEY=VALUE tokens without spaces."""
    tokens = [t for t in env_string.split(' ') if t and t.split('=', 1)[0] not in updates and t.split('=', 1)[0] not in removals]
    for key, value in updates.items():
        if ' ' in value:
            raise ValueError(f'{key} must not contain spaces')
        tokens.append(f'{key}={value}')
    return ' '.join(tokens)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def deploy(args):
    paths = container_paths(args.container)
    require_stopped(args.serial, args.force_stop)
    raw = run_as(args.serial, f'cat {paths["config"]}')
    config = json.loads(raw)
    run_as(args.serial, f'mkdir -p files/xrgame-gfxr {shlex.quote(paths["captures"])} '
                        f'{shlex.quote(os.path.dirname(MANIFEST_PATH))} && '
                        f'if [ ! -e {paths["backup"]} ]; then cp {paths["config"]} {paths["backup"]}; fi')
    staged = f'{STAGING}/libVkLayer_gfxreconstruct.so'
    adb(args.serial, 'push', args.layer, staged, timeout=600)
    run_as(args.serial, f'cp {staged} {LAYER_PATH} && chmod 700 {LAYER_PATH}')
    adb(args.serial, 'shell', f'rm -f {staged}', check=False)
    manifest = {'file_format_version': '1.2.0', 'layer': {
        'name': LAYER_NAME, 'type': 'GLOBAL', 'library_path': LAYER_PATH, 'api_version': '1.4.0',
        'implementation_version': '1', 'description': 'GFXReconstruct capture layer (xrgame private validation)',
        'device_extensions': [{'name': 'VK_EXT_tooling_info', 'spec_version': '1',
                               'entrypoints': ['vkGetPhysicalDeviceToolPropertiesEXT']}],
        'disable_environment': {'GFXRECON_DISABLE': ''}}}
    push_text(args.serial, json.dumps(manifest, indent=2) + '\n', MANIFEST_PATH)
    updates = {
        'VK_INSTANCE_LAYERS': LAYER_NAME,
        'GFXRECON_CAPTURE_FILE': paths['file'],
        'GFXRECON_CAPTURE_TRIGGER_FILE': paths['trigger'],
        'GFXRECON_CAPTURE_TRIGGER_FRAMES': str(args.frames),
        'GFXRECON_CAPTURE_COMPRESSION_TYPE': 'ZSTD',
        'GFXRECON_MEMORY_TRACKING_MODE': args.memory_tracking,
        'GFXRECON_LOG_FILE': paths['log'],
        'GFXRECON_LOG_LEVEL': 'info',
    }
    if args.process:
        updates['GFXRECON_CAPTURE_PROCESS_NAME'] = args.process
    config['envVars'] = edit_env(config.get('envVars', ''), updates)
    push_text(args.serial, json.dumps(config, ensure_ascii=False, separators=(',', ':')), paths['config'])
    apk = adb(args.serial, 'shell', f'pm path {PACKAGE}').strip().removeprefix('package:')
    sidecar = {
        'schema': 'xrgame.api-replay.capture/1', 'createdAt': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
        'mode': 'vulkan-layer', 'container': args.container, 'frames': args.frames,
        'device': {'model': adb(args.serial, 'shell', 'getprop ro.product.model').strip(),
                   'build': adb(args.serial, 'shell', 'getprop ro.build.fingerprint').strip(),
                   'bootId': adb(args.serial, 'shell', 'cat /proc/sys/kernel/random/boot_id').strip()},
        'apk': {'sha256': adb(args.serial, 'shell', f'sha256sum {apk}').split()[0]},
        'gfxr': {'layerSha256': sha256_file(args.layer), 'source': args.gfxr_source},
        'env': updates,
    }
    push_text(args.serial, json.dumps(sidecar, indent=2) + '\n', paths['sidecar'])
    print(json.dumps(sidecar, indent=2))


def set_mode(args):
    """APK-integrated capture (XrGameApiCapture): only the XRGAME_API_CAPTURE* settings are needed."""
    paths = container_paths(args.container)
    require_stopped(args.serial, args.force_stop)
    config = json.loads(run_as(args.serial, f'cat {paths["config"]}'))
    run_as(args.serial, f'mkdir -p {shlex.quote(paths["captures"])} && '
                        f'if [ ! -e {paths["backup"]} ]; then cp {paths["config"]} {paths["backup"]}; fi')
    env = config.get('envVars', '')
    # Settings from the P0 run-as deployment would override the APK's own configuration.
    stale = {t.split('=', 1)[0] for t in env.split(' ') if t.startswith('GFXRECON_')} | {'VK_INSTANCE_LAYERS'}
    updates = {} if args.mode == 'off' else {
        'XRGAME_API_CAPTURE': args.mode, 'XRGAME_API_CAPTURE_FRAMES': str(args.frames)}
    # Without an explicit choice the APK picks the mode's default (unassisted for D3D12).
    if args.mode != 'off' and args.memory_tracking:
        updates['XRGAME_API_CAPTURE_MEMORY'] = args.memory_tracking
    removals = stale | ({'XRGAME_API_CAPTURE', 'XRGAME_API_CAPTURE_FRAMES', 'XRGAME_API_CAPTURE_MEMORY'}
                        if args.mode == 'off' else set() if args.memory_tracking else {'XRGAME_API_CAPTURE_MEMORY'})
    config['envVars'] = edit_env(env, updates, removals)
    push_text(args.serial, json.dumps(config, ensure_ascii=False, separators=(',', ':')), paths['config'])
    print(json.dumps({'container': args.container, 'envVars': config['envVars']}, ensure_ascii=False, indent=2))


def trigger(args, enable):
    paths = container_paths(args.container)
    command = f'touch {paths["trigger"]}' if enable else f'rm -f {paths["trigger"]}'
    run_as(args.serial, command)
    if enable:
        print(json.dumps({'trigger': paths['trigger'], 'processes': wine_processes(args.serial)}, indent=2))
    status(args)


def status(args):
    paths = container_paths(args.container)
    listing = run_as(args.serial, f'ls -la {paths["captures"]} 2>&1; echo; tail -n 20 {paths["log"]} 2>&1', check=False)
    print(listing)


def pull(args):
    paths = container_paths(args.container)
    os.makedirs(args.out, exist_ok=True)
    names = run_as(args.serial, f'cd {paths["captures"]} && ls', check=False).split()
    sums = []
    for name in names:
        if not (name.endswith('.gfxr') or name.endswith('.json') or name.endswith('.log')):
            continue
        target = os.path.join(args.out, name)
        data = adb(args.serial, 'exec-out', f'run-as {PACKAGE} cat {paths["captures"]}/{name}', binary=True, timeout=3600)
        remote = run_as(args.serial, f'stat -c %s {paths["captures"]}/{name}').strip()
        if str(len(data)) != remote:
            raise RuntimeError(f'{name}: pulled {len(data)} bytes, device reports {remote}')
        with open(target, 'wb') as f:
            f.write(data)
        sums.append(f'{hashlib.sha256(data).hexdigest()}  {name}')
        print(f'{name}: {len(data)} bytes')
    with open(os.path.join(args.out, 'SHA256SUMS'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(sums) + '\n')


def restore(args):
    paths = container_paths(args.container)
    require_stopped(args.serial, args.force_stop)
    run_as(args.serial, f'cp {paths["backup"]} {paths["config"]} && rm -f {paths["trigger"]}')
    print(f'restored {paths["config"]} from {paths["backup"]}; layer files are left in place but inactive')


def bmp_pixel_hash(path):
    """Hash pixel rows only, so the summary does not depend on the image encoder."""
    with open(path, 'rb') as f:
        data = f.read()
    offset, = struct.unpack_from('<I', data, 10)
    width, height = struct.unpack_from('<ii', data, 18)
    return hashlib.sha256(data[offset:]).hexdigest()[:16], width, abs(height)


def replay(args):
    os.makedirs(args.out, exist_ok=True)
    shots = os.path.join(args.out, 'frames')
    os.makedirs(shots, exist_ok=True)
    command = [args.gfxrecon_replay, '--screenshot-all', '--screenshot-format', 'bmp',
               '--screenshot-dir', shots, *args.extra, args.capture]
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True)
    elapsed = time.monotonic() - started
    with open(os.path.join(args.out, 'replay.log'), 'w', encoding='utf-8') as f:
        f.write(result.stdout + result.stderr)
    frames = sorted(name for name in os.listdir(shots) if name.lower().endswith('.bmp'))
    lines = []
    for index, name in enumerate(frames):
        digest, width, height = bmp_pixel_hash(os.path.join(shots, name))
        lines.append(f'{index} {digest} {width} {height} {name}')
    with open(os.path.join(args.out, 'frames.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + ('\n' if lines else ''))
    summary = [f'capture={args.capture}', f'capture_sha256={sha256_file(args.capture)}',
               f'replayer={args.gfxrecon_replay}', f'replayer_sha256={sha256_file(args.gfxrecon_replay)}',
               f'arguments={" ".join(args.extra)}', f'exit_code={result.returncode}',
               f'elapsed_s={elapsed:.1f}', f'frames={len(frames)}',
               f'success={str(result.returncode == 0 and bool(frames)).lower()}']
    with open(os.path.join(args.out, 'replay_summary.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(summary) + '\n')
    print('\n'.join(summary))
    return 0 if result.returncode == 0 and frames else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('mode', 'deploy', 'start', 'stop', 'status', 'pull', 'restore'):
        p = sub.add_parser(name)
        p.add_argument('--serial', required=True, help='Explicit adb device serial')
        p.add_argument('--container', required=True, help='Container id, e.g. STEAM_1446780')
        if name in ('mode', 'deploy', 'restore'):
            p.add_argument('--force-stop', action='store_true', help='Stop XRGame Native before editing the container')
        if name == 'mode':
            p.add_argument('--mode', required=True, choices=('off', 'd3d12', 'vulkan'))
        if name in ('mode', 'deploy'):
            p.add_argument('--frames', type=int, default=3, choices=range(1, 601), metavar='1..600')
            p.add_argument('--memory-tracking', default=None if name == 'mode' else 'page_guard',
                           choices=('page_guard', 'unassisted', 'assisted'))
        if name == 'deploy':
            p.add_argument('--layer', required=True, help='Stripped Android arm64 libVkLayer_gfxreconstruct.so')
            p.add_argument('--process', help='Only capture this process name (GFXRECON_CAPTURE_PROCESS_NAME)')
            p.add_argument('--gfxr-source', default='LunarG/gfxreconstruct dev 6dc9b65 + xrgame-wine-capture patches')
        if name == 'pull':
            p.add_argument('--out', required=True)
    p = sub.add_parser('replay')
    p.add_argument('--capture', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--gfxrecon-replay', default='gfxrecon-replay.exe')
    p.add_argument('extra', nargs=argparse.REMAINDER, help='Arguments after -- go to gfxrecon-replay')
    args = parser.parse_args()
    if getattr(args, 'extra', None) and args.extra[:1] == ['--']:
        args.extra = args.extra[1:]
    try:
        if args.command == 'mode':
            set_mode(args)
        elif args.command == 'deploy':
            deploy(args)
        elif args.command in ('start', 'stop'):
            trigger(args, args.command == 'start')
        elif args.command == 'status':
            status(args)
        elif args.command == 'pull':
            pull(args)
        elif args.command == 'restore':
            restore(args)
        elif args.command == 'replay':
            return replay(args)
        return 0
    except (RuntimeError, ValueError, OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f'api_replay: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
