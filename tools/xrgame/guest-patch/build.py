#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build an XRGame guest patch package (patch.json) from a recipe.

Port of shadPS4 tools/guest-functions/build.py for Windows x64 games: C/C++/assembly
compiled into a freestanding x86-64 ELF, loaded by the in-game installer
(xrgame_guest_patch64.dll). Game-facing functions use the Windows x64 convention.

  build.py --recipe RECIPE.json --clang PATH/TO/clang --output DIR

Needs an LLVM toolchain (clang, ld.lld, llvm-objdump; the Android NDK's works) and
the capstone Python module.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import relocate  # noqa: E402

SDK = Path(__file__).resolve().parent / 'sdk'
SDK_IMPORTS = ('xrg_sdk_query', 'xrg_sdk_clock_ns', 'xrg_sdk_counter', 'xrg_sdk_log')
RECIPE_SCHEMA = 'xrgame.guest-patch.recipe.v1'
PACKAGE_SCHEMA = 'xrgame.guest-patch.v1'
ABI = 'x86_64-win64'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def unique(pairs):
    result = {}
    for k, v in pairs:
        if k in result:
            raise ValueError('duplicate JSON key: ' + k)
        result[k] = v
    return result


def identifier(s):
    if not isinstance(s, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,95}', s):
        raise ValueError('invalid symbol: ' + str(s))
    return s


def offset_of(value, what):
    if type(value) is not int or value < 0 or value >= 2**40:
        raise ValueError('invalid module offset: ' + what)
    return value


def ms_abi(prototype):
    """The prototype with the Windows x64 convention, for game-facing functions."""
    return '__attribute__((ms_abi)) ' + prototype


def read_elf(path):
    b = path.read_bytes()
    if len(b) < 64 or b[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected ELF64 LE')
    h = struct.unpack_from('<16sHHIQQQIHHHHHH', b)
    if h[2] != 62 or h[11] != 64 or not h[12] or h[13] >= h[12]:
        raise ValueError('expected x86-64 ELF with sections')
    sections = [struct.unpack_from('<IIQQQQIIQQ', b, h[6] + i * 64) for i in range(h[12])]

    def take(offset, size):
        if offset < 0 or size < 0 or offset + size > len(b):
            raise ValueError('truncated ELF')
        return b[offset:offset + size]
    names = take(sections[h[13]][4], sections[h[13]][5])

    def string(table, offset):
        if offset >= len(table) or b'\0' not in table[offset:]:
            raise ValueError('invalid ELF string')
        return table[offset:].split(b'\0', 1)[0].decode()
    section_names = [string(names, s[0]) for s in sections]
    symbols = {}
    for s in sections:
        if s[1] != 2:
            continue
        if s[9] != 24 or s[5] % 24 or s[6] >= len(sections):
            raise ValueError('invalid symtab')
        strings = take(sections[s[6]][4], sections[s[6]][5])
        for p in range(s[4], s[4] + s[5], 24):
            n, info, other, sh, value, size = struct.unpack('<IBBHQQ', take(p, 24))
            name = string(strings, n)
            if name:
                symbols[name] = (value, size, sh, info)
    return b, h, sections, section_names, symbols, take


def build(recipe_path, compiler, output):
    output.mkdir(parents=True, exist_ok=True)
    # A failed rebuild must not leave a previously valid deployable package.
    for name in ('patch.json', 'build.json'):
        (output / name).unlink(missing_ok=True)
    recipe = json.loads(recipe_path.read_text(encoding='utf-8'), object_pairs_hook=unique)
    if recipe['schema'] != RECIPE_SCHEMA or recipe['abi'] != ABI:
        raise ValueError('unsupported recipe/ABI')
    identifier(recipe['id'])
    if not re.fullmatch(r'[1-9][0-9]{0,9}', str(recipe['title'])):
        raise ValueError('title must be the Steam app id')
    module = recipe['module']
    if not isinstance(module, str) or not re.fullmatch(r'[^\\/:*?"<>|]{1,128}', module):
        raise ValueError('module must be a PE file name')
    if not re.fullmatch(r'[0-9a-f]{64}', recipe.get('module_sha256', '')):
        raise ValueError('module_sha256 must be the lower-case SHA-256 of the module file')
    hooks = [dict(h) for h in recipe.get('hooks', [])]
    sites = recipe.get('sites', [])
    patches = recipe.get('patches', [])
    if not isinstance(sites, list) or not isinstance(patches, list) or len(patches) > 256:
        raise ValueError('sites/patches must be lists (at most 256 patches)')
    for site in sites:
        name = identifier(site['name'])
        if site.get('mode') not in ('before', 'replace'):
            raise ValueError('site mode must be before or replace: ' + name)
        hooks.append({'name': name, 'offset': site['offset'], 'expected': site['expected'],
                      'mode': 'site-x86_64-sse', 'site_mode': site['mode'],
                      'handler': identifier(site['handler']),
                      'replacement': 'xrg_site_' + name, 'original': 'xrg_site_' + name + '_continue',
                      'prototype': 'opaque-machine-site', 'evidence': site.get('evidence')})
    if len(hooks) > 64 or not hooks and not patches:
        raise ValueError('expected 1..64 hooks/sites or at least one code patch')
    patch_names = set()
    for patch in patches:
        identifier(patch['name'])
        offset_of(patch['offset'], patch['name'])
        expected, replacement = bytes.fromhex(patch['expected']), bytes.fromhex(patch['replacement'])
        if (patch['name'] in patch_names or not patch.get('evidence') or not 1 <= len(expected) <= 256
                or len(expected) != len(replacement) or expected == replacement):
            raise ValueError('invalid code patch: ' + patch['name'])
        patch_names.add(patch['name'])
    imports = list(SDK_IMPORTS)
    prototypes = ['#include "xrg_guest.h"']
    if sites:
        prototypes.append('#include "xrg_site.h"')
    site_adapters = []
    handlers = set()
    for header in recipe.get('headers', []):
        prototypes.append('#include ' + json.dumps(str((recipe_path.parent / header).resolve()).replace('\\', '/')))
    prototypes.append('#ifdef __cplusplus\nextern "C" {\n#endif')
    names = set()
    for hook in hooks:
        for field in ('name', 'replacement', 'original'):
            identifier(hook[field])
        if hook['name'] in names or hook['original'] in imports:
            raise ValueError('duplicate hook/original')
        names.add(hook['name'])
        if hook['replacement'] == hook['original'] or hook['replacement'] in SDK_IMPORTS:
            raise ValueError('replacement/import alias')
        offset_of(hook.get('offset'), hook['name'])
        expected = bytes.fromhex(hook['expected'])
        if not hook.get('evidence') or not 5 <= len(expected) <= 256:
            raise ValueError('hook needs evidence and 5..256 expected bytes: ' + hook['name'])
        mode = hook.get('mode', 'typed')
        replace = mode == 'site-x86_64-sse' and hook['site_mode'] == 'replace'
        if replace and len(expected) > 32:
            raise ValueError('replace site covers more than 32 bytes: ' + hook['name'])
        try:
            hook['relocation'] = relocate.relocate(expected, hook['offset'], len(expected) if replace else 5)
        except relocate.RelocationError as e:
            raise ValueError(f"{hook['name']}: {e}") from None
        if replace and hook['relocation']['stolen'] != len(expected):
            raise ValueError('replace site must cover whole instructions: ' + hook['name'])
        if mode == 'site-x86_64-sse':
            handler = hook['handler']
            if handler in (hook['replacement'], hook['original']) or handler in SDK_IMPORTS:
                raise ValueError('site handler aliases adapter/import')
            if handler not in handlers:
                prototypes.append(f'void {handler}(XrgSiteContext*);')
                handlers.add(handler)
            site_adapters.append(hook)
        elif mode == 'typed':
            p = hook['prototype'].strip().rstrip(';')
            if not re.search(r'\b' + re.escape(hook['original']) + r'\s*\(', p) or len(p) > 1024:
                raise ValueError('prototype must declare original import by name')
            prototypes += [ms_abi(p) + ';',
                           ms_abi(re.sub(r'\b' + hook['original'] + r'\b', hook['replacement'], p)) + ';']
        else:
            raise ValueError('unsupported hook mode (typed or a site): ' + hook['name'])
        imports.append(hook['original'])
    bindings = recipe.get('bindings', [])
    if not isinstance(bindings, list) or len(bindings) > 128:
        raise ValueError('expected at most 128 guest bindings')
    data_bindings = set()
    assertions = []
    exports_requested = {h['replacement'] for h in hooks} | set(recipe.get('exports', []))
    if exports_requested.intersection(imports):
        raise ValueError('export/import alias')
    for binding in bindings:
        name = identifier(binding['name'])
        if name in imports or name in exports_requested or name.startswith('xrg_sdk_'):
            raise ValueError('guest binding aliases import/export')
        offset_of(binding['offset'], name)
        if not binding.get('evidence'):
            raise ValueError('guest binding requires evidence')
        if binding['kind'] == 'function':
            p = binding['prototype'].strip().rstrip(';')
            if not re.search(r'\b' + re.escape(name) + r'\s*\(', p) or len(p) > 1024:
                raise ValueError('guest function requires typed prototype')
            if not 5 <= len(bytes.fromhex(binding['expected'])) <= 256:
                raise ValueError('guest function requires 5..256 expected bytes')
            prototypes.append(ms_abi(p) + ';')
        elif binding['kind'] == 'data':
            size, align = binding['size'], binding['alignment']
            if (type(size) is not int or type(align) is not int or not 0 < size <= 2**20 or
                    not 0 < align <= 4096 or align & (align - 1) or binding['offset'] % align or
                    binding['access'] not in ('r', 'rw')):
                raise ValueError('invalid guest data geometry/access')
            t = binding['type']
            if not isinstance(t, str) or not t or len(t) > 512 or any(c in t for c in ';\n{}#='):
                raise ValueError('invalid guest data type')
            const = 'const ' if binding['access'] == 'r' else ''
            prototypes.append(f'extern {const}{t} *const {name} __attribute__((visibility("hidden")));')
            assertions += [f'XRG_BIND_ASSERT(sizeof({t}) == {size}, "{name}: size mismatch");',
                           f'XRG_BIND_ASSERT(XRG_BIND_ALIGN({t}) <= {align}, "{name}: alignment mismatch");']
            data_bindings.add(name)
        else:
            raise ValueError('unknown guest binding kind')
        imports.append(name)
    for kind in ('counters', 'logs'):
        declared = recipe.get(kind, [])
        ids, tags = set(), set()
        if not isinstance(declared, list) or len(declared) > 64:
            raise ValueError(f'at most 64 {kind}')
        for item in declared:
            if (type(item.get('id')) is not int or not 0 < item['id'] < 2**32 or item['id'] in ids
                    or identifier(item.get('name')) in tags):
                raise ValueError(f'invalid/duplicate {kind} declaration')
            ids.add(item['id'])
            tags.add(item['name'])
    prototypes.append('#ifdef __cplusplus\n}\n#define XRG_BIND_ASSERT static_assert\n#define XRG_BIND_ALIGN alignof\n'
                      '#else\n#define XRG_BIND_ASSERT _Static_assert\n#define XRG_BIND_ALIGN _Alignof\n#endif')
    prototypes += assertions + ['#undef XRG_BIND_ASSERT', '#undef XRG_BIND_ALIGN']
    header = output / 'xrg_imports.h'
    header.write_text('\n'.join(prototypes) + '\n')
    asm = []
    for name in imports:
        if name not in data_bindings:
            asm += ['.text', '.p2align 4', f'.globl {name}', f'.type {name},@function',
                    name + ':', f'jmp *xrg_import_slot_{name}(%rip)', f'.size {name}, .-{name}']
        asm += ['.section .xrg_imports,"a",@progbits', '.p2align 3',
                f'.globl xrg_import_slot_{name}', f'xrg_import_slot_{name}:']
        if name in data_bindings:
            asm += [f'.globl {name}', f'.type {name},@object', f'.size {name},8', name + ':']
        asm += ['.quad 0']
    asm += ['.section .note.GNU-stack,"",@progbits']
    (output / 'imports.S').write_text('\n'.join(asm) + '\n')
    defines = recipe.get('defines', [])
    if not isinstance(defines, list) or len(defines) > 32 or not all(
            isinstance(d, str) and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,63}(=[A-Za-z0-9_]{0,64})?', d) for d in defines):
        raise ValueError('defines must be at most 32 NAME or NAME=VALUE entries')
    sources = [(recipe_path.parent / s).resolve() for s in recipe['sources']]
    sources += [SDK / 'runtime.c', output / 'imports.S']
    if site_adapters:
        adapter = output / 'site_adapters.S'
        adapter.write_text('#include ' + json.dumps(str(SDK / 'site_adapter.S').replace('\\', '/')) + '\n' +
                           '\n'.join(f"XRG_SITE_ADAPTER {h['replacement']}, {h['handler']}, {h['original']}"
                                     for h in site_adapters) +
                           '\n.section .note.GNU-stack,"",@progbits\n')
        sources.append(adapter)
    if len(sources) > 66 or len(set(sources)) != len(sources):
        raise ValueError('duplicate/too many sources')
    commands = []

    # Sibling LLVM tools keep the compiler's executable suffix (.exe on Windows).
    def tool(name):
        return compiler.parent / (name + (compiler.suffix if compiler.suffix.lower() == '.exe' else ''))
    dependencies = {recipe_path, Path(__file__).resolve(), Path(relocate.__file__).resolve(), SDK / 'payload.ld', header}
    if site_adapters:
        dependencies.add(SDK / 'site_adapter.S')

    def run(args):
        args = list(map(str, args))
        commands.append(args)
        return subprocess.check_output(args, stderr=subprocess.STDOUT, text=True)
    objects = []
    for i, source in enumerate(sources):
        if source.suffix not in ('.c', '.cpp', '.cc', '.S', '.s'):
            raise ValueError('only C/C++/assembly sources')
        obj = output / f'{i}.o'
        dep = output / f'{i}.d'
        # SSE2 baseline: site handlers may not touch YMM state (see xrg_site.h). There are
        # no stack probes for this target: keep payload frames well below 4 KiB, as Windows
        # commits a thread stack one guard page at a time (Wine commits it up front).
        flags = ['--target=x86_64-none-elf', '-O2', '-g', '-ffreestanding', '-fPIE',
                 '-fno-builtin', '-fno-stack-protector', '-mno-red-zone', '-march=x86-64',
                 '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                 '-fvisibility=hidden', '-nostdinc', '-Wall', '-Wextra', '-Werror', '-MMD', '-MF', dep,
                 '-I', SDK / 'include', '-I', recipe_path.parent, '-I', output, *('-D' + d for d in defines)]
        if source.suffix in ('.cpp', '.cc'):
            flags += ['-std=c++20', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
                      '-include', header]
        elif source.suffix == '.c':
            flags += ['-std=c11']
            if source != SDK / 'runtime.c':
                flags += ['-include', header]
        run([compiler, *flags, '-c', source, '-o', obj])
        dependencies.add(source)
        if dep.exists():
            # Clang make dependencies with escaped spaces. The target ends at the
            # first ':' followed by whitespace, so Windows drive letters survive.
            text = re.sub(r'\\\r?\n', ' ', dep.read_text())
            text = re.split(r':(?:\s|$)', text, maxsplit=1)[1]
            dependencies.update(Path(p.replace('\\ ', ' ')) for p in re.split(r'(?<!\\)\s+', text) if p)
        objects.append(obj)
    linker = tool('ld.lld')
    elf = output / 'patch.elf'
    run([linker, '-static', '--no-undefined', '--fatal-warnings', '--emit-relocs',
         '-e', hooks[0]['replacement'] if hooks else 'memcpy', '-T', SDK / 'payload.ld', '-o', elf, *objects])
    b, h, sections, section_names, symbols, take = read_elf(elf)
    segments = []
    for i in range(h[10]):
        if h[9] != 56:
            raise ValueError('invalid program headers')
        t, flags, offset, va, _, filesz, memsz, alignment = struct.unpack('<IIQQQQQQ', take(h[5] + i * 56, 56))
        if t in (2, 3, 7):
            raise ValueError('dynamic/TLS payload unsupported')
        if t != 1 or not memsz:
            continue
        if flags not in (5, 6) or filesz > memsz or va % 4096 or va + memsz > 2**20:
            raise ValueError('invalid/oversized segment')
        blob = take(offset, filesz)
        segments.append({'offset': va, 'size': (memsz + 4095) & ~4095, 'executable': flags == 5,
                         'hex': blob.hex(), 'sha256': sha(blob)})
    if not segments or segments[0]['offset'] != 0 or not segments[0]['executable']:
        raise ValueError('missing initial code segment')
    rebase = []
    for s, name in zip(sections, section_names):
        if s[2] & 2 and s[5]:
            if s[2] & 0x400 or name.startswith(('.init', '.fini', '.ctors', '.dtors', '.eh_frame', '.gcc_except')):
                raise ValueError('TLS/constructors/unwinding are unsupported: ' + name)
        if s[1] not in (4, 9) or not sections[s[7]][2] & 2:
            continue
        if s[1] != 4 or s[9] != 24 or s[5] % 24:
            raise ValueError('bad relocation table')
        for pos in range(s[4], s[4] + s[5], 24):
            address, info, addend = struct.unpack('<QQq', take(pos, 24))
            kind = info & 0xffffffff
            if kind == 1:
                rebase.append(address)
            elif kind not in (2, 4):
                raise ValueError('unsupported relocation ' + str(kind))
    exports = {}
    for name in {h['replacement'] for h in hooks} | set(recipe.get('exports', [])):
        identifier(name)
        if name not in symbols or symbols[name][2] == 0 or symbols[name][3] & 15 != 2:
            raise ValueError('missing function export: ' + name)
        exports[name] = symbols[name][0]
    payload = {k: recipe[k] for k in ('id', 'module', 'module_sha256')}
    payload['title'] = str(recipe['title'])
    for key, limit in (('name', 96), ('description', 1024)):
        if key in recipe:
            value = recipe[key]
            if not isinstance(value, str) or not value or len(value.encode()) > limit:
                raise ValueError(f'{key} must be a non-empty string of at most {limit} bytes')
            payload[key] = value
    payload['hooks'] = hooks
    if patches:
        payload['patches'] = patches
    if bindings:
        payload['bindings'] = bindings
    payload.update(schema=PACKAGE_SCHEMA, abi=ABI, sdk_version=1, segments=segments, rebase64=rebase,
                   exports=exports,
                   imports=[{'name': name, 'slot': symbols['xrg_import_slot_' + name][0]} for name in imports],
                   counters=recipe.get('counters', []), logs=recipe.get('logs', []), elf_sha256=sha(b))
    report = {'schema': 'xrgame.guest-patch.build.v1', 'compiler': run([compiler, '--version']),
              'linker': run([linker, '--version']), 'commands': commands,
              'tool_sha256': {str(p): sha(p.read_bytes()) for p in (compiler, linker, tool('llvm-objdump'))},
              'sources': {str(p.resolve()): sha(p.read_bytes()) for p in sorted(dependencies)},
              'elf_sha256': sha(b)}
    (output / 'disassembly.txt').write_text(run([tool('llvm-objdump'), '-d', '-r', elf]))
    encoded = (json.dumps(payload, indent=2, sort_keys=True) + '\n').encode()
    report['package_sha256'] = sha(encoded)
    (output / 'patch.json').write_bytes(encoded)
    (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    print('XRGAME_GUEST_PATCH_BUILD_PASS ' + sha(encoded))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--recipe', type=Path, required=True)
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    try:
        build(a.recipe.resolve(), a.clang.resolve(), a.output.resolve())
    except (OSError, ValueError, KeyError, IndexError, TypeError, struct.error, subprocess.CalledProcessError) as e:
        for n in ('patch.json', 'build.json'):
            (a.output / n).unlink(missing_ok=True)
        print(getattr(e, 'output', '') or str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
