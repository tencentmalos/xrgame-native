#!/usr/bin/env python3
"""Stage the pinned runtime as APK assets; never fetch or publish components."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
from urllib.parse import urlparse


def stage(catalog, packages, output):
    manifest = json.loads(catalog.read_text())
    inspect_archive = runpy.run_path(str(Path(__file__).with_name('verify-runtime-packages.py')))['inspect']
    root = Path(__file__).resolve().parents[2]
    versions = dict(re.findall(r'String (\w+) = "([^"]+)"',
        (root / 'app/src/main/java/app/gamenative/xrgame/XrGameRuntimeVersions.java').read_text()))
    for kind, constant in [('proton', 'WINE'), ('fexcore', 'FEX'), ('dxvk', 'DXVK'),
                           ('vkd3d', 'VKD3D'), ('driver', 'TURNIP')]:
        assert any(e['id'] == versions[constant] for e in manifest['items'].get(kind, [])), \
            f'Missing default {kind}: {versions[constant]}'
    assert len(manifest['items'].get('imagefs', [])) == 1, 'Missing or ambiguous base filesystem'
    assert len(manifest['items'].get('steamclient', [])) == 1, 'Missing or ambiguous bundled Steam client'
    checked = []
    names = set()
    for kind, group in manifest['items'].items():
        for entry in group:
            url = urlparse(entry['url'])
            assert url.scheme == 'https' and url.netloc == 'github.com'
            assert not url.query and not url.fragment
            assert re.fullmatch(r'/tencentmalos/xrgame-native/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+', url.path)
            name = url.path.rsplit('/', 1)[1]
            assert name not in names, f'Duplicate component: {name}'
            names.add(name)
            assert entry.get('license') and entry.get('source'), f'Missing provenance: {name}'
            src = packages / name
            with src.open('rb') as stream:
                actual = hashlib.file_digest(stream, 'sha256').hexdigest()
            assert actual == entry['sha256'], f'SHA-256 mismatch: {name}'
            if kind in {'proton', 'fexcore', 'dxvk', 'vkd3d', 'driver', 'imagefs', 'steamclient'}:
                inspect_archive(src, kind)
            checked.append((src, name))
    dest = output / 'components'
    dest.mkdir(parents=True, exist_ok=True)
    for src, name in checked:
        shutil.copy2(src, dest / name)
    shutil.copy2(catalog, output / 'manifest.json')
    print(f'Staged {len(checked)} verified runtime archives ({sum(s.stat().st_size for s, _ in checked)} bytes)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', required=True, type=Path)
    parser.add_argument('--packages', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=Path('build/xrgame-runtime/bundle'))
    args = parser.parse_args()
    stage(args.catalog, args.packages, args.output)
