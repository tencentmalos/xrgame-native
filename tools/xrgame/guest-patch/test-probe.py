#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end check of the guest patch chain on a Windows x64 host (natively).

Builds the installer DLL and the probe fixture with an LLVM toolchain that targets
x86_64-w64-windows-gnu (the Android NDK's works), generates the fixture's recipe from
the built probe.exe, builds the package, then runs probe.exe twice: unpatched, and with
the installer loaded (XRGAME_GUEST_PATCH_SELFLOAD) while a control line disables one hook.

  test-probe.py --llvm-bin NDK/toolchains/llvm/prebuilt/windows-x86_64/bin --output DIR
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
FIXTURE = ROOT / 'tools/xrgame/fixtures/guest-patch-probe'
INSTALLER = ROOT / 'app/src/main/windows/guest_patch'
APP_ID = '1'
FIXTURE_IMPORTS = ['GetStdHandle', 'WriteFile', 'LoadLibraryW', 'GetEnvironmentVariableW', 'Sleep', 'ExitProcess']
SYMBOLS = ['probe_add', 'probe_counted', 'probe_read', 'probe_before_site', 'probe_replace_site',
           'probe_constant', 'probe_triple', 'probe_value']


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True, **kwargs).stdout


def exe(bin_dir, name):
    path = bin_dir / (name + '.exe')
    return path if path.exists() else bin_dir / name


def build_installer(bin_dir, out):
    for lib in ('kernel32', 'ntdll', 'bcrypt'):
        run([exe(bin_dir, 'llvm-dlltool'), '-m', 'i386:x86-64', '-d', INSTALLER / f'{lib}_x64.def',
             '-l', out / f'lib{lib}_x64.a'])
    dll = out / 'xrgame_guest_patch64.dll'
    run([exe(bin_dir, 'clang'), '--target=x86_64-w64-windows-gnu', '-shared', '-nostdlib', '-O2', '-Wall', '-Wextra',
         '-Werror', '-mno-stack-arg-probe', '-Wl,-e,DllMain', '-o', dll, INSTALLER / 'xrgame_guest_patch.c',
         out / 'libkernel32_x64.a', out / 'libntdll_x64.a', out / 'libbcrypt_x64.a'])
    return dll


def build_fixture(bin_dir, out):
    (out / 'fixture_kernel32.def').write_text('LIBRARY KERNEL32.dll\nEXPORTS\n' +
                                              ''.join(f'    {n}\n' for n in FIXTURE_IMPORTS))
    run([exe(bin_dir, 'llvm-dlltool'), '-m', 'i386:x86-64', '-d', out / 'fixture_kernel32.def',
         '-l', out / 'libfixture_kernel32.a'])
    probe = out / 'probe.exe'
    run([exe(bin_dir, 'clang'), '--target=x86_64-w64-windows-gnu', '-nostdlib', '-O1', '-Wall', '-Wextra', '-Werror',
         '-mno-stack-arg-probe', '-Wl,-e,start', '-Wl,--subsystem,console', f'-Wl,-Map={out / "probe.map"}',
         '-o', probe, FIXTURE / 'probe_main.c', FIXTURE / 'probe.S', out / 'libfixture_kernel32.a'])
    return probe


