#!/usr/bin/env bash
# Run in the Ubuntu 24.04 build container prepared by prepare-linux.sh.
# The pinned checkout stays untouched; Android patches are applied to a build copy.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
phase=${1:-all}
pin=5d0d333e7beef02fbb455cc3163b4e8e25615458
work="$base/build/proton-wine"
src="$work/source"
host="$work/host-tools"
target="$work/android"
ndk="$base/toolchains/android-ndk-r27d"
llvm="$base/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64"
prefix=/data/user/0/com.tencentmalos.xrgamenative/files/imagefs/opt/proton-11.0-2-arm64ec
test "$(git -C "$base/src/proton-wine" rev-parse HEAD)" = "$pin"
mkdir -p "$src" "$host" "$target" "$base/output"

if [[ ! -f "$work/source-ready" ]]; then
    git -C "$base/src/proton-wine" archive "$pin" | tar -xf - -C "$src"
    python3 - "$src" <<'PY'
import pathlib, re, subprocess, sys
src = pathlib.Path(sys.argv[1])
recipe = (src/'build-scripts/build-step-arm64ec.sh').read_text()
block = recipe.split('    PATCHES=(', 1)[1].split('\n    )', 1)[0]
patches = re.findall(r'^\s*"([^"]+)"\s*$', block, re.M)
assert patches, 'No Android patches found'
for patch in patches:
    subprocess.run(['git', 'apply', '--check', str(src/'android/patches'/patch)], cwd=src, check=True)
    subprocess.run(['git', 'apply', str(src/'android/patches'/patch)], cwd=src, check=True)
(src/'xrgame-applied-patches.txt').write_text('\n'.join(patches)+'\n')
configure = src/'configure.ac'
text = configure.read_text()
anchor = 'WINE_CONFIG_MAKEFILE(dlls/gameinput)'
assert anchor in text
configure.write_text(text.replace(anchor, anchor+'\nWINE_CONFIG_MAKEFILE(dlls/gamenative_xr_unixbridge)'))
PY
    mkdir -p "$src/dlls/gamenative_xr_unixbridge"
    for file in Makefile.in gamenative_xr_unixbridge.c gamenative_xr_unixbridge.spec; do
        cp "$project/app/src/main/windows/openxr_runtime/builtin/$file" "$src/dlls/gamenative_xr_unixbridge/"
    done
    (cd "$src" && bash autogen.sh)
    touch "$work/source-ready"
fi

if [[ ! -f "$src/include/wine/vulkan.h" || ! -f "$src/include/config.h.in" ]]; then
    (cd "$src" && bash autogen.sh)
fi
# Apply to fresh and existing build copies. Reject an unrelated loader instead
# of silently accepting an obsolete or partially applied bootstrap patch.
bootstrap_patch="$project/tools/xrgame/patches/wine-image-bootstrap.patch"
if git -C "$src" apply --check "$bootstrap_patch" 2>/dev/null; then
    git -C "$src" apply "$bootstrap_patch"
else
    git -C "$src" apply --reverse --check "$bootstrap_patch"
fi
# Host-only AHardwareBuffer interop for the OpenXR bridge (WINE_VK_HOST_AHB=1).
host_ahb_patch="$project/tools/xrgame/patches/wine-vulkan-host-ahb.patch"
if git -C "$src" apply --check "$host_ahb_patch" 2>/dev/null; then
    git -C "$src" apply "$host_ahb_patch"
else
    git -C "$src" apply --reverse --check "$host_ahb_patch"
fi
# Map the unused rest of a small allocation's 64k granule so neighbouring views share one
# kernel VMA; Source 2 games otherwise exceed vm.max_map_count (WINE_MERGE_SMALL_VIEWS=0 disables).
merge_views_patch="$project/tools/xrgame/patches/wine-merge-small-views.patch"
if git -C "$src" apply --check "$merge_views_patch" 2>/dev/null; then
    git -C "$src" apply "$merge_views_patch"
else
    git -C "$src" apply --reverse --check "$merge_views_patch"
