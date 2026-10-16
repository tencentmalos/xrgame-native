#!/usr/bin/env python3
"""Package Turnip xrg16: the validated xrg13 archive with both drivers rebuilt.

build-turnip.sh now also applies turnip-fdm-visualize.patch (TU_DEBUG=fdmviz) and
turnip-max-samples.patch (TU_MAX_SAMPLES), so the X11/AHB driver and the app-process Android
driver are the changed files; the XCB libraries, ICD JSON
and licences are copied from xrg13 unchanged. The result is a private validation component.

  package-turnip-validation.py --base turnip-adb7e30-xrg13.zip --native libvulkan_freedreno.so \
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

BASE = 'turnip-adb7e30-xrg13.zip'
BASE_SHA256 = 'f039931cf9b78572a1cd4128893d1fa5ca4458a8b59a1784de308ff955447d8e'
VERSION = 'turnip-adb7e30-xrg16'
PATCHES = ('turnip-fdm-visualize.patch', 'turnip-max-samples.patch')
MARKERS = (b'fdmviz\0', b'TU_MAX_SAMPLES\0')
VALIDATION = 'Private validation build; see docs/validation/swan-alyx-vulkan-20261015.md.'


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
    if sha(args.base.read_bytes()) != BASE_SHA256:
        raise SystemExit('Expected the validated Turnip xrg13 archive')
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
    inventory['derivedFrom'] = {'file': BASE, 'patches': ['tools/xrgame/patches/' + patch for patch in PATCHES],
                                'rebuilt': sorted(rebuilt), 'sha256': BASE_SHA256}
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
