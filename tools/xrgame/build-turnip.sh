#!/usr/bin/env bash
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
reference="$base/src/mesa-turnip"
# tencentmalos/mesa-mirror malos/main: shadPS4 codex/shadps4-xr-turnip + Azahar
# bugfix/turnip_in_swan merged onto codex/turnip-xr-fdm2, plus 04e1d665 (xrg12): LRZ fast clear
# is off on A8XX unless TU_DEBUG=lrzfc (Swan Alyx GPU hang in LRZ fast-clear passes; LRZ stays on),
# plus adb7e30a (xrg13): the bin foveation registers are reset at every command buffer start, so
# values left by other KGSL contexts no longer redirect render target writes on the A840.
pin=adb7e30a5646f1ae51e49c20ea4a0be4fa683355
git -C "$reference" cat-file -e "$pin^{commit}"
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
# Applied in order; together they equal feature/malos/xrgame-wine-icd. The third (xrg10)
# disables concurrent binning in render passes that emit LRZ CP_REG_RMWs; it has no effect while
# drirc keeps concurrent binning globally off. xrg11 adds the app-process Android build below.
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

# The same source as an Android-only build for the app process (XR composite through
# adrenotools): Android WSI only, loaded by the platform Vulkan loader as its HAL.
if [[ ! -f "$work/app/build.ninja" ]]; then
    meson setup "$work/app" "$src" --cross-file "$cross" --buildtype release --wrap-mode=nofallback \
        -Dplatforms=android -Dandroid-stub=true -Dandroid-strict=false -Dandroid-libbacktrace=disabled \
        -Dplatform-sdk-version=33 -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
        -Dgallium-drivers= -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
        -Dopengl=false -Dllvm=disabled -Dbuild-tests=false -Dzstd=disabled
fi
meson compile -C "$work/app" -j "${XRGAME_JOBS:-8}"
app_library="$work/app/src/freedreno/vulkan/libvulkan_freedreno.so"
"$tools/llvm-nm" -D --defined-only "$app_library" | awk '
    $NF == "vk_icdGetInstanceProcAddr" { icd = 1 }
    $NF == "HMI" { hal = 1 }
    END { exit !(icd && hal) }'
# Termux's zlib is libz.so.1; the app process links the platform's libz.so. Same-length rewrite
# of the one NEEDED string.
python3 - "$app_library" "$base/output/turnip/libvulkan_freedreno_android.so" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
old, new = b'libz.so.1\0', b'libz.so\0\0\0'
assert data.count(old) == 1, 'expected one libz.so.1 NEEDED string'
open(sys.argv[2], 'wb').write(data.replace(old, new))
PY
