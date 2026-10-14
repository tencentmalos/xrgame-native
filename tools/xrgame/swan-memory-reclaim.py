#!/usr/bin/env python3
"""Show a Pico headset's memory and reclaim what the kernel can drop right away.

The reclaim is `sync; echo 2 > /proc/sys/vm/drop_caches` as root. On Swan it empties the dmabuf
system-heap free pool (about 1-1.5 GiB when idle) and reclaimable slab, so a large game starts
with more free memory. See docs/validation/swan-dmabuf-pool-20261013.md.

  swan-memory-reclaim.py [--serial SERIAL] [--status]

--status only prints the snapshot. The device serial comes from --serial, ANDROID_SERIAL, or the
only attached device.
"""
import argparse
import os
import re
import subprocess
import sys

MEMINFO_FIELDS = ('MemTotal', 'MemFree', 'MemAvailable', 'Cached', 'SwapTotal', 'SwapFree', 'KReclaimable')
# Wine games run as `*.exe` processes of the app; the names cover the usual suspects.
GAME_PATTERN = r'[.]exe|wineserver'


def adb(serial, *args, check=True):
    result = subprocess.run(['adb', '-s', serial, *args], capture_output=True, text=True)
    if check and result.returncode != 0:
        sys.exit(f'adb {" ".join(args)} failed: {result.stderr.strip() or result.stdout.strip()}')
    return result.stdout


def pick_serial(requested):
    if requested:
        return requested
    if os.environ.get('ANDROID_SERIAL'):
        return os.environ['ANDROID_SERIAL']
    lines = subprocess.run(['adb', 'devices'], capture_output=True, text=True).stdout.splitlines()[1:]
    devices = [line.split()[0] for line in lines if line.strip().endswith('device')]
    if len(devices) != 1:
        sys.exit(f'Pass --serial: {len(devices)} devices attached')
    return devices[0]


def ensure_root(serial):
    if adb(serial, 'shell', 'id', '-u').strip() == '0':
        return
    adb(serial, 'root', check=False)
    adb(serial, 'wait-for-device')
    if adb(serial, 'shell', 'id', '-u').strip() != '0':
        sys.exit('adbd is not root (adb root failed); drop_caches needs root')


def snapshot(serial):
    meminfo = {}
    for line in adb(serial, 'shell', 'cat', '/proc/meminfo').splitlines():
        name, _, rest = line.partition(':')
        if name in MEMINFO_FIELDS:
            meminfo[name] = int(rest.split()[0])
    # "system:" and "qcom,system:" list the same pool, so only "system" is counted.
    pools = adb(serial, 'shell', 'cat', '/sys/kernel/dma_heap/heap_pools_kb', check=False)
    heap, pool_kb = None, 0
    for token in pools.split():
        if token.endswith(':'):
            heap = token[:-1]
        elif heap == 'system':
            match = re.fullmatch(r'order\d+-(\d+)', token)
            if match:
                pool_kb += int(match.group(1))
    meminfo['DmabufPool'] = pool_kb
    meminfo['SwapUsed'] = meminfo.get('SwapTotal', 0) - meminfo.get('SwapFree', 0)
    return meminfo


def gib(kb):
    return f'{kb / 1048576:.2f} GiB'


def print_table(before, after=None):
    rows = (('MemAvailable', 'MemAvailable'), ('MemFree', 'MemFree'), ('Cached', 'Cached'),
            ('KReclaimable', 'KReclaimable'), ('dmabuf free pool', 'DmabufPool'), ('swap used', 'SwapUsed'))
    print(f'{"":18}{"before" if after else "now":>12}' + (f'{"after":>12}{"change":>12}' if after else ''))
    for label, key in rows:
        line = f'{label:18}{gib(before[key]):>12}'
        if after:
            delta = after[key] - before[key]
            line += f'{gib(after[key]):>12}{("+" if delta >= 0 else "-") + gib(abs(delta)):>12}'
        print(line)
    print(f'{"total":18}{gib(before["MemTotal"]):>12}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--serial')
    parser.add_argument('--status', action='store_true', help='print the snapshot without reclaiming')
    args = parser.parse_args()
    serial = pick_serial(args.serial)
    ensure_root(serial)

    # One quoted command string: adb shell joins its arguments, and the device shell would read the | as a pipe.
    games = adb(serial, 'shell', f"pgrep -fl '{GAME_PATTERN}'", check=False).strip()
    if games:
        lines = games.splitlines()
        print(f'Note: {len(lines)} Wine processes are running (a game session); reclaiming only drops kernel'
              ' caches, not their memory:')
        print('  ' + '\n  '.join(lines[:8]) + ('\n  ...' if len(lines) > 8 else ''))

    before = snapshot(serial)
    if args.status:
        print_table(before)
        return
    adb(serial, 'shell', 'sync; echo 2 > /proc/sys/vm/drop_caches')
    after = snapshot(serial)
    print_table(before, after)
    print('The dmabuf pool refills in the background (about 0.5 GiB within 3 minutes when idle);'
          ' reclaim right before starting a large game.')


if __name__ == '__main__':
    main()
