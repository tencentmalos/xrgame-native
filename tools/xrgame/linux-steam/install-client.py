#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Download the official ARM64 Steam client directly from Valve into a NEW directory.

Selection follows the pinned DroidDeck installer. No Valve binaries are bundled
in SteamPSP. This bootstrap never repairs/removes an existing account directory.
The Steam client owns subsequent updates and login.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import urllib.parse
import urllib.request
import zipfile

CDN = 'https://client-update.fastly.steamstatic.com/'
MANIFEST = 'steam_client_steamdeck_publicbeta_linuxarm64'
COMPAT_MANIFEST = 'steam_client_steamdeck_publicbeta_win64'
COMPAT_FILES = frozenset(('steamclient.dll', 'steamclient64.dll', 'Steam.dll',
                          'tier0_s.dll', 'tier0_s64.dll', 'vstdlib_s.dll', 'vstdlib_s64.dll'))
MAX_ARCHIVE = 512 * 1024 * 1024
MAX_EXTRACTED = 3 * 1024 * 1024 * 1024


def valve_url(url):
    p = urllib.parse.urlsplit(url)
    if (p.scheme != 'https' or not p.hostname or
        not (p.hostname == 'steamstatic.com' or p.hostname.endswith('.steamstatic.com')) or
        p.username or p.password or p.port not in (None, 443) or p.query or p.fragment):
        raise ValueError('Only Valve HTTPS client CDN URLs are allowed')
    return url


class ValveRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return super().redirect_request(req, fp, code, msg, headers, valve_url(newurl))


def fetch(url, target, limit, expected=None):
    digest = hashlib.sha256()
    total = 0
    opener = urllib.request.build_opener(ValveRedirect())
    with opener.open(valve_url(url), timeout=45) as response, target.open('wb') as output:
        while chunk := response.read(128 * 1024):
            total += len(chunk)
            if total > limit:
                raise ValueError('Download exceeds declared size')
            digest.update(chunk)
            output.write(chunk)
    if expected and (total != limit or digest.hexdigest() != expected):
        raise ValueError('Valve component size/SHA-256 mismatch')
    return digest.hexdigest()


def file_sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def parts(manifest, platform='linuxarm64'):
    # A deliberately limited VDF reader: quoted scalars and nested objects only.
    # Reject unparsed characters and duplicate keys instead of accepting a partial manifest.
    tokens = re.findall(r'"[^"\\\x00-\x1f]*"|[{}]|\S+', manifest)
    pos = 0
    def quoted():
        nonlocal pos
        if pos >= len(tokens) or not re.fullmatch(r'"[^"\\\x00-\x1f]*"', tokens[pos]):
            raise ValueError('Invalid Valve manifest token')
        value = tokens[pos][1:-1]
        pos += 1
        return value
    def obj():
        nonlocal pos
        result = {}
        while pos < len(tokens) and tokens[pos] != '}':
            key = quoted()
            if key in result or pos >= len(tokens):
                raise ValueError('Duplicate/incomplete Valve manifest field')
            if tokens[pos] == '{':
                pos += 1
                value = obj()
            else:
                value = quoted()
            result[key] = value
        if pos == len(tokens):
            raise ValueError('Unterminated Valve manifest')
        pos += 1
        return result
    if platform not in ('linuxarm64', 'win64') or quoted() != platform or pos >= len(tokens) or tokens[pos] != '{':
        raise ValueError('Wrong Valve client manifest platform')
    pos += 1
    root = obj()
    # Valve appends signature objects. Download authenticity here relies on
    # HTTPS; do not claim cryptographic signature verification without Valve's key.
    signatures = set()
    while pos < len(tokens):
        key = quoted()
        if key not in ('kvsign2', 'kvsignatures') or key in signatures or pos >= len(tokens) or tokens[pos] != '{':
            raise ValueError('Unexpected manifest trailer')
        signatures.add(key)
        pos += 1
        obj()
    if not re.fullmatch(r'[0-9]+', str(root.get('version', ''))):
        raise ValueError('Invalid manifest version/trailing data')
    selected = []
    for name, item in root.items():
        wanted = (name == 'bins_win64') if platform == 'win64' else (
            name.endswith('_all') or name.endswith('_linuxarm64_linuxarm64') or name == 'bins_androidarm64_linuxarm64')
        if not wanted:
            continue
        if not isinstance(item, dict):
            raise ValueError('Invalid component')
        filename, digest = item.get('file', ''), item.get('sha2', '')
        size = int(item.get('size', '0'))
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', filename) or not re.fullmatch(r'[0-9a-f]{64}', digest) or not 0 < size <= MAX_ARCHIVE:
            raise ValueError('Unsafe/incomplete component metadata')
        selected.append(dict(name=name, file=filename, sha256=digest, size=size))
    required = 'bins_win64' if platform == 'win64' else 'bins_linuxarm64_linuxarm64'
    if not any(x['name'] == required for x in selected):
        raise ValueError('Missing client component: ' + required)
    return root['version'], sorted(selected, key=lambda x: x['name'])


