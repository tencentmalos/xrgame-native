#!/usr/bin/env python3
"""Package the texture quality / render scale builds of DXVK and vkd3d-proton.

Starts from the validated private archives (DXVK xrg4, vkd3d-proton xrg1), replaces the rebuilt
DLLs and adds the quality patches. Source pins, patch hashes and binary hashes are checked against
the build outputs. --only packages one of the two, so the other keeps its published archive.
The results are private validation components, not release artifacts.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import runpy
import tarfile

DXVK_PIN = 'a6764047e587178283fcde4073ae6e1410af594f'
VKD3D_PIN = '212991fc2c266bc0d59f4c4ce8f80f7126508d71'
DXVK_VERSION = '11.0-a676404-arm64ec-xrg6'
DXVK_VERSION_CODE = 6
VKD3D_VERSION = '11.0-212991f-arm64ec-xrg3'
VKD3D_VERSION_CODE = 3
DXVK_BASE_SHA256 = 'e1a2b0581fb95effe566ef2f4f3b1f906ae3455c9e62e88b9abf1a77a3ec2e39'
VKD3D_BASE_SHA256 = 'e50a148e0abbcd3321a1bf6cc3912b9463298b47ded1a0c35eeedbe3f7cb0b42'
DXVK_PATCHES = ('dxvk-present-deferred-clears.patch', 'dxvk-waitable-frame-latency.patch',
                'dxvk-present-gpu-completion.patch', 'dxvk-xrgame-resource-scale.patch')
VKD3D_PATCHES = ('vkd3d-xrgame-resource-scale.patch', 'dxil-spirv-xrgame-render-scale.patch')
DESCRIPTION = 'XRGame private build: XRGAME_TEXTURE_QUALITY and XRGAME_RENDER_SCALE; defaults unchanged'
VALIDATION = ('Private validation build; see docs/validation/swan-d3d-texture-quality-20261013.md and '
              'docs/validation/swan-d3d-render-scale-20261013.md.')
MTIME = 1791849600


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()


def read_base(path, expected, inspect, kind):
    if sha(path.read_bytes()) != expected:
        raise ValueError(f'Expected the validated base archive for {kind}')
    inspect(path, kind)
    with tarfile.open(path) as source:
        return {m.name.removeprefix('./'): source.extractfile(m).read() for m in source if m.isfile()}


def write_archive(path, files, inspect, kind):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    try:
        with tarfile.open(temporary, 'w:xz', preset=3) as dest:
            for name, data in sorted(files.items()):
                member = tarfile.TarInfo('./' + name)
                member.size, member.mode, member.mtime = len(data), 0o644, MTIME
                dest.addfile(member, io.BytesIO(data))
        inspect(temporary, kind)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    return {'file': path.name, 'sha256': sha(path.read_bytes()), 'size': path.stat().st_size}


def finish(original, payload, version_code, version, base_sha, recipes, here):
    inventory = json.loads(original.pop('xrgame-build.json'))
    profile = json.loads(original['profile.json'])
    profile.update(versionName=version, versionCode=version_code, description=DESCRIPTION)
    original['profile.json'] = encoded(profile)
    original.update(payload)
    inventory.update(component=version, releaseReady=False, baseArchiveSha256=base_sha, validation=VALIDATION)
    inventory['recipes'].update(recipes)
    inventory['recipes']['tools/xrgame/' + Path(__file__).name] = sha(Path(__file__).read_bytes())
    inventory['files'] = {name: {'sha256': sha(data), 'size': len(data)} for name, data in original.items()}
    original['xrgame-build.json'] = encoded(inventory)
    return inventory


def package_dxvk(args, here, inspect):
    original = read_base(args.dxvk_base, DXVK_BASE_SHA256, inspect, 'dxvk')
    build_data = (args.dxvk_build / 'xrgame-dxvk-build.json').read_bytes()
    build = json.loads(build_data)
    patches = {name: (here / 'patches' / name).read_bytes() for name in DXVK_PATCHES}
    if (build['sources'][0]['path'] != '.' or build['sources'][0]['commit'] != DXVK_PIN
            or build['architecture'] != 'arm64ec-windows'
            or build['patchSha256'] != sha(patches['dxvk-present-deferred-clears.patch'])
            or build['latencyProbePatchSha256'] != sha(patches['dxvk-waitable-frame-latency.patch'])
            or build['gpuCompletionPatchSha256'] != sha(patches['dxvk-present-gpu-completion.patch'])
            or build.get('qualityPatchSha256') != sha(patches['dxvk-xrgame-resource-scale.patch'])
            or build['recipeSha256'] != sha((here / 'build-dxvk-validation.sh').read_bytes())):
        raise ValueError('DXVK build does not match the pinned quality recipe')
    if json.loads(original['xrgame-build.json'])['sourcePins']['dxvk'] != DXVK_PIN:
        raise ValueError('Wrong DXVK base source pin')
    payload = dict(patches, **{'xrgame-dxvk-build.json': build_data})
    for name in ('d3d8.dll', 'd3d9.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll'):
        data = (args.dxvk_build / 'arm64ec' / name).read_bytes()
        if build['binaries'].get('arm64ec/' + name) != sha(data):
            raise ValueError('Changed DXVK build output: ' + name)
        payload[name] = data
    recipes = {'tools/xrgame/patches/' + name: sha(data) for name, data in patches.items()}
    recipes['tools/xrgame/build-dxvk-validation.sh'] = sha((here / 'build-dxvk-validation.sh').read_bytes())
    finish(original, payload, DXVK_VERSION_CODE, DXVK_VERSION, DXVK_BASE_SHA256, recipes, here)
    return write_archive(args.output / f'dxvk-{DXVK_VERSION}.wcp', original, inspect, 'dxvk')


def package_vkd3d(args, here, inspect):
    original = read_base(args.vkd3d_base, VKD3D_BASE_SHA256, inspect, 'vkd3d')
    if json.loads(original['xrgame-build.json'])['sourcePins']['vkd3d-proton'] != VKD3D_PIN:
        raise ValueError('Wrong vkd3d-proton base source pin')
    patches = {name: (here / 'patches' / name).read_bytes() for name in VKD3D_PATCHES}
    payload = dict(patches)
    for name in ('d3d12.dll', 'd3d12core.dll'):
        payload[name] = (args.vkd3d_build / 'arm64ec' / name).read_bytes()
    recipes = {'tools/xrgame/patches/' + name: sha(data) for name, data in patches.items()}
    recipes['tools/xrgame/build-d3d.sh'] = sha((here / 'build-d3d.sh').read_bytes())
    inventory = finish(original, payload, VKD3D_VERSION_CODE, VKD3D_VERSION, VKD3D_BASE_SHA256, recipes, here)
    # build-d3d.sh applies the patches to a copy of the pinned tree; the build directory carries
    # their digest, so record them next to the binaries.
    inventory['qualityPatchSha256'] = sha(patches['vkd3d-xrgame-resource-scale.patch'])
    inventory['dxilSpirvPatchSha256'] = sha(patches['dxil-spirv-xrgame-render-scale.patch'])
    original['xrgame-build.json'] = encoded(inventory)
    return write_archive(args.output / f'vkd3d-proton-{VKD3D_VERSION}.wcp', original, inspect, 'vkd3d')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxvk-base', type=Path)
    parser.add_argument('--dxvk-build', type=Path)
    parser.add_argument('--vkd3d-base', type=Path)
    parser.add_argument('--vkd3d-build', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--only', choices=('dxvk', 'vkd3d'))
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    inspect = runpy.run_path(str(here / 'verify-runtime-packages.py'))['inspect']
    results = {}
    if args.only != 'vkd3d':
        results['dxvk'] = dict(package_dxvk(args, here, inspect), id=DXVK_VERSION)
    if args.only != 'dxvk':
        results['vkd3d'] = dict(package_vkd3d(args, here, inspect), id=VKD3D_VERSION)
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
