#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# glibc Turnip (KGSL) and Zink for the official Steam client's userspace only; games keep the
# Android Turnip from tools/xrgame/build-turnip.sh. Runs on Ubuntu 24.04, natively on arm64 or on
# x86_64 with arm64 multiarch packages (libdrm, libexpat1, zlib1g, libx11-xcb, libxext, libxfixes,
# libxrandr, libxxf86vm, libxshmfence, libxcb-glx0, libxcb-dri2-0, libxcb-dri3, libxcb-present,
# libglvnd -dev) and aarch64-linux-gnu-gcc.
# The web helper's compositor draws with Zink through the glvnd vendor "steampsp" (Ubuntu's Mesa
# stays the default vendor); Zink presents through the Android X server
# (XRGAME_X11_HOST_DISPLAY, turnip-x11-host-ahb.patch) and reads Xephyr's pixmaps back
# (mesa-kopper-pixmap-readback.patch). A swapchain Zink killed no longer crashes
# glXSwapIntervalEXT or buffer-age queries (mesa-zink-killed-swapchain.patch).
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
reference="$base/src/mesa-turnip"
# Same source as the Android Turnip recipe.
pin=adb7e30a5646f1ae51e49c20ea4a0be4fa683355
git -C "$reference" cat-file -e "$pin^{commit}"
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
# The Android recipe's patches, in its order (their Android-only parts compile out here), then
# host presentation.
patches=("$project/tools/xrgame/patches/turnip-x11-ahb.patch"
         "$project/tools/xrgame/patches/turnip-ahb-entrypoints.patch"
         "$project/tools/xrgame/patches/turnip-lrz-rmw-no-cb.patch"
         "$project/tools/xrgame/patches/turnip-fdm-visualize.patch"
         "$project/tools/xrgame/patches/turnip-max-samples.patch"
         "$project/tools/xrgame/patches/turnip-x11-host-ahb.patch"
         "$project/tools/xrgame/patches/mesa-kopper-pixmap-readback.patch"
         "$project/tools/xrgame/patches/mesa-zink-killed-swapchain.patch")
vendor=steampsp
patch_sha=$( (echo "$pin"; cat "${patches[@]}") | sha256sum | cut -d ' ' -f1)
work="$base/build/linux-steam-gpu/${patch_sha:0:16}"
src="$work/source"
mkdir -p "$src"
if [[ ! -f "$work/source-ready" ]]; then
    git -C "$reference" archive "$pin" | tar -xf - -C "$src"
    for patch in "${patches[@]}"; do git -C "$src" apply "$patch"; done
    touch "$work/source-ready"
fi
meson=${MESON:-meson}
cross=()
# The Vulkan loader and the XCB extension libraries the driver links. The client userspace
# need not carry them; the component brings Ubuntu's own copies.
runtime_packages=(libvulkan1 libx11-xcb1 libxcb-dri3-0 libxcb-present0 libxcb-xfixes0
                  libxcb-randr0 libxcb-shm0 libxcb-keysyms1 libxcb-glx0 libxcb-sync1 libxshmfence1
                  libxxf86vm1 libxext6)
arch_suffix=
if [[ $(uname -m) != aarch64 ]]; then
    cat > "$work/aarch64-linux-gnu.ini" <<'EOF'
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = ['/usr/lib/aarch64-linux-gnu/pkgconfig', '/usr/share/pkgconfig']
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF
    cross=(--cross-file "$work/aarch64-linux-gnu.ini")
    arch_suffix=:arm64
fi
if [[ ! -f "$work/native/build.ninja" ]]; then
    "$meson" setup "$work/native" "$src" "${cross[@]}" --buildtype release --wrap-mode=nofallback \
        -Dc_link_args=-Wl,--build-id=sha1 -Dcpp_link_args=-Wl,--build-id=sha1 \
        -Dplatforms=x11 -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dgallium-drivers=zink \
        -Dopengl=true -Dglx=dri -Dglvnd=enabled -Dglvnd-vendor-name=$vendor \
        -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dgbm=disabled \
        -Dllvm=disabled -Dbuild-tests=false -Dzstd=disabled -Dvideo-codecs= -Dvulkan-layers= -Dtools=
