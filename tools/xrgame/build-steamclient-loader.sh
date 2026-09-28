#!/usr/bin/env bash
# Build the fixed GBE loader on Linux; no Windows host or downloaded loader required.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
pin=7a319f0bedad260f952b0fb367b27f255fd952c5
archive_sha=3294bc129ec6b41bfe2e8229c65aeb438591ff2c53a066ed6f8e36a677f04d08
archive=${1:?Pass the git archive of the fixed GBE loader sources (see README)}
recipe=$(realpath "$0")
patch=$(dirname "$recipe")/patches/gbe-loader-lifetime.patch
compiler=${XRGAME_MINGW_CXX:-"$base/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin/x86_64-w64-mingw32-clang++"}
test "$(sha256sum "$archive" | cut -d' ' -f1)" = "$archive_sha"
source_dir=$(mktemp -d "$base/gbe-loader-$pin-XXXXXXXX")
out="$base/output/gbe-loader"
mkdir -p "$out"
tar xf "$archive" -C "$source_dir"
git -C "$source_dir" apply "$patch"
git -C "$source_dir" apply "$(dirname "$recipe")/patches/gbe-loader-remote-export.patch"
git -C "$source_dir" apply "$(dirname "$recipe")/patches/gbe-loader-process-family.patch"
cd "$source_dir"
"$compiler" -std=c++17 -O1 -g -fno-omit-frame-pointer -static -municode -mwindows \
    -ffile-prefix-map="$source_dir"=/xrgame/gbe-loader \
    -Ihelpers -Ilibs tools/steamclient_loader/win/ColdClientLoader.cpp \
    helpers/pe_helpers.cpp helpers/common_helpers.cpp helpers/dbg_log.cpp \
    -o "$out/steamclient_loader_x64.exe" -luser32 -ladvapi32
python3 - "$out" "$pin" "$archive_sha" "$compiler" "$recipe" "$patch" <<'PY'
import hashlib, json, pathlib, subprocess, sys
out, pin, source_sha, compiler, recipe, patch = sys.argv[1:]
digest = lambda p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
record = {'kind': 'source-built-validation-loader', 'commit': pin,
          'sourceArchiveSha256': source_sha, 'compilerSha256': digest(compiler),
          'compilerVersion': subprocess.check_output([compiler, '--version']).decode().splitlines()[0],
          'recipeSha256': digest(recipe), 'patchSha256': digest(patch), 'debugLogging': True,
          'remoteExportPatchSha256': digest(pathlib.Path(patch).with_name('gbe-loader-remote-export.patch')),
          'processFamilyPatchSha256': digest(pathlib.Path(patch).with_name('gbe-loader-process-family.patch')),
          'sha256': digest(pathlib.Path(out) / 'steamclient_loader_x64.exe'),
          'releaseReady': False}
(pathlib.Path(out) / 'loader-build.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record))
PY
