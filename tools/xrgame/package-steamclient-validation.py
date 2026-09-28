#!/usr/bin/env python3
"""Package pinned source-built GBE clients with a recorded loader for private validation."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

PIN = '7a319f0bedad260f952b0fb367b27f255fd952c5'
LOADER_PIN = '8479ddbc096f7d8a418ec28f463c829b77f66911'
HASHES = {
    'steamclient64.dll': '361668503b497fdad681e789dd708837a9b816eed912f72241c34ad41e2e8982',
    'extra/steamclient_extra_x64.dll': '84dcde1ee0a829fa94b46fb83a110db462816735a89c9219fae4f5d74f18f2ec',
    'steamclient_loader_x64.exe': '26463c57a73b7a1ed4bc6265b7a42ae9d4b8ea065481c64e14a46686673859fd',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-built', type=Path, required=True)
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--loader-build-record', type=Path, help='Use the source loader produced by build-steamclient-loader.sh')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    payload = {}
    hashes = dict(HASHES)
    loader_record = None
    if args.loader_build_record:
        loader_record = json.loads(args.loader_build_record.read_text())
        if (loader_record.get('kind') != 'source-built-validation-loader' or loader_record.get('commit') != PIN or
                loader_record.get('sourceArchiveSha256') != '3294bc129ec6b41bfe2e8229c65aeb438591ff2c53a066ed6f8e36a677f04d08'):
            raise ValueError('Unrecognized source loader build')
        hashes['steamclient_loader_x64.exe'] = loader_record['sha256']
    for name, expected in hashes.items():
        src = args.loader if name.endswith('.exe') else args.source_built / Path(name).name
        data = src.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Unrecognized validation input: {name}')
        payload[name] = data
    payload['LICENSE'] = (root / 'references/gbe_fork/LICENSE').read_bytes()
    payload['provenance.json'] = (json.dumps({
        'kind': 'source-built-validation' if loader_record else 'mixed-build-validation', 'releaseReady': False,
        'clientSource': f'https://github.com/tencentmalos/gbe_fork/tree/{PIN}',
        'clientCommit': PIN, 'clientBuild': 'MSVC source build; source-build-identity.json in private evidence',
        'loaderCommit': PIN if loader_record else LOADER_PIN, 'loaderRelease': None if loader_record else '2026_09_16_2',
        'loaderBuild': loader_record if loader_record else 'fixed upstream release; NOT built by this recipe',
        'loaderArchiveSha256': None if loader_record else 'd311deadc2a8a8aed620fe66976646059388123587aa22d408f723c592fc9688',
        'sha256': hashes,
        'license': 'LGPL-3.0-or-later; complete dependency notices remain a release prerequisite',
    }, indent=2, sort_keys=True) + '\n').encode()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 9, 26, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    print(json.dumps({'file': args.output.name, 'sha256': hashlib.sha256(args.output.read_bytes()).hexdigest(),
                      'size': args.output.stat().st_size, 'releaseReady': False}))


if __name__ == '__main__':
    main()
