#!/usr/bin/env python3
"""Package locally built runtimes. Does not publish or change the APK catalog."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def object_format(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if header[:4] == b'\x7fELF':
            return 'ELF'
        if header[:2] == b'MZ' and len(header) == 64:
            stream.seek(int.from_bytes(header[60:64], 'little'))
            if stream.read(4) == b'PE\0\0':
                return 'PE'
    return None  # Wine also ships legacy NE font resources with an MZ header.


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def archive(tree, dest, epoch):
    with dest.open('wb') as stream:
        tar = subprocess.Popen(['tar', '--sort=name', '--format=gnu', '--owner=0', '--group=0',
                                '--numeric-owner', f'--mtime=@{epoch}', '-C', str(tree), '-cf', '-', '.'],
                               stdout=subprocess.PIPE)
        try:
            run('xz', '-T4', '-3', '-c', stdin=tar.stdout, stdout=stream)
        finally:
            tar.stdout.close()
        if tar.wait() != 0:
            raise RuntimeError('tar failed')


PINS = {
    'proton-wine': '5d0d333e7beef02fbb455cc3163b4e8e25615458',
    'ntsync-android': '7ce6435e5979b1cb5341aa4b299f31e8937fe121',
    'FEX': '3f1f30a060b633980ed8e7674eb8d8997457edad',
    'dxvk': 'a6764047e587178283fcde4073ae6e1410af594f',
    'vkd3d-proton': '212991fc2c266bc0d59f4c4ce8f80f7126508d71',
    'mesa-turnip': '25ef1647a28d6983bd95f8ef0cc84ea74dd9cc32',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(os.environ.get('XRGAME_BUILD_ROOT', '/work')))
    parser.add_argument('--tag', default='runtime-20260926-preview1')
    parser.add_argument('--epoch', type=int, default=1790380800)
    args = parser.parse_args()
    root = args.root.resolve()
    project = Path(os.environ.get('XRGAME_PROJECT_ROOT', root / 'project'))
    if not args.tag or args.tag in {'.', '..'} or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in args.tag):
        parser.error('tag must be a release path segment')
    out = root / 'packages' / args.tag
    out.mkdir(parents=True, exist_ok=True)
    for name, pin in PINS.items():
        actual = subprocess.check_output(['git', '-C', str(root / 'src' / name), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != pin:
            raise RuntimeError(f'{name}: wrong source pin {actual}')
    strip = root / 'toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
    readobj = strip.with_name('llvm-readobj')
    recipes = {}
    for directory in [project / 'tools/xrgame', project / 'app/src/main/windows/openxr_runtime/builtin']:
        for path in sorted(directory.rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                recipes[str(path.relative_to(project))] = digest(path)
    catalog = {'version': 1, 'updatedAt': '2026-09-26', 'items': {}}
    artifacts = []
    release = f'https://github.com/tencentmalos/xrgame-native/releases/download/{args.tag}/'

    def finish(tree, filename, category, version, license_id, sources, *, driver=False, source_archives=None):
        # Strip only ELF copies. Wine's synthetic PE debug directories are not
        # supported by llvm-strip; keep those generated images byte-for-byte.
        for path in sorted(tree.rglob('*')):
            if path.is_file() and not path.is_symlink():
                if object_format(path) == 'ELF':
                    run(str(strip), '--strip-debug', str(path))
        identities = {}
        for path in sorted(tree.rglob('*')):
            if path.is_symlink():
                if not path.resolve().is_relative_to(tree.resolve()) or not path.exists():
                    raise RuntimeError(f'Unresolved or escaping runtime symlink: {path}')
                identities[str(path.relative_to(tree))] = {'symlink': os.readlink(path)}
            elif path.is_file():
                item = {'sha256': digest(path), 'size': path.stat().st_size}
                if object_format(path):
                    item['objectHeaders'] = subprocess.check_output(
                        [str(readobj), '--file-headers', '--notes', str(path)], text=True).replace(str(tree), '.')
                identities[str(path.relative_to(tree))] = item
        if category == 'proton':
            for arch in ['aarch64-windows', 'i386-windows']:
                if 'XRGAME_BOOTSTRAP_IMAGE'.encode('utf-16le') not in (tree / f'lib/wine/{arch}/ntdll.dll').read_bytes():
                    raise RuntimeError('Rebuild Wine with the exact-image bootstrap patch before packaging')
        write_json(tree / 'xrgame-build.json', {
            'component': version, 'license': license_id, 'sourcePins': {name: PINS[name] for name in sources},
            'sourceArchives': source_archives or {},
            'recipes': recipes, 'files': identities,
            **({'xrgameBootstrapVersion': 1} if category == 'proton' else {}),
            'validation': 'Built and packaged; no device execution has been verified.',
        })
        dest = out / filename
        if driver:
            # Adrenotools' current importer expects a flat ZIP (no directory entries).
            with zipfile.ZipFile(dest, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
                for path in sorted(tree.iterdir()):
                    if not path.is_file():
                        raise RuntimeError('Driver ZIP must be flat')
                    entry = zipfile.ZipInfo(path.name, (2026, 9, 26, 0, 0, 0))
                    entry.compress_type = zipfile.ZIP_DEFLATED
                    entry.external_attr = 0o100644 << 16
                    bundle.writestr(entry, path.read_bytes())
        else:
            archive(tree, dest, args.epoch)
        sha = digest(dest)
        artifacts.append({'file': filename, 'sha256': sha, 'size': dest.stat().st_size})
        catalog['items'].setdefault(category, []).append({
            'id': version, 'name': version, 'url': release + filename, 'sha256': sha,
            'variant': 'bionic', 'arch': 'arm64ec' if not driver else 'aarch64',
            'license': license_id, 'source': release + 'runtime-sources.tar.xz',
        })
        print(f'{filename}: {sha}', flush=True)

    def license_file(tree, component, name, target=None):
        shutil.copy2(root / 'src' / component / name, tree / (target or f'LICENSE-{component}-{Path(name).name}'))

    def profile(tree, kind, version, files):
        value = {'type': kind, 'versionName': version, 'versionCode': 1,
                 'description': 'XRGame source build; device validation pending', 'files': files}
        if kind == 'Proton':
            value['proton'] = {'binPath': 'bin', 'libPath': 'lib', 'prefixPack': 'prefixPack.txz'}
        write_json(tree / 'profile.json', value)

    with tempfile.TemporaryDirectory(prefix='packaging-', dir=root / 'build') as temp:
        work = Path(temp)
        wine = work / 'proton'
        wine.mkdir()
        stage = Path((root / 'output/proton-stage.txt').read_text().strip())
        for name in ['bin', 'lib/wine', 'share/wine']:
            shutil.copytree(stage / name, wine / name, symlinks=True)
        for name in ['wine', 'wine-preloader']:
            (wine / 'bin' / name).symlink_to(f'../lib/wine/aarch64-unix/{name}')
        with tempfile.TemporaryDirectory(dir=work) as empty:
            archive(Path(empty), wine / 'prefixPack.txz', args.epoch)
        version = 'proton-11.0-2-arm64ec'
        profile(wine, 'Proton', version, [])
        for name in ['LICENSE', 'COPYING.LIB', 'AUTHORS']:
            license_file(wine, 'proton-wine', name)
        license_file(wine, 'ntsync-android', 'LICENSE')
        shutil.copy2(project / 'LICENSE', wine / 'LICENSE-XR-bridge-GPL-3.0')
        finish(wine, version + '-xrg5.wcp', 'proton', version,
               'LGPL-2.1-or-later AND LGPL-3.0-only AND GPL-3.0-or-later', ['proton-wine', 'ntsync-android'])

        fex = work / 'fex'
        fex.mkdir()
        files = []
        for name in ['libarm64ecfex', 'libwow64fex']:
            for arch, suffix, target in [('aarch64-windows', '.dll', '${system32}'),
                                         ('aarch64-unix', '.so', '${libdir}/wine/aarch64-unix')]:
                shutil.copy2(root / 'output/fex/lib/wine' / arch / (name + suffix), fex / (name + suffix))
                files.append({'source': name + suffix, 'target': target + '/' + name + suffix})
        record = root / 'output/fex/xrgame-fex-build.json'
        build = json.loads(record.read_text())
        if build['sourcePin'] != PINS['FEX']:
            raise RuntimeError('FEX build source pin does not match the recipe')
        for name in ['fex-wine-4k-pages.patch', 'fex-arm64ec-work-callback-guard.patch',
                     'fex-single-step-return.patch']:
            patch = project / 'tools/xrgame/patches' / name
            if build['patches'].get(name) != digest(patch):
                raise RuntimeError('FEX build patch does not match the packaged recipe')
            shutil.copy2(patch, fex / name)
        for entry in files:
            arch = 'aarch64-windows' if entry['source'].endswith('.dll') else 'aarch64-unix'
            if build['binaries'].get('lib/wine/' + arch + '/' + entry['source']) != digest(fex / entry['source']):
                raise RuntimeError('FEX binary does not match its build record')
        shutil.copy2(record, fex / record.name)
        version = '2608-3f1f30a-xrg5'
        profile(fex, 'FEXCore', version, files)
        license_file(fex, 'FEX', 'LICENSE')
        finish(fex, 'fexcore-' + version + '.wcp', 'fexcore', version, 'MIT', ['FEX'])

        for name, kind, version, license_id in [
            ('dxvk', 'DXVK', '11.0-a676404-arm64ec-xrg4', 'Zlib'),
            ('vkd3d-proton', 'VKD3D', '11.0-212991f-arm64ec-xrg1', 'LGPL-2.1-or-later'),
        ]:
            tree = work / name
            tree.mkdir()
            files = []
            for path in sorted((root / 'output' / name / 'arm64ec').glob('*.dll')):
                shutil.copy2(path, tree / path.name)
                files.append({'source': path.name, 'target': '${system32}/' + path.name})
            if not files:
                raise RuntimeError(f'No {name} DLLs')
            profile(tree, kind, version, files)
            license_file(tree, name, 'LICENSE')
            if name == 'dxvk':
                record = root / 'output/dxvk/xrgame-dxvk-build.json'
                build = json.loads(record.read_text())
                patch = project / 'tools/xrgame/patches/dxvk-present-deferred-clears.patch'
                if build['patchSha256'] != digest(patch):
                    raise RuntimeError('DXVK build patch does not match the packaged recipe')
                latency_patch = project / 'tools/xrgame/patches/dxvk-waitable-frame-latency.patch'
                if build.get('latencyProbePatchSha256') != digest(latency_patch):
                    raise RuntimeError('DXVK latency patch does not match the packaged recipe')
                completion_patch = project / 'tools/xrgame/patches/dxvk-present-gpu-completion.patch'
                if build.get('gpuCompletionPatchSha256') != digest(completion_patch):
                    raise RuntimeError('DXVK completion patch does not match the packaged recipe')
                for entry in files:
                    if build['binaries'].get('arm64ec/' + entry['source']) != digest(tree / entry['source']):
                        raise RuntimeError('DXVK binary does not match its build record')
                shutil.copy2(record, tree / record.name)
                shutil.copy2(patch, tree / patch.name)
                shutil.copy2(latency_patch, tree / latency_patch.name)
                shutil.copy2(completion_patch, tree / completion_patch.name)
            finish(tree, name + '-' + version + '.wcp', name if name == 'dxvk' else 'vkd3d',
                   version, license_id, [name])

        turnip = work / 'turnip'
        turnip.mkdir()
        shutil.copy2(root / 'output/turnip/libvulkan_freedreno.so', turnip / 'libvulkan_freedreno.so')
        for name in ['libxcb-dri3.so', 'libxcb-present.so']:
            shutil.copy2(root / 'output/xcb' / name, turnip / name)
        shutil.copy2(root / 'output/xcb/LICENSE', turnip / 'LICENSE-libxcb-MIT')
        xcb_sources = {name: digest(root / 'downloads' / name) for name in
                       ['libxcb-1.17.0.tar.xz', 'xcb-proto-1.17.0.tar.xz']}
        version = 'turnip-25ef164-xrg10'
        write_json(turnip / 'meta.json', {'schemaVersion': 1, 'name': version, 'author': 'Mesa contributors',
                   'description': 'XRGame KGSL, Android and X11 WSI source build', 'vendor': 'Mesa',
                   'driverVersion': version, 'minApi': 33, 'libraryName': 'libvulkan_freedreno.so'})
        write_json(turnip / 'freedreno_icd.aarch64.json', {'file_format_version': '1.0.0',
                   'ICD': {'library_path': 'libvulkan_freedreno.so', 'api_version': '1.3.0'}})
        license_file(turnip, 'mesa-turnip', 'licenses/MIT')
        license_file(turnip, 'mesa-turnip', 'docs/license.rst')
        finish(turnip, version + '.zip', 'driver', version, 'MIT', ['mesa-turnip'],
               driver=True, source_archives=xcb_sources)

    write_json(out / 'manifest.draft.json', catalog)
    write_json(out / 'build-record.json', {
        'sourcePins': PINS, 'recipes': recipes, 'artifacts': artifacts, 'published': False,
        'releaseReady': False,
        'pending': ['Corresponding source archive and dependency notices',
                    'Base imagefs dependency package', 'Device execution and validation'],
    })
    (out / 'SHA256SUMS').write_text(''.join(f"{item['sha256']}  {item['file']}\n" for item in artifacts))
    print(f'Draft catalog and local build record: {out}', flush=True)


if __name__ == '__main__':
    main()
