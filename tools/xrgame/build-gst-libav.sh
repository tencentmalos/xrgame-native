#!/usr/bin/env bash
# Match the pinned GStreamer 1.28 / FFmpeg 8 Termux dependency snapshot.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
pin=9058212f43074ef7df229e73cea135c4ea96e0d6
repo="$base/src/gstreamer"
deps="$base/termuxfs/aarch64/data/data/com.termux/files/usr"
ndk="$base/toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin"
src="$base/build/gst-libav-source"
build="$base/build/gst-libav-media"
out="$base/output/gst-libav"
if [[ ! -d "$repo/.git" ]]; then
    git init "$repo"
    git -C "$repo" remote add origin https://github.com/tencentmalos/gstreamer.git
    git -C "$repo" fetch --depth 1 origin "$pin"
    git -C "$repo" checkout --detach FETCH_HEAD
fi
test "$(git -C "$repo" rev-parse HEAD)" = "$pin"
mkdir -p "$src"
git -C "$repo" archive "$pin:subprojects/gst-libav" | tar -xf - -C "$src"
# Optional FFmpeg deinterlace/comparison filters pull libavfilter/libplacebo and
# a newer C++ ABI than the NDK runtime. Keep decoding independent of those filters.
python3 - "$src" <<'PY'
import pathlib, sys
src = pathlib.Path(sys.argv[1])
edits = {
    'meson.build': [("libavfilter_dep = dependency('libavfilter', version: '>= 7.16.100')\n", ''),
                    ('libavcodec_dep, libavfilter_dep, libavformat_dep', 'libavcodec_dep, libavformat_dep')],
    'ext/libav/meson.build': [("    'gstavdeinterlace.c',\n", ''), ("    'gstavvidcmp.c',\n", '')],
    'ext/libav/gstav.c': [('  gst_ffmpegdeinterlace_register (plugin);\n', ''),
                         ('  gst_ffmpegvidcmp_register (plugin);\n', '')],
}
for name, changes in edits.items():
    path = src / name
    text = path.read_text()
    for before, after in changes:
        assert text.count(before) == 1, (name, before)
        text = text.replace(before, after)
    path.write_text(text)
PY
mkdir -p "$build" "$out"
cat > "$build/cross.ini" <<EOF
[binaries]
c = '$ndk/aarch64-linux-android28-clang'
ar = '$ndk/llvm-ar'
strip = '$ndk/llvm-strip'
pkg-config = '/usr/bin/pkg-config'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[properties]
sys_root = '$base/termuxfs/aarch64'
pkg_config_libdir = ['$deps/lib/pkgconfig', '$deps/share/pkgconfig']
needs_exe_wrapper = true
[built-in options]
c_link_args = ['-Wl,--build-id=sha1', '-Wl,-z,max-page-size=16384']
EOF
meson="$base/toolchains/python/bin/meson"
if [[ ! -f "$build/build.ninja" ]]; then
    "$meson" setup "$build" "$src" --cross-file "$build/cross.ini" \
        --buildtype=release --wrap-mode=nofallback -Dtests=disabled -Ddoc=disabled \
        --prefix=/data/user/0/com.tencentmalos.xrgamenative/files/imagefs/usr
fi
"$meson" compile -C "$build" -j "${XRGAME_JOBS:-12}"
cp "$build/ext/libav/libgstlibav.so" "$out/"
cp "$repo/subprojects/gst-libav/COPYING" "$out/LICENSE"
python3 - "$out" "$pin" "$(realpath "$0")" "$build/cross.ini" <<'PY'
import hashlib, json, pathlib, sys
out, pin, recipe, cross = sys.argv[1:]
out = pathlib.Path(out)
sha = lambda p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
(out/'build.json').write_text(json.dumps({
    'source': 'https://github.com/tencentmalos/gstreamer', 'pin': pin,
    'version': '1.28.0', 'recipeSha256': sha(recipe), 'crossFileSha256': sha(cross),
    'files': {'libgstlibav.so': {'sha256': sha(out/'libgstlibav.so')}},
    'omittedElements': ['avdeinterlace', 'avvideocompare'], 'releaseReady': False,
}, indent=2)+'\n')
PY
