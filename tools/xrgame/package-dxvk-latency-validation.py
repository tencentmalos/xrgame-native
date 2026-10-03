#!/usr/bin/env python3
"""Package the opt-in DXGI latency probe with verified source and binary hashes."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import runpy
import tarfile

PIN = 'a6764047e587178283fcde4073ae6e1410af594f'
VERSION = '11.0-a676404-arm64ec-xrg4'
BASE_SHA256 = 'c2b801c2e584b793c93deb678bee5c7138740dd5916c37bad324caebc005deee'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    inspect = runpy.run_path(str(here / 'verify-runtime-packages.py'))['inspect']
    if sha(args.base.read_bytes()) != BASE_SHA256:
        raise ValueError('Expected the validated xrg2 base archive')
    inspect(args.base, 'dxvk')
    build_data = (args.build / 'xrgame-dxvk-build.json').read_bytes()
    build = json.loads(build_data)
    patches = {name: (here / 'patches' / name).read_bytes() for name in (
        'dxvk-present-deferred-clears.patch', 'dxvk-waitable-frame-latency.patch',
        'dxvk-present-gpu-completion.patch')}
    if (build['sources'][0]['path'] != '.' or build['sources'][0]['commit'] != PIN
            or build['architecture'] != 'arm64ec-windows'
            or build['patchSha256'] != sha(patches['dxvk-present-deferred-clears.patch'])
            or build['latencyProbePatchSha256'] != sha(patches['dxvk-waitable-frame-latency.patch'])
            or build['gpuCompletionPatchSha256'] != sha(patches['dxvk-present-gpu-completion.patch'])
            or build['recipeSha256'] != sha((here / 'build-dxvk-validation.sh').read_bytes())):
        raise ValueError('Build does not match the pinned latency probe recipe')
    payload = dict(patches, **{'xrgame-dxvk-build.json': build_data})
    for name in ('d3d8.dll', 'd3d9.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll'):
        data = (args.build / 'arm64ec' / name).read_bytes()
        if build['binaries'].get('arm64ec/' + name) != sha(data):
            raise ValueError('Changed build output: ' + name)
        payload[name] = data
    with tarfile.open(args.base) as source:
        original = {m.name.removeprefix('./'): source.extractfile(m).read()
                    for m in source if m.isfile()}
    inventory = json.loads(original.pop('xrgame-build.json'))
    if inventory['sourcePins']['dxvk'] != PIN:
        raise ValueError('Wrong base source pin')
    profile = json.loads(original['profile.json'])
    profile.update(versionName=VERSION, versionCode=4,
                   description='XRGame private DXGI latency probe; override disabled by default')
    payload['profile.json'] = encoded(profile)
    original.update(payload)
    inventory.update(component=VERSION, releaseReady=False,
                     baseArchiveSha256=BASE_SHA256,
                     validation='Private latency probe; not a measured performance improvement.')
    inventory['recipes'].update({
        'tools/xrgame/patches/' + name: sha(data) for name, data in patches.items()})
    for name in ('build-dxvk-validation.sh', Path(__file__).name):
        inventory['recipes']['tools/xrgame/' + name] = sha((here / name).read_bytes())
    inventory['files'] = {name: {'sha256': sha(data), 'size': len(data)}
                          for name, data in original.items()}
    original['xrgame-build.json'] = encoded(inventory)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.tmp')
    try:
        with tarfile.open(temporary, 'w:xz', preset=3) as dest:
            for name, data in sorted(original.items()):
                member = tarfile.TarInfo('./' + name)
                member.size, member.mode, member.mtime = len(data), 0o644, 1790899200
                dest.addfile(member, io.BytesIO(data))
        inspect(temporary, 'dxvk')
        temporary.replace(args.output)
    finally:
        temporary.unlink(missing_ok=True)
    print(json.dumps({'file': args.output.name, 'sha256': sha(args.output.read_bytes()),
                      'version': VERSION, 'releaseReady': False}))


if __name__ == '__main__':
    main()
