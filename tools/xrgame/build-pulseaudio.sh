#!/usr/bin/env bash
# Rebuild the Android PulseAudio daemon, dependencies and AAudio module.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
pin=178f0a379af39ae3da8fefdded8b0b1e5a3fda39
repo="$base/src/pulseaudio-android"
src="$base/build/pulseaudio-source"
mkdir -p "$base/src" "$base/build"
if [[ ! -d "$repo/.git" ]]; then
    git init "$repo"
    git -C "$repo" remote add origin https://github.com/tencentmalos/pulseaudio-android.git
    git -C "$repo" fetch --depth 1 origin "$pin"
    git -C "$repo" checkout --detach FETCH_HEAD
fi
test "$(git -C "$repo" rev-parse HEAD)" = "$pin"
git -C "$repo" submodule update --init --depth 1
test "$(git -C "$repo/pulseaudio" rev-parse HEAD)" = 4166ad3e30ec2a29a37d5fc1eaafcc7e03ebfe55
if [[ ! -f "$src/main-build.sh" ]]; then
    mkdir -p "$src/pulseaudio"
    git -C "$repo" archive HEAD | tar -xf - -C "$src"
    git -C "$repo/pulseaudio" archive HEAD | tar -xf - -C "$src/pulseaudio"
fi
python3 - "$base" "$src" "$repo/main-build.sh" <<'PY'
import pathlib, sys
base, src, recipe = sys.argv[1:]
text = pathlib.Path(recipe).read_text()
text = text.replace('#!/bin/bash', '#!/bin/bash\nset -euo pipefail', 1)
text = text.replace('export NDK_PATH="$HOME/Android/Sdk/ndk/27.3.13750724"',
                    'export NDK_PATH="'+base+'/toolchains/android-ndk-r27d"')
text = text.replace('$(nproc)', '${XRGAME_JOBS:-12}')
text = text.replace('rm -r ', 'rm -rf ')
text = text.replace('export LDFLAGS="', 'export LDFLAGS="-Wl,--build-id=sha1 ')
pathlib.Path(src, 'xrgame-build.sh').write_text(text)
PY
(cd "$src" && bash xrgame-build.sh -a arm64)
bin="$base/toolchains/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin"
"$bin/aarch64-linux-android26-clang" -O2 -fPIC -shared -Wl,--build-id=sha1 \
    -Wl,-z,max-page-size=16384 -I"$src/pulseaudio/build-arm64" -I"$src/pulseaudio/src" \
    -I"$src/root-arm64/include" -L"$src/root-arm64/lib/pulseaudio" -L"$src/root-arm64/lib" \
    "$src/pulseaudio-module/module-aaudio-sink.c" -lpulsecore-13.0 -lpulsecommon-13.0 \
    -lpulse -laaudio -llog -o "$src/output/aarch64-linux-gnu/modules/module-aaudio-sink.so"
mkdir -p "$base/output/pulseaudio"
cp -aL "$src/output/aarch64-linux-gnu/." "$base/output/pulseaudio/"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@1790380800 \
    -C "$base/output/pulseaudio" -cf - modules pactl | zstd -T4 -q -f \
    -o "$base/output/pulseaudio/pulseaudio-xrgame-20260926.tzst"
python3 - "$base/output/pulseaudio" <<'PY'
import hashlib, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
files = {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(out.rglob('*')) if p.is_file() and p.name != 'xrgame-pulse-build.json'}
(out/'xrgame-pulse-build.json').write_text(json.dumps({
    'source': 'https://github.com/tencentmalos/pulseaudio-android/tree/178f0a379af39ae3da8fefdded8b0b1e5a3fda39',
    'pulseaudioPin': '4166ad3e30ec2a29a37d5fc1eaafcc7e03ebfe55', 'sha256': files,
}, indent=2)+'\n')
PY
printf '%s\n' 'PulseAudio and AAudio sink built from pinned source.'
