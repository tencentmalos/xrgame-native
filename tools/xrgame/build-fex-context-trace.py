#!/usr/bin/env python3
"""Build a private ARM64EC context diagnostic, then restore the cached source.

Requires the ordinary pinned build-fex.sh output. Do not run concurrently with
that build. The diagnostic DLL is never installed or packaged by this script.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['context-trace', 'work-guard'], default='context-trace')
    parser.add_argument('--extra-patch', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    base = Path(os.environ.get('XRGAME_BUILD_ROOT', '/work'))
    project = Path(os.environ.get('XRGAME_PROJECT_ROOT', base / 'project'))
    source = base / 'build/fex-source'
    build = base / 'build/fex-wine-arm64ec'
    patch_name = ('fex-arm64ec-context-trace.patch' if args.mode == 'context-trace'
                  else 'fex-arm64ec-work-callback-guard.patch')
    patch = project / 'tools/xrgame/patches' / patch_name
    patches = [patch] + ([args.extra_patch.resolve()] if args.extra_patch else [])
    pin = '3f1f30a060b633980ed8e7674eb8d8997457edad'
    git = ['git', '-C', str(source)]
    if subprocess.check_output(git + ['rev-parse', 'HEAD'], text=True).strip() != pin:
        raise RuntimeError('Unexpected FEX source pin')
    # Refuse unrelated cached-source edits, including an interrupted older trace build.
    changed = subprocess.check_output(git + ['diff', '--name-only', 'HEAD'], text=True).splitlines()
    if changed != ['FEXCore/include/FEXCore/Utils/TypeDefines.h']:
        raise RuntimeError('Expected only the existing 4 KiB Wine patch')
    subprocess.run(git + ['apply', '--reverse', '--check',
                         str(project / 'tools/xrgame/patches/fex-wine-4k-pages.patch')], check=True)
    changed_paths = set()
    for item in patches:
        changed_paths.update(line.split('\t', 2)[2] for line in subprocess.check_output(
            git + ['apply', '--numstat', str(item)], text=True).splitlines())
    before = {name: (source / name).read_bytes() for name in changed_paths}
    base_diff = subprocess.check_output(git + ['diff', 'HEAD'])
    out = args.output or base / ('output/fex-' + args.mode)
    out.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    try:
        for item in patches:
            subprocess.run(git + ['apply', '--check', str(item)], check=True)
            subprocess.run(git + ['apply', str(item)], check=True)
        subprocess.run(['cmake', '--build', str(build), '--target', 'arm64ecfex',
                        '--parallel', os.environ.get('XRGAME_JOBS', '12')], check=True)
        dll = build / 'Bin/libarm64ecfex.dll'
        shutil.copy2(dll, out / dll.name)
        (out / 'build.json').write_text(json.dumps({
            'sourcePin': pin, 'baseDiffSha256': hashlib.sha256(base_diff).hexdigest(),
            'patchSha256': sha(patch), 'patches': {p.name: sha(p) for p in patches},
            'recipeSha256': sha(Path(__file__)),
            'cmakeCacheSha256': sha(build / 'CMakeCache.txt'),
            'dllSha256': sha(out / dll.name), 'purpose': 'private-validation-' + args.mode,
            'releaseReady': False,
        }, indent=2) + '\n')
        for item in patches:
            shutil.copy2(item, out / item.name)
        print((out / 'build.json').read_text())
    finally:
        for name, content in before.items():
            (source / name).write_bytes(content)
        # Its new mtime makes the normal build recompile the restored source.
        if subprocess.check_output(git + ['diff', 'HEAD']) != base_diff:
            raise RuntimeError('Cached source restoration did not match its initial diff')


if __name__ == '__main__':
    main()
