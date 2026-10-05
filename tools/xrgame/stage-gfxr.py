#!/usr/bin/env python3
"""Stage GFXReconstruct capture binaries for picoXr debug APKs (docs/specs/xrgame-native-api-replay-v1.md).

    stage-gfxr.py --win-dir DIR --android-layer SO [--source TEXT]

DIR holds the static-CRT Windows x64 d3d12.dll, dxgi.dll and d3d12_capture.dll; SO is the stripped
Android arm64 libVkLayer_gfxreconstruct.so. Output: build/xrgame-gfxr/xrgame-gfxr/ (gitignored), which
Gradle adds to picoXrDebug assets and tools/audit-apk compares byte for byte.
"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = 'tools/xrgame/patches/gfxreconstruct-wine-capture.patch'
WINDOWS = ('d3d12.dll', 'dxgi.dll', 'd3d12_capture.dll')


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--win-dir', type=Path, required=True)
    parser.add_argument('--android-layer', type=Path, required=True)
    parser.add_argument('--source', default='tencentmalos/gfxreconstruct feature/malos/xrgame-wine-capture 3868cd12')
    args = parser.parse_args()
    out = ROOT/'build/xrgame-gfxr/xrgame-gfxr'
    if out.exists():
        shutil.rmtree(out)
    (out/'win-x64').mkdir(parents=True)
    (out/'android-arm64').mkdir()
    for name in WINDOWS:
        data = (args.win_dir/name).read_bytes()
        # Static CRT only: Wine's builtin msvcp140/vcruntime140 are not a dependency of the capture path.
        for runtime in (b'MSVCP140', b'VCRUNTIME140', b'msvcp140', b'vcruntime140'):
            if runtime + b'.dll' in data or runtime + b'_1.dll' in data:
                raise SystemExit(f'{name} links the dynamic MSVC runtime; rebuild with CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded')
        (out/'win-x64'/name).write_bytes(data)
    layer = args.android_layer.read_bytes()
    if not layer.startswith(b'\x7fELF'):
        raise SystemExit('Android layer is not an ELF file')
    (out/'android-arm64/libVkLayer_gfxreconstruct.so').write_bytes(layer)
    files = {str(path.relative_to(out)).replace('\\', '/'): sha256(path)
             for path in sorted(out.rglob('*')) if path.is_file()}
    manifest = {'schema': 1, 'source': args.source, 'patch': PATCH, 'patchSha256': sha256(ROOT/PATCH),
                'license': 'MIT', 'scope': 'picoXr debug validation APK only', 'files': files}
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
