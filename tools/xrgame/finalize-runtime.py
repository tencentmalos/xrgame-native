#!/usr/bin/env python3
"""Join binary/source evidence into a six-component draft; never activate production."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(os.environ.get('XRGAME_BUILD_ROOT', '/work')))
    parser.add_argument('--tag', default='runtime-20260926-preview1')
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--dependency-sources', type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9._-]+', args.tag) or args.tag in {'.', '..'}:
        parser.error('tag must be a release path segment')
    out = args.root / 'packages' / args.tag
    record = json.loads((out / 'build-record.json').read_text())
    catalog = json.loads((out / 'manifest.draft.json').read_text())
    source = json.loads((args.sources / 'runtime-source-index.json').read_text())
    source_archive = args.sources / 'runtime-sources.tar.xz'
    if source['archive'] != source_archive.name or source['archiveSha256'] != sha(source_archive):
        raise ValueError('Source archive does not match its index')
    for name, pin in record['sourcePins'].items():
        if source['sources'].get(name, {}).get('commit') != pin:
            raise ValueError(f'Source archive does not contain matching {name}')
    for name, digest in record['recipes'].items():
        if source['files'].get('project/' + name, {}).get('sha256') != digest:
            raise ValueError(f'Source archive does not contain the build recipe: {name}')
    image = out / 'imagefs_bionic.txz'
    image_record = json.loads((out / 'imagefs-record.json').read_text())
    media = image_record['gstLibavBuild']
    if source['sources'].get('gstreamer', {}).get('commit') != media['pin']:
        raise ValueError('Source archive does not contain matching GStreamer')
    if source['files'].get('project/tools/xrgame/build-gst-libav.sh', {}).get('sha256') != media['recipeSha256']:
        raise ValueError('Source archive does not contain the GStreamer build recipe')
    image_sha = sha(image)
    release = f'https://github.com/tencentmalos/xrgame-native/releases/download/{args.tag}/'
    catalog['items']['imagefs'] = [{
        'id': 'imagefs-bionic-20260926-xrg1', 'name': 'XRGame bionic base filesystem',
        'url': release + image.name, 'sha256': image_sha, 'variant': 'bionic', 'arch': 'aarch64',
        'license': 'Mixed; see xrgame-imagefs.json and usr/share/licenses',
        'source': release + 'runtime-sources.tar.xz',
    }]
    artifacts = {item['file']: item for item in record['artifacts']}
    artifacts[image.name] = {'file': image.name, 'sha256': image_sha, 'size': image.stat().st_size}
    for name, item in artifacts.items():
        if Path(name).name != name or (out / name).is_symlink() or sha(out / name) != item['sha256']:
            raise ValueError(f'Artifact changed or escaped: {name}')
    record.update(artifacts=list(artifacts.values()), published=False, releaseReady=False,
                  sourceArchive={'file': source_archive.name, 'sha256': source['archiveSha256']},
                  sourceIndexSha256=sha(args.sources / 'runtime-source-index.json'),
                  imagefsRecordSha256=sha(out / 'imagefs-record.json'),
                  pending=['Termux dependency source attribution and complete notices',
                           'Rebuild from complete corresponding source bundle',
                           'Release authorization and production catalog activation'])
    if args.dependency_sources:
        index_path = args.dependency_sources / 'dependency-source-index.json'
        dependency = json.loads(index_path.read_text())
        dependency_archive = args.dependency_sources / 'dependency-sources.tar.xz'
        if (dependency['archive'] != dependency_archive.name or
                dependency['archiveSha256'] != sha(dependency_archive) or
                dependency['imagefsRecordSha256'] != record['imagefsRecordSha256']):
            raise ValueError('Dependency sources do not match the imagefs build record')
        record['dependencySources'] = {'file': dependency_archive.name, 'sha256': sha(dependency_archive),
                                       'indexSha256': sha(index_path)}
    (out / 'manifest.draft.json').write_text(json.dumps(catalog, indent=2) + '\n')
    record['catalogSha256'] = sha(out / 'manifest.draft.json')
    (out / 'build-record.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    (out / 'SHA256SUMS').write_text(''.join(
        f"{item['sha256']}  {item['file']}\n" for item in record['artifacts']))
    print(json.dumps({'components': len(record['artifacts']), 'catalogSha256': record['catalogSha256'],
                      'sourceArchiveSha256': source['archiveSha256'],
                      'imagefsPackages': len(image_record['packages']), 'releaseReady': False}))


if __name__ == '__main__':
    main()
