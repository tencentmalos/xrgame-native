#!/usr/bin/env python3
"""Bind an incremental ntdll build to its verified base Proton archive for private tests."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import runpy
import tarfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    inspect = runpy.run_path(str(here / 'verify-runtime-packages.py'))['inspect']
    inspect(args.base, 'proton')
    patch = (here / 'patches/wine-image-bootstrap.patch').read_bytes()
    record = json.loads((args.build / 'bootstrap-build.json').read_text())
    pin = '5d0d333e7beef02fbb455cc3163b4e8e25615458'
    if record['sourcePin'] != pin or record['patchSha256'] != sha(patch) or record['recipeSha256'] != sha((here / 'build-wine-bootstrap.sh').read_bytes()):
        raise ValueError('Wine bootstrap build does not match the source recipe')
    replacements = {}
    for arch in ['aarch64-windows', 'i386-windows']:
        name = f'lib/wine/{arch}/ntdll.dll'
        data = (args.build / arch / 'ntdll.dll').read_bytes()
        if record['modules'][name] != {'sha256': sha(data), 'size': len(data)}:
            raise ValueError(f'Bootstrap DLL changed: {name}')
        replacements[name] = data
    with tarfile.open(args.base) as source:
        inventory = json.load(source.extractfile('./xrgame-build.json'))
        if inventory['sourcePins']['proton-wine'] != pin:
            raise ValueError('Wrong base Proton pin')
        inventory['files'].update(record['modules'])
        inventory['xrgameBootstrapVersion'] = 1
        inventory['bootstrapBuild'] = dict(record, baseArchiveSha256=sha(args.base.read_bytes()))
        inventory['validation'] = 'Private incremental bootstrap candidate; device verification required.'
        inventory['files']['wine-image-bootstrap.patch'] = {'sha256': sha(patch), 'size': len(patch)}
        replacements['xrgame-build.json'] = (json.dumps(inventory, indent=2, sort_keys=True) + '\n').encode()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.output, 'w:xz', preset=3) as dest:
            seen = set()
            for member in source:
                name = member.name.removeprefix('./')
                if name in replacements:
                    data = replacements[name]
                    member.size = len(data)
                    dest.addfile(member, io.BytesIO(data))
                    seen.add(name)
                else:
                    dest.addfile(member, source.extractfile(member) if member.isfile() else None)
            if seen != replacements.keys():
                raise ValueError('Expected Proton modules missing from base')
            member = tarfile.TarInfo('./wine-image-bootstrap.patch')
            member.size, member.mode, member.mtime = len(patch), 0o644, 1790380800
            dest.addfile(member, io.BytesIO(patch))
    inspect(args.output, 'proton')
    print(json.dumps({'file': args.output.name, 'sha256': sha(args.output.read_bytes()), 'releaseReady': False}))


if __name__ == '__main__':
    main()
