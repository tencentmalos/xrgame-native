#!/usr/bin/env bash
# MIT-licensed XCB rebuilt with XRGame's X11 socket path, without a path shim.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
deps="$base/termuxfs/aarch64/data/data/com.termux/files/usr"
ndk="$base/toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin"
src="$base/build/xcb-source"
proto="$base/build/xcb-proto-source"
host="$base/toolchains/xcb-proto"
target=/data/user/0/com.tencentmalos.xrgamenative/files/imagefs/usr
mkdir -p "$base/downloads" "$src" "$proto" "$base/build/xcb" "$base/build/xcb-proto"
fetch() {
    local url=$1 sha=$2 file="$base/downloads/$3"
    if [[ ! -f "$file" ]]; then
        curl -fL --retry 3 "$url" -o "$file.part"
        mv "$file.part" "$file"
    fi
    printf '%s  %s\n' "$sha" "$file" | sha256sum -c -
}
# SHA values also appear in termux-packages @ 3d8967a7a9b07d53f08aef1c1028c2d527cab7bf.
fetch https://xorg.freedesktop.org/archive/individual/lib/libxcb-1.17.0.tar.xz \
    599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55 libxcb-1.17.0.tar.xz
fetch https://xorg.freedesktop.org/archive/individual/proto/xcb-proto-1.17.0.tar.xz \
    2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c xcb-proto-1.17.0.tar.xz
if [[ ! -f "$proto/configure" ]]; then tar -xJf "$base/downloads/xcb-proto-1.17.0.tar.xz" --strip-components=1 -C "$proto"; fi
if [[ ! -f "$src/configure" ]]; then tar -xJf "$base/downloads/libxcb-1.17.0.tar.xz" --strip-components=1 -C "$src"; fi
python3 - "$src/src/xcb_util.c" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
text = p.read_text()
old = '"/tmp/.X11-unix/X"'
new = '"/data/user/0/com.tencentmalos.xrgamenative/files/imagefs/tmp/.X11-unix/X"'
assert old in text or new in text
p.write_text(text.replace(old, new))
PY
if [[ ! -f "$base/build/xcb-proto/Makefile" ]]; then
    (cd "$base/build/xcb-proto" && "$proto/configure" --prefix="$host" PYTHON=/usr/bin/python3)
fi
make -C "$base/build/xcb-proto" -j4 install
export PYTHONPATH="$proto${PYTHONPATH:+:$PYTHONPATH}"
export CC="$ndk/aarch64-linux-android28-clang"
export AR="$ndk/llvm-ar" RANLIB="$ndk/llvm-ranlib"
export CFLAGS="-O2 -fPIC -I$deps/include"
export LDFLAGS="-L$deps/lib -Wl,--build-id=sha1 -Wl,-z,max-page-size=16384"
export PKG_CONFIG_LIBDIR="$host/share/pkgconfig:$deps/lib/pkgconfig:$deps/share/pkgconfig"
export XAU_CFLAGS="-I$deps/include" XAU_LIBS="-L$deps/lib -lXau"
export XDMCP_CFLAGS="-I$deps/include" XDMCP_LIBS="-L$deps/lib -lXdmcp"
if [[ ! -f "$base/build/xcb/Makefile" ]]; then
    (cd "$base/build/xcb" && "$src/configure" --host=aarch64-linux-android \
        --prefix="$target" --enable-shared --disable-static --disable-devel-docs PYTHON=/usr/bin/python3)
fi
make -C "$base/build/xcb" -j"${XRGAME_JOBS:-12}"
make -C "$base/build/xcb" install DESTDIR="$base/build/xcb-stage"
mkdir -p "$base/output/xcb"
cp -a "$base/build/xcb-stage$target/lib/"*.so* "$base/output/xcb/"
cp "$src/COPYING" "$base/output/xcb/LICENSE"
printf '%s\n' 'XCB built with the XRGame socket path.'
