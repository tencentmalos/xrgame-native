#!/usr/bin/env python3
"""Package Turnip xrg17: the validated xrg16 archive with both drivers rebuilt at mesa 55186f1f.

build-turnip.sh now pins mesa-mirror malos/main 55186f1f (KGSL device timestamps from
IOCTL_KGSL_READ_CALIBRATED_TIMESTAMPS; calibrated timestamps and present timing advertised only
when the probe succeeds) with the same patches as xrg16, so the X11/AHB driver and the app-process
Android driver are the changed files; the XCB libraries, ICD JSON and licences are copied from
xrg16 unchanged. The result is a private validation component.

  package-turnip-validation.py --base turnip-adb7e30-xrg16.zip --native libvulkan_freedreno.so \
      --android libvulkan_freedreno_android.so --strip <ndk>/llvm-strip \
      --readobj <ndk>/llvm-readobj --output DIR
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import zipfile

BASE = 'turnip-adb7e30-xrg16.zip'
# xrg16 as packaged on 2026-10-15 from the build host's drivers (runtime-20261015-private).
BASE_SHA256 = '20b24d3071ece86cce7c759e1038c74b97358b6853a6440966bd91e7b1fea52f'
# xrg16 as bundled in the v14 APK (runtime-20261017-wsl-private, from the Apple Container xrg13
# baseline); same XCB libraries and licences, different ICD JSON. Internal, never a public release.
LOCAL_BASE_SHA256 = 'be6fbc76a4b7522ae891424d7135befaed807f4d469420a254129ade67385340'
VERSION = 'turnip-55186f1-xrg17'
PIN = '55186f1f0e739a507876bb3733cf00e1734c78f1'
PATCHES = ('turnip-fdm-visualize.patch', 'turnip-max-samples.patch')
MARKERS = (b'fdmviz\0', b'TU_MAX_SAMPLES\0')
VALIDATION = 'Private validation build; see docs/validation/turnip-kgsl-calibrated-timestamps-20261019.md.'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def text_bytes(path):
    return path.read_bytes().replace(b'\r\n', b'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--android', type=Path, required=True)
    parser.add_argument('--strip', type=Path, required=True)
    parser.add_argument('--readobj', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    base_sha = sha(args.base.read_bytes())
    if base_sha not in (BASE_SHA256, LOCAL_BASE_SHA256):
        raise SystemExit('Expected the validated Turnip xrg16 archive')
    with zipfile.ZipFile(args.base) as source:
        files = {info.filename: source.read(info) for info in source.infolist()}
    inventory = json.loads(files.pop('xrgame-build.json'))
    rebuilt = {}
    with tempfile.TemporaryDirectory() as temporary:
        for name, built in (('libvulkan_freedreno.so', args.native), ('libvulkan_freedreno_android.so', args.android)):
            library = Path(temporary) / name
            shutil.copy2(built, library)
            subprocess.run([str(args.strip), '--strip-debug', str(library)], check=True)
            data = library.read_bytes()
            if not all(marker in data for marker in MARKERS):
                raise SystemExit(f'{name} was not built with {PATCHES}')
            headers = subprocess.check_output([str(args.readobj), '--file-headers', '--notes', str(library)],
                                              text=True).replace(temporary, '.')
            rebuilt[name] = (data, headers)
    for name, (data, headers) in rebuilt.items():
        files[name] = data
        entry = dict(inventory['files'][name], sha256=sha(data), size=len(data))
        if 'objectHeaders' in entry:
            entry['objectHeaders'] = headers
        inventory['files'][name] = entry
    meta = json.loads(files['meta.json'])
    meta.update(name=VERSION, driverVersion=VERSION)
    files['meta.json'] = (json.dumps(meta, indent=2, sort_keys=True) + '\n').encode()
    inventory['files']['meta.json'] = {'sha256': sha(files['meta.json']), 'size': len(files['meta.json'])}
    for name in tuple('patches/' + patch for patch in PATCHES) + ('build-turnip.sh', Path(__file__).name):
        inventory['recipes']['tools/xrgame/' + name] = sha(text_bytes(here / name))
    inventory['component'] = VERSION
    inventory['sourcePins'] = dict(inventory['sourcePins'], **{'mesa-turnip': PIN})
    inventory['derivedFrom'] = {'file': BASE, 'sourcePin': PIN, 'rebuilt': sorted(rebuilt), 'sha256': base_sha}
    inventory['validation'] = VALIDATION
    files['xrgame-build.json'] = (json.dumps(inventory, indent=2, sort_keys=True) + '\n').encode()
    args.output.mkdir(parents=True, exist_ok=True)
    dest = args.output / (VERSION + '.zip')
    partial = dest.with_suffix('.zip.part')
    inspect = runpy.run_path(str(here / 'verify-runtime-packages.py'))['inspect']
    try:
        # Adrenotools' importer expects a flat ZIP, as package-components.py writes it.
        with zipfile.ZipFile(partial, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
            for name in sorted(files):
                entry = zipfile.ZipInfo(name, (2026, 9, 26, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = 0o100644 << 16
                bundle.writestr(entry, files[name])
        final = partial.with_name(dest.name)
        partial.replace(final)
        inspect(final, 'driver')
    except BaseException:
        partial.unlink(missing_ok=True)
        dest.unlink(missing_ok=True)
        raise
    print(json.dumps({'file': dest.name, 'id': VERSION, 'sha256': sha(dest.read_bytes()),
                      'size': dest.stat().st_size,
                      'libraries': {name: sha(data) for name, (data, _) in rebuilt.items()}}, indent=2))


if __name__ == '__main__':
    main()
