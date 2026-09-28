#!/usr/bin/env bash
# The PE runtime and Android unixlib; the Wine builtin is built with Proton 11.
set -euo pipefail
repository=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?Usage: build-picoxr-xr-payload.sh OUTPUT_DIRECTORY}
sdk=${ANDROID_HOME:-"$HOME/Library/Android/sdk"}
ndk=${ANDROID_NDK_HOME:-"$sdk/ndk/27.3.13750724"}
case "$(uname -s)" in Darwin) host=darwin-x86_64 ;; Linux) host=linux-x86_64 ;; *) exit 2 ;; esac
bin="$ndk/toolchains/llvm/prebuilt/$host/bin"
work=${XRGAME_NATIVE_BUILD:-"$repository/app/build/xrgame-native"}
headers="$work/openxr/prefab/modules/headers/include"
src="$repository/app/src/main/windows/openxr_runtime"
test -f "$headers/openxr/openxr.h"
mkdir -p "$work/pe" "$output"
for lib in ws2_32 kernel32 ntdll dxgi; do
    "$bin/llvm-dlltool" -m i386:x86-64 -d "$src/${lib}_x64.def" -l "$work/pe/lib${lib}_x64.a"
    "$bin/llvm-dlltool" -m i386 -k -d "$src/${lib}_x86.def" -l "$work/pe/lib${lib}_x86.a"
done
for arch in x64 x86; do
    case "$arch" in x64) target=x86_64; bits=64 ;; x86) target=i686; bits=32 ;; esac
    "$bin/clang" --target="$target-w64-windows-gnu" -shared -nostdlib -Wl,-e,DllMain -I "$headers" \
        -o "$output/gamenative_openxr_runtime$bits.dll" \
        "$src/gamenative_openxr_runtime.c" "$src/gamenative_openxr_runtime_$arch.def" \
        "$work/pe/libws2_32_$arch.a" "$work/pe/libkernel32_$arch.a" \
        "$work/pe/libntdll_$arch.a" "$work/pe/libdxgi_$arch.a"
done
"$sdk/cmake/3.22.1/bin/cmake" -S "$src/unix" -B "$work/xr-unixlib" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$sdk/cmake/3.22.1/bin/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--build-id=sha1
"$sdk/cmake/3.22.1/bin/cmake" --build "$work/xr-unixlib"
cp "$work/xr-unixlib/gamenative_xr_unixbridge.so" "$output/"
pulse=${XRGAME_PULSE_DIR:-"$repository/build/xrgame-runtime/pulseaudio"}
python3 - "$pulse" "$output" <<'PY'
import hashlib, json, pathlib, shutil, sys
src, out = map(pathlib.Path, sys.argv[1:])
name = 'pulseaudio-xrgame-20260926.tzst'
record = json.loads((src/'xrgame-pulse-build.json').read_text())
assert hashlib.sha256((src/name).read_bytes()).hexdigest() == record['sha256'][name]
shutil.copy2(src/name, out/name)
shutil.copy2(src/'xrgame-pulse-build.json', out/'xrgame-pulse-build.json')
PY
python3 - "$output" <<'PY'
import hashlib, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.iterdir()) if p.suffix in ('.dll', '.so')}
(out/'payload.version').write_text('3 '+hashes['gamenative_openxr_runtime64.dll']+' '+hashes['gamenative_openxr_runtime32.dll']+'\n')
(out/'xrgame-payload.json').write_text(json.dumps({'schema':1, 'openxrLoader':'1.1.61', 'sha256':hashes}, indent=2)+'\n')
PY
