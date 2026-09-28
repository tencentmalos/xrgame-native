#!/usr/bin/env bash
# Incremental validation build, using the already configured pinned Android Wine tree.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
work="$base/build/proton-wine"
patch="$project/tools/xrgame/patches/wine-image-bootstrap.patch"
test "$(git -C "$base/src/proton-wine" rev-parse HEAD)" = 5d0d333e7beef02fbb455cc3163b4e8e25615458
if git -C "$work/source" apply --check "$patch" 2>/dev/null; then
    git -C "$work/source" apply "$patch"
else
    git -C "$work/source" apply --reverse --check "$patch"
fi
source "$work/environment.sh"
make -C "$work/android" -j"${XRGAME_JOBS:-12}" dlls/ntdll/all
out="$base/output/wine-bootstrap"
mkdir -p "$out"
python3 - "$work/android/dlls/ntdll" "$out" "$patch" "$(realpath "$0")" "$work/environment.sh" <<'PY'
import hashlib, json, pathlib, shutil, subprocess, sys
src, out, patch, recipe, env = map(pathlib.Path, sys.argv[1:])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
modules = {}
for arch in ['aarch64-windows', 'i386-windows']:
    dll = src / arch / 'ntdll.dll'
    assert 'XRGAME_BOOTSTRAP_IMAGE'.encode('utf-16le') in dll.read_bytes()
    target = out / arch / dll.name
    target.parent.mkdir(exist_ok=True)
    shutil.copy2(dll, target)
    modules[f'lib/wine/{arch}/ntdll.dll'] = {'sha256': sha(target), 'size': target.stat().st_size}
record = {'sourcePin': '5d0d333e7beef02fbb455cc3163b4e8e25615458',
          'patchSha256': sha(patch), 'recipeSha256': sha(recipe), 'environmentSha256': sha(env),
          'modules': modules, 'releaseReady': False}
(out / 'bootstrap-build.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record))
PY
