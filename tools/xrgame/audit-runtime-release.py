#!/usr/bin/env python3
"""Fail closed before publication. A successful build alone is insufficient."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import urlsplit


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def audit(packages, sources, dependencies):
    errors = []
    record = json.loads((packages / 'build-record.json').read_text())
    catalog_path = packages / 'manifest.draft.json'
    catalog = json.loads(catalog_path.read_text())
    if sha(catalog_path) != record.get('catalogSha256'):
        errors.append('Catalog changed after finalization')
    if record.get('releaseReady') is not True or record.get('pending'):
        errors.append('Build record still has release prerequisites')
    if set(catalog['items']) != {'proton', 'fexcore', 'dxvk', 'vkd3d', 'driver', 'imagefs'}:
        errors.append('Expected the complete six-component catalog')
    artifact_map = {item['file']: item for item in record['artifacts']}
    seen = set()
    for entries in catalog['items'].values():
        if len(entries) != 1:
            errors.append('Each default component category must have exactly one entry')
        for entry in entries:
            url = urlsplit(entry['url'])
            if (url.scheme != 'https' or url.netloc != 'github.com' or url.query or url.fragment or
                    not re.fullmatch(r'/tencentmalos/xrgame-native/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+', url.path)):
                errors.append('Unexpected component release URL')
                continue
            name = url.path.rsplit('/', 1)[1]
            expected_source = entry['url'].rsplit('/', 1)[0] + '/runtime-sources.tar.xz'
            if not entry.get('license') or entry.get('source') != expected_source:
                errors.append(f'Missing or unexpected source/license metadata: {name}')
            path = packages / name
            item = artifact_map.get(name)
            if name in seen:
                errors.append(f'Duplicate catalog artifact: {name}')
            seen.add(name)
            if (not item or path.is_symlink() or sha(path) != entry['sha256'] or
                    item['sha256'] != entry['sha256'] or path.stat().st_size != item['size']):
                errors.append(f'Artifact hash/size mismatch: {name}')
    if set(artifact_map) != seen:
        errors.append('Catalog and artifact list differ')
    for directory in [packages, sources, dependencies]:
        if any(directory.rglob('*.apk')):
            errors.append('APKs must not be published from the public repository')
    for directory, index_name, archive_name, binding in [
        (sources, 'runtime-source-index.json', 'runtime-sources.tar.xz', record.get('sourceArchive', {})),
        (dependencies, 'dependency-source-index.json', 'dependency-sources.tar.xz', record.get('dependencySources', {})),
    ]:
        index = json.loads((directory / index_name).read_text())
        expected_index_sha = (record.get('sourceIndexSha256') if index_name == 'runtime-source-index.json'
                              else binding.get('indexSha256'))
        if sha(directory / index_name) != expected_index_sha:
            errors.append(f'Source index changed after finalization: {index_name}')
        archive = directory / archive_name
        if (index.get('archive') != archive_name or archive.is_symlink() or
                sha(archive) != index.get('archiveSha256') or binding.get('sha256') != sha(archive)):
            errors.append(f'Source archive hash/binding mismatch: {archive_name}')
        if index.get('completeCorrespondingSource') is not True or index.get('pending'):
            errors.append(f'Corresponding source remains incomplete: {archive_name}')
    return {'passed': not errors, 'errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packages', type=Path, required=True)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--dependency-sources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = audit(args.packages, args.sources, args.dependency_sources)
    except (ValueError, KeyError, OSError, TypeError) as error:
        report = {'passed': False, 'errors': [str(error)]}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
