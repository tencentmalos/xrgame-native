#!/usr/bin/env bash
# Optional x64 WineDbg for inspecting x64 guests; not a production component.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
pin=5d0d333e7beef02fbb455cc3163b4e8e25615458
reference="$base/src/proton-wine"
patch="$project/tools/xrgame/patches/winedbg-local-guest.patch"
inputs=$( { printf '%s\n' "$pin"; cat "$patch"; } | sha256sum | cut -c1-16)
work="$base/build/winedbg-x64/$inputs"
src="$work/source"
test "$(git -C "$reference" rev-parse HEAD)" = "$pin"
mkdir -p "$src" "$work/native" "$base/output/debugger"
if [[ ! -f "$work/source-ready" ]]; then
    git -C "$reference" archive "$pin" | tar -xf - -C "$src"
    git -C "$src" apply "$patch"
    (cd "$src" && bash autogen.sh)
    touch "$work/source-ready"
fi
export PATH="$base/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin:$PATH"
if [[ ! -f "$work/native/Makefile" ]]; then
    (cd "$work/native" && "$src/configure" --enable-win64 --enable-archs=x86_64 \
        --with-mingw=llvm-mingw --with-wine-tools="$base/build/proton-wine/host-tools" \
        --without-x --without-wayland --without-gstreamer --without-vulkan \
        --without-opengl --without-capstone --disable-tests)
fi
make -C "$work/native" -j"${XRGAME_JOBS:-12}" programs/winedbg/x86_64-windows/winedbg.exe
cp "$work/native/programs/winedbg/x86_64-windows/winedbg.exe" "$base/output/debugger/xrgame-winedbg-x64.exe"
sha256sum "$base/output/debugger/xrgame-winedbg-x64.exe"
python3 - "$base/output/debugger" "$pin" "$patch" <<'PY_META'
import hashlib, json, pathlib, sys
out, pin, patch = pathlib.Path(sys.argv[1]), sys.argv[2], pathlib.Path(sys.argv[3])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
(out / "xrgame-winedbg-build.json").write_text(json.dumps({
    "source": "https://github.com/tencentmalos/proton-wine", "revision": pin,
    "patchSha256": sha(patch), "binarySha256": sha(out / "xrgame-winedbg-x64.exe"),
    "architecture": "x86_64-windows", "purpose": "private-debugging",
}, indent=2) + "\n")
PY_META
