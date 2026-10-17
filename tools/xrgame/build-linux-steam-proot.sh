#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# PRoot recipe adapted from the pinned DroidDeck tools/proot/build.sh (GPL-3.0).
# Build in the local Apple Container; never modify either source submodule.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?usage: build-linux-steam-proot.sh OUTPUT_DIRECTORY}
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || exit 2
source "$repo/references/DroidDeck/tools/proot/source.env"
[[ $(git -C "$repo/references/proot" rev-parse HEAD) == "$PROOT_COMMIT" ]] || {
    echo 'PRoot revision differs from reviewed DroidDeck recipe' >&2; exit 2;
}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tc=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
cc=$tc/aarch64-linux-android29-clang
mkdir "$work/proot" "$work/talloc" "$work/bin"
git -C "$repo/references/proot" archive HEAD | tar -x -C "$work/proot"
if [[ -n ${XRGAME_TALLOC_ARCHIVE:-} ]]; then
    cp "$XRGAME_TALLOC_ARCHIVE" "$work/talloc.tar.gz"
else
    curl -4 --fail --location --proto '=https' --connect-timeout 15 --max-time 180 --retry 2 \
        "https://www.samba.org/ftp/talloc/talloc-$TALLOC_VERSION.tar.gz" -o "$work/talloc.tar.gz"
fi
echo "$TALLOC_SHA256  $work/talloc.tar.gz" | sha256sum -c -
tar -xzf "$work/talloc.tar.gz" -C "$work/talloc" --strip-components=1
for change in "$repo"/references/DroidDeck/tools/proot/patches/*.patch; do
    patch -d "$work/proot" -p1 --forward --quiet < "$change"
done
mkdir "$work/talloc/config"
cp "$repo/references/DroidDeck/tools/proot/talloc-config.h" "$work/talloc/config/config.h"
"$cc" -c -O2 -g -fPIC -ffile-prefix-map="$work"=. -D__STDC_WANT_LIB_EXT1__=1 -DHAVE_CONFIG_H \
    -I"$work/talloc/config" -I"$work/talloc" -I"$work/talloc/lib/replace" \
    -o "$work/talloc.o" "$work/talloc/talloc.c"
"$tc/llvm-ar" rcs "$work/libtalloc.a" "$work/talloc.o"
ln -s "$tc/llvm-readelf" "$work/bin/readelf"
PATH="$work/bin:$PATH" CFLAGS="-g -ffile-prefix-map=$work=." make -C "$work/proot/src" -j"${XRGAME_JOBS:-4}" \
    CC="$cc" STRIP="$tc/llvm-strip" OBJCOPY="$tc/llvm-objcopy" OBJDUMP="$tc/llvm-objdump" \
    CPPFLAGS="-D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -I. -I$work/talloc -DARG_MAX=131072 -Wno-error=implicit-function-declaration" \
    LDFLAGS="$work/libtalloc.a -Wl,-z,noexecstack -Wl,--build-id=sha1" \
    PROOT_UNBUNDLE_LOADER=/nonexistent proot loader
install -m755 "$work/proot/src/proot" "$out/libproot.so"
install -m755 "$work/proot/src/loader/loader" "$out/libproot-loader.so"
grep -q '^#define HAVE_SECCOMP_FILTER' "$work/proot/src/build.h"
grep -q '^#define HAVE_PROCESS_VM' "$work/proot/src/build.h"
"$tc/llvm-readelf" -d "$out/libproot.so" > "$out/proot-dynamic.txt"
while read -r lib; do
    case "$lib" in libc.so|libdl.so|libm.so) ;; *) echo "Unexpected dependency: $lib" >&2; exit 2;; esac
done < <(sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' "$out/proot-dynamic.txt")
python3 - "$repo" "$out" "$TALLOC_VERSION" "$TALLOC_SHA256" <<'PY'
import hashlib, json, pathlib, subprocess, sys
repo, out = map(pathlib.Path, sys.argv[1:3])
def revision(name):
    return subprocess.check_output(['git', '-C', str(repo / 'references' / name), 'rev-parse', 'HEAD'], text=True).strip()
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
manifest = dict(schema=1, proot=revision('proot'), droiddeck=revision('DroidDeck'),
                talloc=dict(version=sys.argv[3], sha256=sys.argv[4]),
                recipe_sha256=sha(repo / 'tools/xrgame/build-linux-steam-proot.sh'),
                patches={p.name: sha(p) for p in sorted((repo / 'references/DroidDeck/tools/proot/patches').glob('*.patch'))},
                files={p.name: sha(p) for p in sorted(out.glob('*.so'))})
(out / 'proot-build.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
