#!/usr/bin/env python3
"""Bundle version-matched Termux sources and patches, with explicit attribution gaps."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', type=Path, default=Path(__file__).with_name('termux-sources.lock.json'))
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--imagefs-record', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    image = json.loads(args.imagefs_record.read_text())
    if image['termuxSnapshotSha256'] != lock['snapshotSha256']:
        raise ValueError('Dependency lock is for a different binary snapshot')
    for name, package in image['packages'].items():
        if lock['packages'].get(name, {}).get('version') != package['version']:
            raise ValueError(f'Unmatched dependency version: {name}')
    entries = [lock['recipeArchive']]
    for package in lock['packages'].values():
        entries.extend(package['sources'])
    for override in lock.get('recipeOverrides', {}).values():
        entries.extend(override.values())
    entries = {entry['sha256']: entry for entry in entries}
    # Verify every input before producing a distributable archive.
    for digest in entries:
        path = args.cache / digest
        if len(digest) != 64 or path.is_symlink() or sha(path) != digest:
            raise ValueError(f'Missing or corrupt dependency source: {digest}')
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / 'dependency-sources.tar.xz'
    if archive.exists():
        parser.error('Refusing to overwrite a source archive')
    spec = importlib.util.spec_from_file_location('components', Path(__file__).with_name('package-components.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix='dependency-sources-', dir=args.output) as temp:
        tree = Path(temp)
        (tree / 'archives').mkdir()
        for digest in entries:
            shutil.copy2(args.cache / digest, tree / 'archives' / digest)
        shutil.copy2(args.lock, tree / 'termux-sources.lock.json')
        shutil.copy2(args.imagefs_record, tree / 'imagefs-record.json')
        (tree / 'README.txt').write_text(
            'Archives are named by SHA-256. The lock maps names, versions, license declarations, '
            'source URLs and recipe commits. Extract recipeArchive for Termux patches/build scripts; '
            'apply recipeOverrides for the libwebp RC package.\n'
            'This is a version-matched source collection, not an attestation of the original '
            'binary build. The original snapshot did not record individual recipe commits. '
            'Additional build-time downloads (including librav1e Rust dependencies and the '
            'ICU license replacement) and NDK source attribution still require closure.\n')
        module.archive(tree, archive, 1790380800)
    report = {'schema': 1, 'archive': archive.name, 'archiveSha256': sha(archive),
              'lockSha256': sha(args.lock), 'imagefsRecordSha256': sha(args.imagefs_record),
              'packages': len(image['packages']), 'sourceObjects': len(entries),
              'completeCorrespondingSource': False,
              'pending': ['Exact recipe attribution for the prebuilt Termux snapshot',
                          'Additional build-time dependencies and NDK source attribution',
                          'Rebuild verification from this source collection']}
    (args.output / 'dependency-source-index.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
