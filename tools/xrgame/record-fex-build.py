#!/usr/bin/env python3
"""Bind the normal FEX output to its pinned source and local fixes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--project', required=True, type=Path)
    args = parser.parse_args()
    source = args.root / 'build/fex-source'
    out = args.root / 'output/fex'
    git = ['git', '-C', str(source)]
    pin = subprocess.check_output(git + ['rev-parse', 'HEAD'], text=True).strip()
    if pin != '3f1f30a060b633980ed8e7674eb8d8997457edad':
        raise RuntimeError('Unexpected FEX source pin')
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    patches = {}
    expected_paths = set()
    for name in ['fex-wine-4k-pages.patch', 'fex-arm64ec-work-callback-guard.patch',
                 'fex-single-step-return.patch']:
        patch = args.project / 'tools/xrgame/patches' / name
        subprocess.run(git + ['apply', '--reverse', '--check', str(patch)], check=True)
        expected_paths.update(line.split('\t', 2)[2] for line in subprocess.check_output(
            git + ['apply', '--numstat', str(patch)], text=True).splitlines())
        patches[name] = digest(patch)
    changed = set(subprocess.check_output(git + ['diff', '--name-only', 'HEAD'], text=True).splitlines())
    if changed != expected_paths:
        raise RuntimeError('Unexpected files modified in the FEX build source')
    binaries = {}
    for arch, suffix in [('aarch64-windows', '.dll'), ('aarch64-unix', '.so')]:
        for name in ['libarm64ecfex', 'libwow64fex']:
            path = out / 'lib/wine' / arch / (name + suffix)
            binaries[str(path.relative_to(out))] = digest(path)
    (out / 'xrgame-fex-build.json').write_text(json.dumps({
        'sourcePin': pin, 'patches': patches,
        'sourceDiffSha256': hashlib.sha256(subprocess.check_output(git + ['diff', 'HEAD'])).hexdigest(),
        'binaries': binaries, 'releaseReady': False,
    }, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
