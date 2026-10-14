#!/usr/bin/env bash
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
export PATH="$base/toolchains/python/bin:$base/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin:$PATH"
mkdir -p "$base/build" "$base/output"
cross="$base/build/arm64ec.ini"
cat > "$cross" <<EOF
[binaries]
c = 'arm64ec-w64-mingw32-clang'
cpp = 'arm64ec-w64-mingw32-clang++'
ar = 'arm64ec-w64-mingw32-ar'
strip = 'arm64ec-w64-mingw32-strip'
windres = 'arm64ec-w64-mingw32-windres'
widl = ['$base/build/proton-wine/host-tools/tools/widl/widl', '-b', 'arm64ec-w64-mingw32']
[properties]
needs_exe_wrapper = true
[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'arm64ec'
endian = 'little'
EOF
for component in dxvk vkd3d-proton; do
    if [[ "$component" == dxvk ]]; then
        XRGAME_DXVK_OUTPUT="$base/output/dxvk" bash "$project/tools/xrgame/build-dxvk-validation.sh"
        continue
    fi
    case "$component" in
        dxvk) pin=a6764047e587178283fcde4073ae6e1410af594f ;;
        vkd3d-proton) pin=212991fc2c266bc0d59f4c4ce8f80f7126508d71 ;;
    esac
    test "$(git -C "$base/src/$component" rev-parse HEAD)" = "$pin"
    git -C "$base/src/$component" submodule update --init --recursive --depth 1
    # XRGame texture quality and render scale, applied to a copy of the pinned tree. The shader side
    # of the render scale patches the pinned dxil-spirv subproject.
    patch="$project/tools/xrgame/patches/vkd3d-xrgame-resource-scale.patch"
    dxil_patch="$project/tools/xrgame/patches/dxil-spirv-xrgame-render-scale.patch"
    work="$base/build/$component-xrgame/$(cat "$patch" "$dxil_patch" | sha256sum | cut -c1-16)"
    if [[ ! -f "$work/source-ready" ]]; then
        rm -rf "$work/source"
        mkdir -p "$work"
        cp -a "$base/src/$component" "$work/source"
        git -C "$work/source" -c safe.directory='*' apply --check "$patch"
        git -C "$work/source" -c safe.directory='*' apply "$patch"
        git -C "$work/source/subprojects/dxil-spirv" -c safe.directory='*' apply --check "$dxil_patch"
        git -C "$work/source/subprojects/dxil-spirv" -c safe.directory='*' apply "$dxil_patch"
        touch "$work/source-ready"
    fi
    if [[ ! -f "$work/native/build.ninja" ]]; then
        meson setup "$work/native" "$work/source" --cross-file "$cross" \
            --buildtype release --prefix "$base/output/$component" --bindir arm64ec
    fi
    meson compile -C "$work/native" -j "${XRGAME_JOBS:-12}"
    meson install -C "$work/native"
done
