#!/usr/bin/env bash
# Incremental validation build of the PE lsteamclient.dll modules (aarch64-windows, i386-windows) with
# the Steamworks SDK 1.65 patch, using the already configured pinned Android Wine tree. The unix side is
# built by linux-steam/build-wine-client.py from the same patch; package-proton-validation.py
# --release linux-steam2 puts both into the Proton archive.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
work="$base/build/proton-wine"
patch="$project/tools/xrgame/patches/lsteamclient-sdk165.patch"
test "$(git -C "$base/src/proton-wine" rev-parse HEAD)" = 5d0d333e7beef02fbb455cc3163b4e8e25615458
if git -C "$work/source" apply --check "$patch" 2>/dev/null; then
    git -C "$work/source" apply "$patch"
else
    git -C "$work/source" apply --reverse --check "$patch"
fi
source "$work/environment.sh"
# The configured Makefile names ccache for the unix compiler; a pass-through changes no output.
if ! command -v ccache >/dev/null; then
    mkdir -p /tmp/ccache-shim
    printf '#!/bin/sh\nexec "$@"\n' > /tmp/ccache-shim/ccache
    chmod +x /tmp/ccache-shim/ccache
    export PATH=/tmp/ccache-shim:$PATH
fi
make -C "$work/android" -j"${XRGAME_JOBS:-12}" \
    lsteamclient/aarch64-windows/lsteamclient.dll lsteamclient/i386-windows/lsteamclient.dll
out="$base/output/lsteamclient-pe"
mkdir -p "$out"
python3 - "$work/android/lsteamclient" "$out" "$patch" "$(realpath "$0")" "$work/environment.sh" <<'PY'
import hashlib, json, pathlib, shutil, sys
src, out, patch, recipe, env = map(pathlib.Path, sys.argv[1:])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
modules = {}
for arch in ['aarch64-windows', 'i386-windows']:
    dll = src / arch / 'lsteamclient.dll'
    assert b'SteamUtils011' in dll.read_bytes()
    target = out / arch / dll.name
    target.parent.mkdir(exist_ok=True)
    shutil.copy2(dll, target)
    modules[f'lib/wine/{arch}/lsteamclient.dll'] = {'sha256': sha(target), 'size': target.stat().st_size}
record = {'sourcePin': '5d0d333e7beef02fbb455cc3163b4e8e25615458',
          'patchSha256': sha(patch), 'recipeSha256': sha(recipe), 'environmentSha256': sha(env),
          'modules': modules, 'releaseReady': False}
(out / 'lsteamclient-pe-build.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record))
PY