def extract(archive, root, remaining, allow_files=None):
    with zipfile.ZipFile(archive) as z:
        for member in z.infolist():
            name = member.filename.replace('\\', '/')
            if allow_files is not None and name not in allow_files:
                continue
            path = PurePosixPath(name)
            mode = member.external_attr >> 16
            if (not name or path.is_absolute() or '..' in path.parts or ':' in name or
                any(ord(c) < 32 for c in name)):
                raise ValueError('Unsafe Valve archive member: ' + name)
            target = root.joinpath(*path.parts)
            if not target.resolve().is_relative_to(root.resolve()):
                raise ValueError('Archive path escapes destination')
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            if stat.S_ISLNK(mode):
                if member.file_size > 4096:
                    raise ValueError('Oversized archive symlink')
                link = z.read(member).decode('utf-8')
                if (not link or PurePosixPath(link).is_absolute() or '\\' in link or ':' in link or
                    any(ord(c) < 32 for c in link) or
                    not (target.parent / link).resolve().is_relative_to(root.resolve())):
                    raise ValueError('Archive symlink escapes destination')
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.is_symlink() and os.readlink(target) == link:
                    continue
                if target.exists() or target.is_symlink():
                    raise ValueError('Archive symlink conflicts with existing member')
                target.symlink_to(link)
                continue
            remaining -= member.file_size
            if remaining < 0:
                raise ValueError('Valve client exceeds extraction limit')
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(member) as source, target.open('wb') as output:
                shutil.copyfileobj(source, output)
            target.chmod(0o700 if mode & 0o111 else 0o600)
    return remaining


def cached_component(component, cache):
    archive = cache / component['sha256']
    valid = (archive.is_file() and archive.stat().st_size == component['size'] and
             file_sha256(archive) == component['sha256'])
    if not valid:
        partial = archive.with_suffix('.part')
        fetch(CDN + component['file'], partial, component['size'], component['sha256'])
        os.replace(partial, archive)
    return archive


def install_compat(destination, cache, progress=print):
    """Official PE ABI facades for Wine, not a Windows Steam desktop replacement."""
    if destination.exists() or destination.is_symlink():
        raise ValueError('Refusing to overwrite an existing compatibility directory')
    cache.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.steam-compat-', dir=destination.parent) as work:
        work = Path(work)
        manifest = work / 'manifest.vdf'
        manifest_sha = fetch(CDN + COMPAT_MANIFEST, manifest, 1024 * 1024)
        version, components = parts(manifest.read_text(), 'win64')
        stage = work / 'legacycompat'; stage.mkdir()
        for component in components:
            progress('Downloading official Windows Steam ABI facades')
            extract(cached_component(component, cache), stage, MAX_ARCHIVE, COMPAT_FILES)
        if {p.name for p in stage.iterdir()} != COMPAT_FILES:
            raise ValueError('Incomplete Windows Steam ABI facades')
        for name, machine in (('steamclient64.dll', 0x8664), ('steamclient.dll', 0x14c)):
            with (stage / name).open('rb') as source:
                header = source.read(64)
                if header[:2] != b'MZ': raise ValueError('Expected Windows Steam PE image')
                source.seek(int.from_bytes(header[60:64], 'little'))
                pe = source.read(6)
                if pe[:4] != b'PE\0\0' or int.from_bytes(pe[4:6], 'little') != machine:
                    raise ValueError('Wrong Steam compatibility architecture')
        record = dict(version=version, manifest_sha256=manifest_sha, components=components,
                      files={p.name: file_sha256(p) for p in stage.iterdir()})
        (stage / 'steampsp-compat.json').write_text(json.dumps(record, indent=2) + '\n')
        os.rename(stage, destination)
        return record


