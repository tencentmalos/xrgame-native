#!/usr/bin/env python3
"""Package a Proton validation build: the previous archive with one rebuilt unix library.

xrg6 = xrg5 + win32u.so rebuilt with wine-vulkan-host-image-copy.patch (dlls/win32u/vulkan.c).
xrg7 = xrg6 + ntdll.so rebuilt with wine-new-thread-sigmask.patch (dlls/ntdll/unix/thread.c).
Each patch changes one source file, so its unix library is the one changed member; every
other member (symlinks included) is copied from the base archive unchanged. The result is
a private validation component, not a release artifact.

linux-steam2 = xrg7 + lsteamclient with the Steamworks SDK 1.65 interfaces (lsteamclient-sdk165.patch):
the unix lsteamclient.so from linux-steam/build-wine-client.py (which also applies
wine-linux-steam-client.patch, as linux-steam1 did on xrg3) and both PE lsteamclient.dll modules
from build-lsteamclient-pe.sh. The PE and unix sides share the unix call table, so they change together.

  package-proton-validation.py --release xrg7 --base proton-11.0-2-arm64ec-xrg6.wcp \
      --library <stage>/lib/wine/aarch64-unix/ntdll.so --source <build>/proton-wine/source \
      --strip <ndk>/llvm-strip --readobj <ndk>/llvm-readobj --output DIR
  package-proton-validation.py --release linux-steam2 --base proton-11.0-2-arm64ec-xrg7.wcp \
      --library <build-wine-client output>/lsteamclient.so --pe <build-lsteamclient-pe output> \
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
    'linux-steam2': {
        'base': VERSION + '-xrg7.wcp',
        'base_sha256': 'a0de997e03ac0c07caaf94042838b61618fd2562c6f484a16cfec196e6568a38',
        'member': 'lib/wine/aarch64-unix/lsteamclient.so',
        'patch': 'wine-linux-steam-client.patch',
        'sdk_patch': 'lsteamclient-sdk165.patch',
        'pe_members': ['lib/wine/aarch64-windows/lsteamclient.dll', 'lib/wine/i386-windows/lsteamclient.dll'],
        'source_file': 'lsteamclient/unixlib.cpp',
        'marker': b'XRGAME_STEAMCLIENT_LIBRARY',
        'validation': 'Private official Linux Steam candidate: xrg7 with source-built lsteamclient (SDK 1.65 interfaces), not a full Wine rebuild. See docs/validation/swan-official-steam-20261019.md.',
    },
}
# build-wine-client.py links lsteamclient.so against xrg3's ntdll.so ABI, whatever archive it goes into.
CLIENT_ABI_SHA256 = RELEASES['linux-steam1']['base_sha256']


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
    parser.add_argument('--pe', type=Path,
                        help='build-lsteamclient-pe.sh output (lsteamclient-pe-build.json and modules); linux-steam2 only')
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
    if args.release.startswith('linux-steam'):
        built = json.loads((args.library.parent / 'wine-client-build.json').read_text())
        if (built['wine_bundle_sha256'] != CLIENT_ABI_SHA256 or built['sha256'] != sha(args.library.read_bytes())
                or built['patch_sha256'] != sha(text_bytes(here / 'patches' / PATCH))
                or built['wine_pin'] != '5d0d333e7beef02fbb455cc3163b4e8e25615458'):
            raise SystemExit('Official Steam adapter does not match its recorded inputs')
        if 'sdk_patch' in release and built.get('sdk_patch_sha256') != sha(text_bytes(here / 'patches' / release['sdk_patch'])):
            raise SystemExit('Official Steam adapter was not built with ' + release['sdk_patch'])
    pe_modules = {}
    if 'pe_members' in release:
        if not args.pe:
            raise SystemExit('--pe is required for ' + args.release)
        record = json.loads((args.pe / 'lsteamclient-pe-build.json').read_text())
        if (record['sourcePin'] != '5d0d333e7beef02fbb455cc3163b4e8e25615458'
                or record['patchSha256'] != sha(text_bytes(here / 'patches' / release['sdk_patch']))
                or record['recipeSha256'] != sha(text_bytes(here / 'build-lsteamclient-pe.sh'))
                or sorted(record['modules']) != sorted(release['pe_members'])):
            raise SystemExit('PE lsteamclient does not match its recorded inputs')
        for name in release['pe_members']:
            module = args.pe / Path(name).parent.name / Path(name).name
            payload = module.read_bytes()
            if sha(payload) != record['modules'][name]['sha256'] or b'SteamUtils011' not in payload:
                raise SystemExit(name + ' is not the recorded SDK 1.65 build')
            headers = subprocess.check_output([str(args.readobj), '--file-headers', str(module)],
                                              text=True).replace(str(module), './' + name)
            pe_modules[name] = (payload, headers)
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
    for name, (payload, pe_headers) in pe_modules.items():
        if name not in names or members[names[name]][1] == payload:
            raise SystemExit(name + ' is missing or unchanged in ' + BASE)
        members[names[name]] = (members[names[name]][0], payload)
        pe_entry = dict(inventory['files'][name], sha256=sha(payload), size=len(payload))
        if 'objectHeaders' in pe_entry:
            pe_entry['objectHeaders'] = pe_headers
        inventory['files'][name] = pe_entry
    patches = [release['sdk_patch'], PATCH] if 'sdk_patch' in release else [PATCH]
    for name in [*('patches/' + p for p in patches), 'build-wine.sh', Path(__file__).name]:
        inventory['recipes']['tools/xrgame/' + name] = sha(text_bytes(here / name))
    if args.release.startswith('linux-steam'):
        inventory['recipes']['tools/xrgame/linux-steam/build-wine-client.py'] = sha(text_bytes(here / 'linux-steam/build-wine-client.py'))
    if pe_modules:
        inventory['recipes']['tools/xrgame/build-lsteamclient-pe.sh'] = sha(text_bytes(here / 'build-lsteamclient-pe.sh'))
    # LinuxSteamLauncher recognises the official Steam runtime by derivedFrom.patch.
    inventory['derivedFrom'] = {'file': BASE, 'patch': 'tools/xrgame/patches/' + PATCH, 'rebuilt': [MEMBER, *pe_modules],
                                'sha256': BASE_SHA256}
    if len(patches) > 1:
        inventory['derivedFrom']['patches'] = ['tools/xrgame/patches/' + p for p in patches]
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
    if pe_modules:
        result['peMembers'] = {name: sha(payload) for name, (payload, _) in pe_modules.items()}
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
