#!/usr/bin/env python3
"""Check the complete picoXr APK input closure before starting an expensive build."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

SUBMODULES = {
    '.': [
        'foundation', 'references/JavaSteam', 'references/opencomposite',
        'references/Vulkan-Headers', 'references/fmt',
        'app/src/main/cpp/extras/adrenotools', 'app/src/main/cpp/lsfg-vk-android',
    ],
    'foundation': ['third_party/pinyinime'],
    'app/src/main/cpp/extras/adrenotools': ['lib/linkernsbypass'],
    'app/src/main/cpp/lsfg-vk-android': ['thirdparty/dxbc'],
}
PULSE_FILES = ['libpulseaudio.so', 'libpulse.so', 'libpulsecommon-13.0.so',
               'libpulsecore-13.0.so', 'libltdl.so', 'libsndfile.so',
               'pulseaudio-xrgame-20260926.tzst']


def digest(path, text=False):
    if text:
        return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def member(root, name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError('Unsafe manifest path: ' + name)
    result = root.joinpath(*path.parts)
    if not result.resolve().is_relative_to(root.resolve()):
        raise ValueError('Manifest path escapes input directory: ' + name)
    return result


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True, stderr=subprocess.PIPE).strip()


def check(root, sources_only=False):
    checks = []

    def perform(name, action):
        try:
            detail = action()
            checks.append({'name': name, 'passed': True, 'detail': detail})
        except (OSError, ValueError, KeyError, AssertionError, subprocess.CalledProcessError) as error:
            checks.append({'name': name, 'passed': False, 'error': str(error)})

    def submodule(parent, path):
        repo = root / parent
        index = git(repo, 'ls-files', '--stage', '--', path).split()
        if len(index) != 4 or index[0] != '160000':
            raise ValueError('Missing gitlink: ' + str(repo / path))
        if not (repo / path / '.git').exists():
            raise ValueError(f'Run: git -C {repo} submodule update --init {path}')
        actual = git(repo / path, 'rev-parse', 'HEAD')
        if actual != index[1]:
            raise ValueError(f'Expected {index[1]}, found {actual}; update {repo / path} to its gitlink')
        return actual

    for parent, paths in SUBMODULES.items():
        for path in paths:
            perform(str(Path(parent) / path), lambda parent=parent, path=path: submodule(parent, path))

    def pinyin():
        dictionary = root / 'foundation/third_party/pinyinime/res/raw/dict_pinyin.dat'
        expected = '6bf0bbde4e3134cce38d08524a9f4dc1af40435243c8e36b38eb68d1e14462b2'
        if digest(dictionary) != expected:
            raise ValueError('PinyinIME dictionary checksum mismatch')
        return expected

    perform('pinyin dictionary', pinyin)
    pin = json.loads((root / 'tools/xrgame/opencomposite-pin.json').read_text())

    def recipes():
        for name, expected in pin['recipeFiles'].items():
            if digest(member(root, name), text=True) != expected:
                raise ValueError('OpenComposite recipe checksum mismatch: ' + name)
        registry = json.loads((root / 'tools/xrgame/apk-sources.json').read_text(encoding='utf-8'))
        entry = next((item for item in registry['rules'] if item['pattern'] == 'assets/opencomposite_x64.dll'), None)
        if entry is None or entry.get('sha256') != pin['sha256']:
            raise ValueError('OpenComposite APK audit pin differs from the packaging/runtime pin')
        return f"{len(pin['recipeFiles'])} pinned recipe files"

    perform('OpenComposite recipe', recipes)
    if not sources_only:
        def opencomposite():
            if digest(member(root, pin['artifact'])) != pin['sha256']:
                raise ValueError('OpenComposite DLL checksum mismatch')
            return pin['sha256']

        perform('OpenComposite DLL', opencomposite)

        def bundle():
            directory = root / 'build/xrgame-runtime/bundle'
            catalog = json.loads((directory / 'manifest.json').read_text())
            versions = dict(re.findall(r'String (\w+) = "([^"]+)"',
                (root / 'app/src/main/java/app/gamenative/xrgame/XrGameRuntimeVersions.java').read_text()))
            for kind, field in [('proton', 'WINE'), ('fexcore', 'FEX'), ('dxvk', 'DXVK'),
                                ('vkd3d', 'VKD3D'), ('driver', 'TURNIP')]:
                if not any(item['id'] == versions[field] for item in catalog['items'].get(kind, [])):
                    raise ValueError(f'Missing default {kind}: {versions[field]}')
            for kind in ['imagefs', 'steamclient']:
                if len(catalog['items'].get(kind, [])) != 1:
                    raise ValueError('Missing or ambiguous default ' + kind)
            for items in catalog['items'].values():
                for item in items:
                    name = item['url'].rsplit('/', 1)[1]
                    if not item.get('license') or not item.get('source'):
                        raise ValueError('Missing source/license: ' + name)
                    if digest(member(directory / 'components', name)) != item['sha256']:
                        raise ValueError('Runtime checksum mismatch: ' + name)
            return {'catalogSha256': digest(directory / 'manifest.json'),
                    'components': sum(len(items) for items in catalog['items'].values())}

        perform('bundled game runtime', bundle)

        def pulse():
            directory = root / 'build/xrgame-runtime/pulseaudio'
            record = json.loads((directory / 'xrgame-pulse-build.json').read_text())
            if record['pulseaudioPin'] != '4166ad3e30ec2a29a37d5fc1eaafcc7e03ebfe55':
                raise ValueError('Unexpected PulseAudio source pin')
            for name in PULSE_FILES:
                if digest(directory / name) != record['sha256'][name]:
                    raise ValueError('PulseAudio checksum mismatch: ' + name)
            return digest(directory / 'xrgame-pulse-build.json')

        perform('PulseAudio', pulse)

        def steam():
            directory = root / 'build/xrgame-linux-steam'
            path = directory / 'assets/linux-steam/manifest.json'
            record = json.loads(path.read_text())
            if record.get('valveBinariesBundled') is not False:
                raise ValueError('Linux Steam staging must not contain Valve binaries')
            required = {'assets/linux-steam/' + name for name in [
                'rootfs.tgz', 'lsteamclient.so', 'libsteampsp-client-compat.so',
                'client-compat-build.json', 'install-client.py', 'launch-client.py',
                'ime-text.py', 'game-launch.py',
            ]} | {'jniLibs/arm64-v8a/' + name for name in [
                'libproot.so', 'libproot-loader.so', 'libxrgame_steam_session.so',
            ]}
            missing = required - record['files'].keys()
            if missing:
                raise ValueError('Linux Steam manifest lacks required files: ' + ', '.join(sorted(missing)))
            for name, expected in record['files'].items():
                if digest(member(directory, name)) != expected:
                    raise ValueError('Linux Steam checksum mismatch: ' + name)
            # Scripts are app source; a self-consistent stale staging manifest is not sufficient.
            for name in ['install-client.py', 'launch-client.py', 'ime-text.py', 'game-launch.py']:
                if digest(directory / 'assets/linux-steam' / name, text=True) != digest(root / 'tools/xrgame/linux-steam' / name, text=True):
                    raise ValueError('Linux Steam staging uses stale source: ' + name)
            adapter = json.loads((directory / 'assets/linux-steam/client-compat-build.json').read_text())
            if adapter['adapterSha256'] != digest(root / 'tools/xrgame/linux-steam/client-proc.c', text=True):
                raise ValueError('Rebuild Linux Steam client compatibility library from current client-proc.c with linux-steam/stage.py')
            return digest(path)

        perform('Linux Steam userspace', steam)
    return {'schema': 1, 'passed': all(item['passed'] for item in checks), 'checks': checks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--init-submodules', action='store_true', help='Initialize only APK source dependencies at recorded gitlinks')
    parser.add_argument('--sources-only', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.init_submodules:
        for parent, paths in SUBMODULES.items():
            subprocess.run(['git', '-C', str(args.root / parent), 'submodule', 'update', '--init', *paths], check=True)
    result = check(args.root, args.sources_only)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2) + '\n')
    for item in result['checks']:
        print(('OK   ' if item['passed'] else 'FAIL ') + item['name'] + ': ' + str(item.get('detail', item.get('error'))))
    if not result['passed'] and not args.sources_only:
        print('See docs/building/apple-container.md for staged runtime inputs. A Git clone alone does not supply private build artifacts.', file=sys.stderr)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