def pe_sections(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    count = struct.unpack_from('<H', data, pe + 6)[0]
    optional = struct.unpack_from('<H', data, pe + 20)[0]
    image_base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    sections = []
    for i in range(count):
        s = pe + 24 + optional + 40 * i
        vsize, rva, raw_size, raw = struct.unpack_from('<IIII', data, s + 8)
        sections.append((rva, max(vsize, raw_size), raw))
    return image_base, sections


def read_rva(data, sections, rva, size):
    for start, extent, raw in sections:
        if start <= rva and rva + size <= start + extent:
            return data[raw + rva - start: raw + rva - start + size]
    raise ValueError('RVA outside sections: %#x' % rva)


def symbols(map_text):
    """Symbol RVAs from an lld COFF map (its addresses are RVAs)."""
    found = {}
    for line in map_text.splitlines():
        m = re.match(r'\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+\d+\s+(\S+)\s*$', line)
        if m and m.group(2) in SYMBOLS:
            found[m.group(2)] = int(m.group(1), 16)
    missing = set(SYMBOLS) - set(found)
    if missing:
        raise ValueError('symbols missing from the map: ' + ', '.join(sorted(missing)))
    return found


def recipe(probe, out):
    data = probe.read_bytes()
    _, sections = pe_sections(data)
    rva = symbols((out / 'probe.map').read_text())

    def code(name, size):
        return read_rva(data, sections, rva[name], size).hex()

    def hook(name, original, target, size, prototype):
        return {'name': name, 'offset': rva[target], 'expected': code(target, size),
                'replacement': 'patched_' + name, 'original': original, 'prototype': prototype,
                'evidence': f'fixture tools/xrgame/fixtures/guest-patch-probe/probe.S: {target}'}
    return {
        'schema': 'xrgame.guest-patch.recipe.v1', 'abi': 'x86_64-win64', 'id': 'probe_fixture',
        'name': 'Probe fixture', 'description': 'Exercises hooks, sites, code patches and bindings.',
        'title': APP_ID, 'module': 'probe.exe', 'module_sha256': hashlib.sha256(data).hexdigest(),
        'sources': [str(FIXTURE / 'patch.cpp')],
        'hooks': [hook('add', 'original_add', 'probe_add', 7, 'int original_add(int a, int b)'),
                  hook('counted', 'original_counted', 'probe_counted', 9, 'int original_counted(void)'),
                  hook('read', 'original_read', 'probe_read', 7, 'int original_read(void)')],
        'sites': [{'name': 'before_add_site', 'offset': rva['probe_before_site'], 'expected': code('probe_before_site', 6),
                   'mode': 'before', 'handler': 'before_add', 'evidence': 'probe.S: add eax, 100; imul eax, eax, 3'},
                  {'name': 'replace_imul_site', 'offset': rva['probe_replace_site'],
                   'expected': code('probe_replace_site', 6), 'mode': 'replace', 'handler': 'replace_imul',
                   'evidence': 'probe.S: imul eax, eax, 65536'}],
        'patches': [{'name': 'constant', 'offset': rva['probe_constant'], 'expected': code('probe_constant', 5),
                     'replacement': 'b82a000000', 'evidence': 'probe.S: mov eax, 7 -> mov eax, 42'}],
        'bindings': [{'name': 'bound_triple', 'kind': 'function', 'offset': rva['probe_triple'],
                      'expected': code('probe_triple', 5), 'prototype': 'int bound_triple(int a)',
                      'evidence': 'probe.S: lea eax, [rcx + rcx * 2]'},
                     {'name': 'bound_value', 'kind': 'data', 'offset': rva['probe_value'], 'size': 4, 'alignment': 4,
                      'access': 'r', 'type': 'int', 'evidence': 'probe.S: probe_value .long 5'}],
        'counters': [{'id': 1, 'name': 'add_calls'}],
        'logs': [{'id': 1, 'name': 'counted'}],
    }


def parse(stdout):
    rounds = []
    for line in stdout.splitlines():
        rounds.append(dict(re.findall(r'(\w+)=(-?\d+)', line)))
    return rounds


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--llvm-bin', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    dll = build_installer(a.llvm_bin, out)
    probe = build_fixture(a.llvm_bin, out)
    (out / 'recipe.json').write_text(json.dumps(recipe(probe, out), indent=2))
    build = subprocess.run([sys.executable, HERE / 'build.py', '--recipe', out / 'recipe.json',
                            '--clang', exe(a.llvm_bin, 'clang'), '--output', out / 'package'],
                           capture_output=True, text=True)
    print(build.stdout.strip() or build.stderr.strip())
    if build.returncode:
        return 1
    state = out / 'state'
    (state / 'packages').mkdir(parents=True, exist_ok=True)
    for f in state.glob('*'):
        if f.is_file():
            f.unlink()
    for f in (state / 'packages').glob('*'):
        f.unlink()
    (state / 'packages' / '01-probe.json').write_bytes((out / 'package' / 'patch.json').read_bytes())

    plain = parse(run([probe]))
    print('unpatched:', plain)
    env = dict(__import__('os').environ, XRGAME_GUEST_PATCH_DIR=str(state), XRGAME_GUEST_PATCH_APP=APP_ID,
               XRGAME_GUEST_PATCH_SELFLOAD=str(dll))
    process = subprocess.Popen([str(probe)], env=env, stdout=subprocess.PIPE, text=True)
    time.sleep(0.7)
    (state / 'control.txt').write_text('1 disable probe_fixture/add\n')
    stdout, _ = process.communicate(timeout=30)
    patched = parse(stdout)
    print('patched:  ', patched)
    status = (state / 'status.txt').read_text() if (state / 'status.txt').exists() else ''
    print(status)
    expected = [{'round': '0', 'add': '3005', 'counted': '10', 'read': '105', 'before': '318', 'replace': '6',
                 'constant': '42'},
                {'round': '1', 'add': '5', 'counted': '20', 'read': '105', 'before': '318', 'replace': '6',
                 'constant': '42'}]
    checks = {
        'unpatched': plain == [{'round': '0', 'add': '5', 'counted': '1', 'read': '5', 'before': '303',
                                'replace': '196608', 'constant': '7'},
                               {'round': '1', 'add': '5', 'counted': '2', 'read': '5', 'before': '303',
                                'replace': '196608', 'constant': '7'}],
        'patched': patched == expected,
        'installed': 'installed: 1' in status,
        'switched': 'switches: 1' in status and 'hook: add' in status and re.search(r'hook: add .* enabled=0', status),
        'counter': re.search(r'counter: 1 add_calls samples=1 last=2', status) is not None,
        # The status is rewritten every 250 ms: round 1's sample may land after the last write.
        'log': re.search(r'log_tag: 1 counted samples=(1 last=1|2 last=2)', status) is not None
               and 'tag=counted value=1' in (state / 'guest-patch.log').read_text(),
    }
    for name, ok in checks.items():
        print(('PASS ' if ok else 'FAIL ') + name)
    return 0 if all(checks.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
