#!/usr/bin/env bash
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
reference="$base/src/mesa-turnip"
# tencentmalos/mesa-mirror malos/main: shadPS4 codex/shadps4-xr-turnip + Azahar
# bugfix/turnip_in_swan merged onto codex/turnip-xr-fdm2.
pin=25ef1647a28d6983bd95f8ef0cc84ea74dd9cc32
git -C "$reference" cat-file -e "$pin^{commit}"
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
# Applied in order; the first two equal feature/malos/xrgame-wine-icd. The third (xrg10)
# disables concurrent binning in render passes that emit LRZ CP_REG_RMWs (Swan GPU hang).
patches=("$project/tools/xrgame/patches/turnip-x11-ahb.patch"
         "$project/tools/xrgame/patches/turnip-ahb-entrypoints.patch"
         "$project/tools/xrgame/patches/turnip-lrz-rmw-no-cb.patch")
patch_sha=$( (echo "$pin"; cat "${patches[@]}") | sha256sum | cut -d ' ' -f1)
work="$base/build/turnip-x11-ahb/${patch_sha:0:16}"
src="$work/source"
mkdir -p "$src"
if [[ ! -f "$work/source-ready" ]]; then
    git -C "$reference" archive "$pin" | tar -xf - -C "$src"
    for patch in "${patches[@]}"; do git -C "$src" apply "$patch"; done
    touch "$work/source-ready"
fi
export PATH="$base/toolchains/python/bin:$PATH"
tools="$base/toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin"
deps="$base/termuxfs/aarch64/data/data/com.termux/files/usr"
cross="$work/android-aarch64.ini"
cat > "$cross" <<EOF
[binaries]
c = '$tools/aarch64-linux-android33-clang'
cpp = '$tools/aarch64-linux-android33-clang++'
ar = '$tools/llvm-ar'
strip = '$tools/llvm-strip'
pkg-config = ['/usr/bin/pkg-config', '--define-variable=prefix=$deps']
[properties]
pkg_config_libdir = ['$deps/lib/pkgconfig', '$deps/share/pkgconfig']
[built-in options]
c_args = ['-idirafter', '$deps/include']
cpp_args = ['-idirafter', '$deps/include']
c_link_args = ['-L$deps/lib', '-Wl,--build-id=sha1']
cpp_link_args = ['-L$deps/lib', '-Wl,--build-id=sha1']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF
if [[ ! -f "$work/native/build.ninja" ]]; then
    # Wine's X11 driver needs Xlib/XCB WSI in addition to Android AHB support.
    meson setup "$work/native" "$src" --cross-file "$cross" --buildtype release --wrap-mode=nofallback \
        -Dplatforms=android,x11 -Dandroid-stub=true -Dandroid-strict=false -Dandroid-libbacktrace=disabled \
        -Dplatform-sdk-version=33 -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
        -Dgallium-drivers= -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
        -Dopengl=false -Dllvm=disabled -Dbuild-tests=false -Dzstd=disabled
else
    # Wine uses the driver's desktop API, not Android's SDK extension allowlist.
    meson configure "$work/native" -Dandroid-strict=false
fi
meson compile -C "$work/native" -j "${XRGAME_JOBS:-8}"
library="$work/native/src/freedreno/vulkan/libvulkan_freedreno.so"
# This is a runtime ABI requirement, not merely a successful link.
"$tools/llvm-nm" -D --defined-only "$library" | awk '
    $NF == "vk_icdGetInstanceProcAddr" { icd = 1 }
    $NF == "HMI" { hal = 1 }
    END { exit !(icd && hal) }'
mkdir -p "$base/output/turnip"
cp "$library" "$base/output/turnip/"
