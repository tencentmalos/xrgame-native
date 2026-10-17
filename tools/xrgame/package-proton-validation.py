#!/usr/bin/env python3
"""Package a Proton validation build: the previous archive with one rebuilt unix library.

xrg6 = xrg5 + win32u.so rebuilt with wine-vulkan-host-image-copy.patch (dlls/win32u/vulkan.c).
xrg7 = xrg6 + ntdll.so rebuilt with wine-new-thread-sigmask.patch (dlls/ntdll/unix/thread.c).
Each patch changes one source file, so its unix library is the one changed member; every
other member (symlinks included) is copied from the base archive unchanged. The result is
a private validation component, not a release artifact.

  package-proton-validation.py --release xrg7 --base proton-11.0-2-arm64ec-xrg6.wcp \
      --library <stage>/lib/wine/aarch64-unix/ntdll.so --source <build>/proton-wine/source \
      --strip <ndk>/llvm-strip --readobj <ndk>/llvm-readobj --output DIR
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import tarfile
import tempfile

VERSION = 'proton-11.0-2-arm64ec'
RELEASES = {
    'linux-steam1': {
        'base': VERSION + '-xrg3.wcp',
        'base_sha256': '9f3382ec66f86bfa8ba7ef2b96929b916b1b2d6d4c21bd96df90965e10f66dbc',
        'member': 'lib/wine/aarch64-unix/lsteamclient.so',
        'patch': 'wine-linux-steam-client.patch',
        'source_file': 'lsteamclient/unixlib.cpp',
        'marker': b'XRGAME_STEAMCLIENT_LIBRARY',
        'validation': 'Private official Linux Steam candidate; source-built lsteamclient only, not a full Wine rebuild. See docs/validation/linux-steam-20261016.md.',
    },
    'xrg6': {
        'base': VERSION + '-xrg5.wcp',
        'base_sha256': '1adaf5bf6653e138ee7a40c8e50162db01e84f7aafef8dadccf272fd583dc315',
        'member': 'lib/wine/aarch64-unix/win32u.so',
        'patch': 'wine-vulkan-host-image-copy.patch',
        'source_file': 'dlls/win32u/vulkan.c',
        'marker': b'Enabling VK_EXT_host_image_copy on the host device',
        'validation': 'Private validation build; see docs/validation/swan-alyx-vulkan-20261015.md.',
    },
    'xrg7': {
        'base': VERSION + '-xrg6.wcp',
        'base_sha256': '2c421539badc8692175a8e9a3c2e2eee6014816c156ee5aa48cbc2d8e13435a0',
        'member': 'lib/wine/aarch64-unix/ntdll.so',
        'patch': 'wine-new-thread-sigmask.patch',
        'source_file': 'dlls/ntdll/unix/thread.c',
        # The change adds no string; the build copy must carry the patch instead.
        'marker': None,
        'validation': 'Private validation build; see docs/validation/swan-guest-debugger-20261015.md.',
    },
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def text_bytes(path):
    # Recipe hashes are of the LF form the build container uses, whatever the checkout's line endings.
    return path.read_bytes().replace(b'\r\n', b'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--release', choices=sorted(RELEASES), default='xrg7')
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--library', '--win32u', type=Path, required=True,
                        help='the rebuilt, unstripped unix library from the build stage')
    parser.add_argument('--source', type=Path,
                        help='the build copy of the Wine source; required when the patch adds no marker')
    parser.add_argument('--strip', type=Path, required=True)
    parser.add_argument('--readobj', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    release = RELEASES[args.release]
    BASE, MEMBER, PATCH = release['base'], release['member'], release['patch']
    BASE_SHA256 = release['base_sha256']
    FILE = VERSION + '-' + args.release + '.wcp'
    if sha(args.base.read_bytes()) != BASE_SHA256:
        raise SystemExit('Expected the validated Proton archive ' + BASE)
    if release['marker'] is None:
        if not args.source:
            raise SystemExit('--source is required for ' + args.release)
        # The build copy has the patch applied, and the library was linked after that file changed.
        subprocess.run(['git', 'apply', '--reverse', '--check', str(here / 'patches' / PATCH)],
                       cwd=args.source, check=True)
        if args.library.stat().st_mtime < (args.source / release['source_file']).stat().st_mtime:
            raise SystemExit(Path(MEMBER).name + ' is older than ' + release['source_file'])
    if args.release == 'linux-steam1':
        built = json.loads((args.library.parent / 'wine-client-build.json').read_text())
        if (built['wine_bundle_sha256'] != BASE_SHA256 or built['sha256'] != sha(args.library.read_bytes())
                or built['patch_sha256'] != sha(text_bytes(here / 'patches' / PATCH))
                or built['wine_pin'] != '5d0d333e7beef02fbb455cc3163b4e8e25615458'):
            raise SystemExit('Official Steam adapter does not match its recorded inputs')
    with tempfile.TemporaryDirectory() as temporary:
        # Strip like package-components.py does for every ELF in the component.
        library = Path(temporary) / Path(MEMBER).name
        shutil.copy2(args.library, library)
        subprocess.run([str(args.strip), '--strip-debug', str(library)], check=True)
        data = library.read_bytes()
        if release['marker'] is not None and release['marker'] not in data:
            raise SystemExit(Path(MEMBER).name + ' was not built with ' + PATCH)
        headers = subprocess.check_output([str(args.readobj), '--file-headers', '--notes', str(library)],
                                          text=True).replace(str(library), './' + MEMBER)
    with tarfile.open(args.base) as source:
        members = [(m, source.extractfile(m).read() if m.isfile() else None) for m in source]
    names = {m.name.removeprefix('./'): i for i, (m, _) in enumerate(members)}
    if MEMBER not in names or 'xrgame-build.json' not in names:
        raise SystemExit('Unexpected Proton archive layout')
    inventory = json.loads(members[names['xrgame-build.json']][1])
    if inventory['sourcePins']['proton-wine'] != '5d0d333e7beef02fbb455cc3163b4e8e25615458':
        raise SystemExit('Wrong Proton source pin')
    old = members[names[MEMBER]]
    if old[1] == data:
        raise SystemExit(Path(MEMBER).name + ' is unchanged from ' + BASE)
    members[names[MEMBER]] = (old[0], data)
    entry = dict(inventory['files'][MEMBER], sha256=sha(data), size=len(data))
    if 'objectHeaders' in entry:
        entry['objectHeaders'] = headers
    inventory['files'][MEMBER] = entry
    for name in ('patches/' + PATCH, 'build-wine.sh', Path(__file__).name):
        inventory['recipes']['tools/xrgame/' + name] = sha(text_bytes(here / name))
    if args.release == 'linux-steam1':
        inventory['recipes']['tools/xrgame/linux-steam/build-wine-client.py'] = sha(text_bytes(here / 'linux-steam/build-wine-client.py'))
    inventory['derivedFrom'] = {'file': BASE, 'patch': 'tools/xrgame/patches/' + PATCH, 'rebuilt': [MEMBER],
                                'sha256': BASE_SHA256}
    inventory['validation'] = release['validation']
    blob = (json.dumps(inventory, indent=2, sort_keys=True) + '\n').encode()
    info = members[names['xrgame-build.json']][0]
    members[names['xrgame-build.json']] = (info, blob)
    args.output.mkdir(parents=True, exist_ok=True)
    dest = args.output / FILE
    partial = dest.with_suffix('.wcp.part')
    inspect = runpy.run_path(str(here / 'verify-runtime-packages.py'))['inspect']
    try:
        with tarfile.open(partial, 'w:xz', preset=3, format=tarfile.GNU_FORMAT) as out:
            for member, payload in members:
                member = member.replace()  # a copy with its own size
                if payload is not None:
                    member.size = len(payload)
                    out.addfile(member, io.BytesIO(payload))
                else:
                    out.addfile(member)
        inspect(partial, 'proton')
        partial.replace(dest)
    finally:
        partial.unlink(missing_ok=True)
    result = {'file': dest.name, 'id': VERSION, 'sha256': sha(dest.read_bytes()), 'size': dest.stat().st_size,
              'member': MEMBER, 'memberSha256': sha(data)}
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