fi
"$meson" compile -C "$work/native" -j "${XRGAME_JOBS:-8}"
library="$work/native/src/freedreno/vulkan/libvulkan_freedreno.so"
# Runtime ABI requirements, not merely a successful link.
nm -D --defined-only "$library" | awk '$NF == "vk_icdGetInstanceProcAddr" { icd = 1 } END { exit !icd }'
grep -q XRGAME_X11_HOST_DISPLAY "$library"
glx="$work/native/src/glx/libGLX_$vendor.so.0"
gallium=$(ls "$work"/native/src/gallium/targets/dri/libgallium-*.so)
nm -D --defined-only "$glx" | awk '$NF == "__glx_Main" { found = 1 } END { exit !found }'
grep -q zink "$gallium"

out="$base/output/linux-steam-gpu"
rm -rf "$out"
mkdir -p "$out/lib" "$out/share/vulkan/icd.d" "$out/share/doc/mesa"
cp "$library" "$out/lib/libvulkan_freedreno.so"
cp -L "$glx" "$out/lib/libGLX_$vendor.so.0"
cp "$gallium" "$out/lib/$(basename "$gallium")"
cp "$src/docs/license.rst" "$out/share/doc/mesa/license.rst"
# Runtime libraries come from the signed Ubuntu archive; record exactly which packages.
debs="$work/debs"
rm -rf "$debs"
mkdir -p "$debs"
(cd "$debs" && apt-get download "${runtime_packages[@]/%/$arch_suffix}")
for package in "${runtime_packages[@]}"; do
    deb=$(ls "$debs/${package}"_*.deb)
    dpkg-deb -x "$deb" "$debs/root-$package"
    for library_file in "$debs/root-$package"/usr/lib/aarch64-linux-gnu/*.so.*; do
        # Keep the SONAME the driver and Chromium load (libfoo.so.N), not the full version.
        soname=$(readelf -d "$library_file" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
        if [[ -n "$soname" && ! -e "$out/lib/$soname" ]]; then cp -L "$library_file" "$out/lib/$soname"; fi
    done
    mkdir -p "$out/share/doc/$package"
    cp "$debs/root-$package/usr/share/doc/$package/copyright" "$out/share/doc/$package/copyright"
done
python3 - "$(ls "$work"/native/src/freedreno/vulkan/freedreno_devenv_icd.*.json)" "$out" "$pin" "$debs" "$library" "$vendor" "${patches[@]}" <<'PY'
import hashlib, json, pathlib, subprocess, sys
generated, out, pin, debs, library, vendor, *patches = sys.argv[1:]
out = pathlib.Path(out)
def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()
api = json.loads(pathlib.Path(generated).read_text())['ICD']['api_version']
# PRoot installs the component at this fixed guest path.
icd = {'file_format_version': '1.0.1',
       'ICD': {'library_path': '/opt/steampsp-gpu/lib/libvulkan_freedreno.so', 'api_version': api}}
(out / 'share/vulkan/icd.d/freedreno_icd.aarch64.json').write_text(json.dumps(icd, indent=2) + '\n')
def build_id(path):
    notes = subprocess.check_output(['readelf', '-n', str(path)], text=True)
    return next(line.split()[-1] for line in notes.splitlines() if 'Build ID' in line)
packages = []
for deb in sorted(pathlib.Path(debs).glob('*.deb')):
    fields = subprocess.check_output(['dpkg-deb', '-f', str(deb), 'Package', 'Version', 'Architecture'], text=True)
    packages.append({'deb': deb.name, 'sha256': sha(deb),
                     'control': dict(line.split(': ', 1) for line in fields.strip().splitlines())})
record = {
    'scope': 'official Steam client userspace only; games use the Android Turnip',
    'mesa': {'repository': 'https://github.com/tencentmalos/mesa-mirror', 'pin': pin,
             'patches': {pathlib.Path(p).name: sha(p) for p in patches},
             'buildId': build_id(library), 'glvndVendor': vendor,
             'buildIds': {p.name: build_id(p) for p in sorted((out / 'lib').iterdir())
                          if p.name.startswith(('libvulkan_freedreno', 'libGLX_', 'libgallium'))}},
    'ubuntuPackages': packages,
    'files': {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*')) if p.is_file()},
}
(out / 'gpu-build.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
PY
