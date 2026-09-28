#!/usr/bin/env python3
"""Independently verify archive hashes, per-file records and runtime link targets."""
import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import tarfile
from urllib.parse import urlsplit
import zipfile


def normalized(name):
    path = posixpath.normpath(name)
    if path.startswith('/') or path == '..' or path.startswith('../'):
        raise ValueError(f'Escaping archive path: {name}')
    return path


def inspect(path, kind):
    files, links, documents = {}, {}, {}

    def read(name, stream):
        name = normalized(name)
        if name in files or name in links:
            raise ValueError(f'Duplicate archive member: {name}')
        digest, size, content = hashlib.sha256(), 0, bytearray()
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
            size += len(chunk)
            if name in {'xrgame-build.json', 'xrgame-imagefs.json', 'profile.json', 'meta.json', 'provenance.json'}:
                content.extend(chunk)
        files[name] = {'sha256': digest.hexdigest(), 'size': size}
        if content:
            documents[name] = json.loads(content)

    if path.suffix == '.zip':
        with zipfile.ZipFile(path) as archive:
            for member in archive.infolist():
                if not member.is_dir():
                    with archive.open(member) as stream:
                        read(member.filename, stream)
    else:
        with tarfile.open(path, mode='r|*') as archive:
            for member in archive:
                name = normalized(member.name)
                if member.isfile():
                    with archive.extractfile(member) as stream:
                        read(name, stream)
                elif member.issym():
                    if name in files or name in links:
                        raise ValueError(f'Duplicate archive member: {name}')
                    if member.linkname.startswith('/'):
                        raise ValueError(f'Absolute runtime link: {name}')
                    links[name] = member.linkname
                elif not member.isdir():
                    raise ValueError(f'Unsupported archive member: {name}')
    if kind == 'steamclient':
        binaries = {'steamclient_loader_x64.exe', 'steamclient64.dll', 'extra/steamclient_extra_x64.dll'}
        if set(files) != binaries | {'LICENSE', 'provenance.json'} or links:
            raise ValueError('Incomplete or unexpected bundled Steam client files')
        for name in binaries:
            if documents['provenance.json'].get('sha256', {}).get(name) != files[name]['sha256']:
                raise ValueError(f'Steam client provenance mismatch: {name}')
        return {'file': path.name, 'members': len(files), 'recordedFiles': len(binaries)}
    record_name = 'xrgame-imagefs.json' if kind == 'imagefs' else 'xrgame-build.json'
    if record_name not in documents:
        raise ValueError(f'Missing runtime file inventory: {record_name}')
    record = documents[record_name]
    for name, expected in record['files'].items():
        if 'symlink' in expected:
            if links.get(name) != expected['symlink']:
                raise ValueError(f'Link record mismatch: {name}')
        else:
            actual = files.get(name, {})
            if actual.get('sha256') != expected['sha256'] or (
                    'size' in expected and actual.get('size') != expected['size']):
                raise ValueError(f'File record mismatch: {name}')
    for name, target in links.items():
        resolved = normalized(posixpath.join(posixpath.dirname(name), target))
        visited = {name}
        while resolved in links:
            if resolved in visited:
                raise ValueError(f'Link cycle: {name}')
            visited.add(resolved)
            resolved = normalized(posixpath.join(posixpath.dirname(resolved), links[resolved]))
        if resolved not in files:
            raise ValueError(f'Unresolved runtime link: {name}')
    required = {
        'proton': {'lib/wine/aarch64-unix/wine', 'lib/wine/aarch64-windows/gamenative_xr_unixbridge.dll',
                   'lib/wine/aarch64-windows/d3d10.dll', 'lib/wine/aarch64-windows/d3d10_1.dll',
                   'lib/wine/aarch64-windows/d3dcompiler_47.dll'},
        'fexcore': {'libarm64ecfex.dll', 'libarm64ecfex.so', 'libwow64fex.dll', 'libwow64fex.so'},
        'dxvk': {'d3d8.dll', 'd3d9.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll'},
        'vkd3d': {'d3d12.dll', 'd3d12core.dll'},
        'driver': {'libvulkan_freedreno.so'},
        'imagefs': {'usr/lib/libxcb.so', 'usr/lib/libandroid-sysvshm.so', 'usr/etc/fonts/fonts.conf',
                    'usr/lib/libEGL.so.1', 'usr/lib/gstreamer-1.0/libgstcoreelements.so',
                    'usr/lib/gstreamer-1.0/libgsttypefindfunctions.so',
                    'usr/lib/gstreamer-1.0/libgstplayback.so',
                    'usr/lib/gstreamer-1.0/libgstdebug.so',
                    'usr/lib/gstreamer-1.0/libgstdeinterlace.so',
                    'usr/lib/gstreamer-1.0/libgstvideofilter.so',
                    'usr/lib/gstreamer-1.0/libgstisomp4.so', 'usr/lib/gstreamer-1.0/libgstlibav.so'},
    }
    if missing := required[kind] - files.keys():
        raise ValueError(f'Missing runtime files: {sorted(missing)}')
    if kind == 'proton' and links.get('bin/wine') != '../lib/wine/aarch64-unix/wine':
        raise ValueError('Missing Wine loader link')
    if kind == 'fexcore':
        targets = {entry['target'] for entry in documents['profile.json']['files']}
        if not {'${system32}/libarm64ecfex.dll', '${libdir}/wine/aarch64-unix/libarm64ecfex.so'} <= targets:
            raise ValueError('Wrong FEX installation targets')
    if kind == 'driver' and documents['meta.json']['libraryName'] not in files:
        raise ValueError('Driver metadata points to a missing library')
    return {'file': path.name, 'members': len(files) + len(links), 'recordedFiles': len(record['files'])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packages', type=Path, required=True)
    args = parser.parse_args()
    catalog = json.loads((args.packages / 'manifest.draft.json').read_text())
    results = []
    for kind, entries in catalog['items'].items():
        for entry in entries:
            name = urlsplit(entry['url']).path.rsplit('/', 1)[1]
            if not name or name in {'.', '..'}:
                raise ValueError('Invalid component filename')
            path = args.packages / name
            with path.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != entry['sha256']:
                    raise ValueError(f'Archive SHA mismatch: {name}')
            results.append(inspect(path, kind))
    report = {'passed': True, 'archives': results, 'deviceExecutionVerified': False}
    (args.packages / 'package-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
