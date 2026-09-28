#!/usr/bin/env bash
# Source-owned x64 console/GDI/D3D11 fixture for runtime and debugger validation.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
out="$base/output/windows-probe"
compiler=${XRGAME_MINGW_CC:-"$base/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin/x86_64-w64-mingw32-clang"}
mkdir -p "$out"
"$compiler" -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Werror -municode \
    "$project/tools/xrgame/fixtures/windows-probe.c" \
    -o "$out/xrgame-windows-probe.exe" -ld3d11 -ldxgi -ldxguid -ld3dcompiler -luser32 -lgdi32
sha256sum "$out/xrgame-windows-probe.exe" "$project/tools/xrgame/fixtures/windows-probe.c" > "$out/SHA256SUMS"
