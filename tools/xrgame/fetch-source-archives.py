#!/usr/bin/env python3
"""Fetch hash-pinned dependency sources. Retain failures and never declare release readiness."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import urlsplit


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fetch(entry, output):
    url, expected = entry['url'], entry['sha256']
    if not re.fullmatch(r'[0-9a-f]{64}', expected):
        raise ValueError('Missing or invalid source SHA')
    parsed = urlsplit(url)
    if parsed.scheme not in {'https', 'http'} or parsed.username or parsed.password:
        raise ValueError('Expected public source URL')
    # Older recipes still list HTTP; transport is upgraded, bytes must match.
    url = 'https:' + url.split(':', 1)[1]
    path = output / expected
    if path.exists():
        if digest(path) != expected:
            raise ValueError(f'Existing source cache is corrupt: {expected}')
        return {'sha256': expected, 'url': entry['url'], 'bytes': path.stat().st_size, 'verified': True}
    partial = path.with_suffix('.part')
    errors = []
    for transport in [url, *entry.get('mirrors', [])]:
        mirror = urlsplit(transport)
        if mirror.scheme != 'https' or mirror.username or mirror.password:
            raise ValueError('Expected HTTPS source mirror')
        result = subprocess.run(['curl', '--fail', '--location', '--silent', '--show-error',
                                 '--proto', '=https', '--proto-redir', '=https', '--retry', '2',
                                 '--connect-timeout', '20', '--max-time', '180', transport, '-o', str(partial)],
                                capture_output=True, text=True)
        if not result.returncode:
            break
        errors.append(result.stderr.strip())
    else:
        raise RuntimeError('; '.join(errors))
    if digest(partial) != expected:
        raise ValueError(f'Source SHA mismatch: {entry["url"]}; retained {partial}')
    partial.replace(path)
    return {'sha256': expected, 'url': entry['url'], 'transportUrl': transport,
            'bytes': path.stat().st_size, 'verified': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', type=Path, default=Path(__file__).with_name('termux-sources.lock.json'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    entries = [lock['recipeArchive']]
    for package in lock['packages'].values():
        entries.extend(package['sources'])
    for override in lock.get('recipeOverrides', {}).values():
        entries.extend(override.values())
    entries = list({entry['sha256']: entry for entry in entries}.values())
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'lockSha256': digest(args.lock), 'verified': [], 'failed': [],
              'completeCorrespondingSource': False}
    with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, 12))) as pool:
        futures = {pool.submit(fetch, entry, args.output): entry for entry in entries}
        for future in as_completed(futures):
            entry = futures[future]
            try:
                report['verified'].append(future.result())
            except Exception as error:
                report['failed'].append({**entry, 'error': str(error)})
                print(f'Failed: {entry["url"]}: {error}', flush=True)
            for key in ['verified', 'failed']:
                report[key].sort(key=lambda item: item['sha256'])
            (args.output / 'fetch-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"{len(report['verified'])} verified, {len(report['failed'])} failed", flush=True)
    return 1 if report['failed'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