def valid_compat(destination):
    try:
        if destination.is_symlink():
            return False
        record = json.loads((destination / 'steampsp-compat.json').read_text())
        return set(record['files']) == COMPAT_FILES and all(
            (destination / name).is_file() and not (destination / name).is_symlink() and
            file_sha256(destination / name) == digest for name, digest in record['files'].items())
    except (OSError, ValueError, KeyError, TypeError):
        return False


def ensure_compat(destination, cache):
    """Our verified PE cache lives outside Steam's updater-owned legacycompat directory."""
    if valid_compat(destination):
        return
    if destination.is_symlink() or (destination.exists() and not (destination / 'steampsp-compat.json').is_file()):
        raise ValueError('Unrecognized compatibility cache; files preserved')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.steampsp-abi-', dir=destination.parent) as temporary:
        stage = Path(temporary) / 'verified'
        install_compat(stage, cache)
        if not valid_compat(stage):
            raise ValueError('Compatibility cache verification failed')
        backup = Path(temporary) / 'previous'
        previous = destination.exists()
        if previous:
            destination.rename(backup)
        try:
            stage.rename(destination)
        except OSError:
            if previous:
                backup.rename(destination)
            raise


def install(destination, progress=print):
    destination = destination.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError('Steam directory already exists; client-owned updates must preserve account data')
    destination.parent.mkdir(parents=True, exist_ok=True)
    cache = destination.parent / '.steampsp-valve-cache'
    cache.mkdir(mode=0o700, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.steam-install-', dir=destination.parent) as work:
        work = Path(work)
        manifest = work / 'manifest.vdf'
        manifest_sha = fetch(CDN + MANIFEST, manifest, 1024 * 1024)
        version, components = parts(manifest.read_text())
        stage = work / 'client'
        stage.mkdir()
        remaining = MAX_EXTRACTED
        for component in components:
            progress('Downloading ' + component['name'])
            archive = cached_component(component, cache)
            remaining = extract(archive, stage, remaining)
        executable = stage / 'steamrtarm64/steam'
        with executable.open('rb') as stream:
            header = stream.read(20)
        if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183:
            raise ValueError('Valve client is not ARM64 ELF')
        executable.chmod(0o700)
        for name in ('steamwebhelper', 'reaper', 'steamsysinfo', 'gldriverquery', 'vulkandriverquery'):
            target = stage / 'steamrtarm64' / name
            if target.is_file(): target.chmod(0o700)
        (stage / 'package').mkdir(exist_ok=True)
        (stage / 'package/beta').write_text('steamdeck_publicbeta\n')
        compat = install_compat(stage / 'legacycompat', cache, progress)
        (stage / 'steampsp-bootstrap.json').write_text(json.dumps(dict(schema=1, version=version,
            manifest_sha256=manifest_sha, components=components, windows_compat=compat), indent=2) + '\n')
        # No username, credential, token, or login configuration is created here.
        os.rename(stage, destination)
        destination.chmod(0o700)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compat-only', action='store_true')
    parser.add_argument('destination', type=Path)
    arguments = parser.parse_args()
    if arguments.compat_only:
        ensure_compat(arguments.destination, arguments.destination.parent / 'home/.steampsp-valve-cache')
    else:
        install(arguments.destination)
