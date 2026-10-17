#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build only Bionic lsteamclient against our existing Wine; no graphics/runtime replacement."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

REPO = Path(__file__).resolve().parents[3]
PIN = '5d0d333e7beef02fbb455cc3163b4e8e25615458'
BASE_SHA256 = '9f3382ec66f86bfa8ba7ef2b96929b916b1b2d6d4c21bd96df90965e10f66dbc'


def run(args, **kwargs):
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--base', type=Path, default=REPO / 'build/xrgame-runtime/bundle/components/proton-11.0-2-arm64ec-xrg3.wcp',
                        help='verified xrg3 baseline, independent of the currently staged catalog')
    args = parser.parse_args()
    wcp = args.base
    if hashlib.sha256(wcp.read_bytes()).hexdigest() != BASE_SHA256:
        raise SystemExit('Expected the validated xrg3 Wine baseline')
    out = args.output.resolve()
    if out.exists(): raise SystemExit('Use a new output directory')
    out.mkdir(parents=True)
    source = out / 'source'
    source.mkdir()
    archive = out / 'wine-source.tar'
    with archive.open('wb') as stream:
        run(['git', '-C', REPO / 'references/proton-wine', 'archive', PIN, 'lsteamclient', 'include'], stdout=stream)
    run(['tar', '-xf', archive, '-C', source])
    patch = REPO / 'tools/xrgame/patches/wine-linux-steam-client.patch'
    run(['git', 'apply', '--check', patch], cwd=source)
    run(['git', 'apply', patch], cwd=source)
    # keysym.h/keysymdef.h contain architecture-independent X11 protocol constants;
    # never add the host's whole /usr/include to an Android compiler's search path.
    headers = out / 'headers/X11'
    headers.mkdir(parents=True)
    for name in ('keysym.h', 'keysymdef.h'):
        shutil.copyfile(Path('/usr/include/X11') / name, headers / name)
    # This is a build-time ABI/link input only, extracted from our verified bundle.
    with tarfile.open(wcp, 'r:xz') as archive:
        members = [m for m in archive if m.name.lstrip('./') == 'lib/wine/aarch64-unix/ntdll.so']
        if len(members) != 1 or not members[0].isfile(): raise SystemExit('Missing Wine Unix ABI')
        (out / 'ntdll.so').write_bytes(archive.extractfile(members[0]).read())
    tc = Path(os.environ['ANDROID_NDK_HOME']) / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    compiler = tc / 'aarch64-linux-android29-clang++'
    flags = ['-O2', '-g', '-fPIC', '-std=gnu++17', '-pthread', '-Wno-pragma-pack', '-Wno-format-extra-args',
             '-D__WINESRC__', '-DWINE_UNIX_LIB', '-DSTEAM_API_EXPORTS', '-Dprivate=public', '-Dprotected=public',
             '-I' + str(out / 'headers'), '-I' + str(source / 'include'), '-I' + str(source / 'lsteamclient')]
    objects = out / 'objects'
    objects.mkdir()
    def compile(path):
        obj = objects / (path.name + '.o')
        with (objects / (path.name + '.log')).open('w') as log:
            run([compiler, *flags, '-c', path, '-o', obj], stdout=log, stderr=subprocess.STDOUT)
        return obj
    paths = sorted((source / 'lsteamclient').glob('*.cpp'))
    with ThreadPoolExecutor(max_workers=int(os.environ.get('XRGAME_JOBS', '4'))) as workers:
        compiled = list(workers.map(compile, paths))
    run([compiler, '-shared', '-static-libstdc++', '-pthread', '-Wl,-soname,lsteamclient.so',
         '-Wl,--no-undefined', '-Wl,--build-id=sha1', '-o', out / 'lsteamclient.so',
         *compiled, '-L' + str(out), '-l:ntdll.so', '-ldl'])
    run([tc / 'llvm-readelf', '-d', out / 'lsteamclient.so'])
    manifest = dict(schema=1, wine_pin=PIN, wine_bundle_sha256=BASE_SHA256,
                    patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                    sha256=hashlib.sha256((out / 'lsteamclient.so').read_bytes()).hexdigest())
    (out / 'wine-client-build.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__': main()
