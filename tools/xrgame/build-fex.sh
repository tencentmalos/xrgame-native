#!/usr/bin/env bash
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
pin="$base/src/FEX"
src="$base/build/fex-source"
test "$(git -C "$pin" rev-parse HEAD)" = 3f1f30a060b633980ed8e7674eb8d8997457edad
if [[ ! -d "$src" ]]; then
    mkdir -p "$base/build"
    cp -a "$pin" "$src"
fi
for patch in fex-wine-4k-pages fex-arm64ec-work-callback-guard fex-single-step-return; do
    recipe="$project/tools/xrgame/patches/$patch.patch"
    if ! git -C "$src" apply --reverse --check "$recipe" 2>/dev/null; then
        git -C "$src" apply --check "$recipe"
        git -C "$src" apply "$recipe"
    fi
done
export PATH="$base/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin:$PATH"
for arch in arm64ec aarch64; do
    cmake -S "$src" -B "$base/build/fex-wine-$arch" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$src/Data/CMake/toolchain_mingw.cmake" \
        -DMINGW_TRIPLE="$arch-w64-mingw32" -DCMAKE_INSTALL_LIBDIR=lib/wine/aarch64-windows \
        -DENABLE_LTO=OFF -DENABLE_ASSERTIONS=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
        -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$base/output/fex" \
        -DTUNE_ARCH=generic -DTUNE_CPU=none -DRANGES_NATIVE=OFF
    cmake --build "$base/build/fex-wine-$arch" --parallel "${XRGAME_JOBS:-12}"
    cmake --install "$base/build/fex-wine-$arch"
done
cmake --fresh -S "$project/tools/xrgame/fex-unixlib" -B "$base/build/fex-unixlib" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$base/toolchains/android-ndk-r27d/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DFEX_SOURCE="$src" -DCMAKE_INSTALL_PREFIX="$base/output/fex"
cmake --build "$base/build/fex-unixlib" --parallel 4
cmake --install "$base/build/fex-unixlib"
python3 "$project/tools/xrgame/record-fex-build.py" --root "$base" --project "$project"