fi
if [[ "$phase" == all || "$phase" == host ]]; then
    if [[ ! -f "$host/Makefile" ]]; then
        (cd "$host" && "$src/configure" --enable-win64 --without-x --without-gstreamer --without-vulkan --without-wayland)
    fi
    make -C "$host" -j"${XRGAME_JOBS:-16}" __tooldeps__ nls/all
fi
if [[ "$phase" == host ]]; then exit 0; fi

# Proton 11's makedep adds ARM64X flags to the aarch64/arm64ec hybrid itself.
# The older standalone bridge recipe applied -marm64x to i386 as well.
sed 's/-marm64x //g' "$project/app/src/main/windows/openxr_runtime/builtin/Makefile.in" \
    > "$src/dlls/gamenative_xr_unixbridge/Makefile.in"
cp "$project/app/src/main/windows/openxr_runtime/builtin/gamenative_xr_unixbridge.c" \
    "$src/dlls/gamenative_xr_unixbridge/"

# Preserve the pinned upstream feature switches and cross-compiler flags, changing
# only the build/staging locations and the runtime's own Android package path.
python3 - "$base" "$src" "$host" "$prefix" <<'PY'
import pathlib, re, shlex, sys
base, src, host, prefix = sys.argv[1:]
recipe = (pathlib.Path(src)/'build-scripts/build-step-arm64ec.sh').read_text()
env = recipe.split('\nfor arg in "$@"', 1)[0]
values = {
 'OUTPUT_DIR': base+'/output/proton',
 'deps': base+'/termuxfs/aarch64/data/data/com.termux/files/usr',
 'RUNTIME_PATH': '/data/user/0/com.tencentmalos.xrgamenative/files/imagefs/usr',
 'install_dir': prefix,
 'TOOLCHAIN': base+'/toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin',
 'LLVM_MINGW_TOOLCHAIN': base+'/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin',
}
for name, value in values.items():
    env, count = re.subn(r'^export '+name+r'=.*$', 'export '+name+'='+shlex.quote(value), env, flags=re.M)
    assert count == 1, (name, count)
configure = recipe.split('    ./configure \\\n', 1)[1].split('\n\n    echo "Applying patches..."', 1)[0]
configure = configure.replace('--with-wine-tools=./wine-tools', '--with-wine-tools='+shlex.quote(host))
out = pathlib.Path(base)/'build/proton-wine'
(out/'environment.sh').write_text(env+'\n')
(out/'configure-android.sh').write_text('set -euo pipefail\n'+shlex.quote(src+'/configure')+' \\\n'+configure+'\n')
PY
# shellcheck source=/dev/null
source "$work/environment.sh"
export RUSTUP_HOME="$base/toolchains/rustup"
export CARGO_HOME="$base/toolchains/cargo"
export PATH="$CARGO_HOME/bin:$PATH"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$TOOLCHAIN/aarch64-linux-android28-clang"
export CARGO_TARGET_DIR="$base/build/ntsync"
(cd "$base/src/ntsync-android" && cargo build --locked --release --target aarch64-linux-android)
cp "$CARGO_TARGET_DIR/aarch64-linux-android/release/libntsync_android.a" "$deps/lib/"
"$TOOLCHAIN/aarch64-linux-android28-clang" -Wall -std=gnu99 -shared -fPIC \
    -Wl,--build-id=sha1 -Wl,-z,max-page-size=16384 -I"$src/android/android_sysvshm" \
    "$src/android/android_sysvshm/android_sysvshm.c" -o "$deps/lib/libandroid-sysvshm.so"
if [[ ! -f "$target/Makefile" ]]; then
    (cd "$target" && bash "$work/configure-android.sh")
fi
make -C "$target" -j"${XRGAME_JOBS:-16}"
make -C "$target" -j"${XRGAME_JOBS:-16}" install DESTDIR="$work/stage"
test -f "$work/stage$prefix/lib/wine/aarch64-windows/gamenative_xr_unixbridge.dll"
printf '%s\n' "$work/stage$prefix" > "$base/output/proton-stage.txt"
printf '%s\n' 'Wine built; stage is ready for component packaging.'
