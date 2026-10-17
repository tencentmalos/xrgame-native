#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Internal diagnostic only. No Valve client or account data in this APK.
set -euo pipefail
source /project/tools/xrgame/apple-container/environment.sh
here=/project/tools/xrgame/linux-steam/probe
out=$XRGAME_DEV_ROOT/linux-steam/probe
mkdir -p "$out"/{classes,dex,assets,lib/arm64-v8a,root/lib/aarch64-linux-gnu}
aarch64-linux-gnu-gcc -O2 -g "$here/glibc-probe.c" -o "$out/root/probe" -pthread -ldl -Wl,--build-id=sha1
cp -L /usr/aarch64-linux-gnu/lib/{ld-linux-aarch64.so.1,libc.so.6} "$out/root/lib/"
mv "$out/root/lib/libc.so.6" "$out/root/lib/aarch64-linux-gnu/"
cp "$XRGAME_DEV_ROOT/linux-steam/proot/"*.so "$out/lib/arm64-v8a/"
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang" \
    -O2 -g "$here/bionic-client-probe.c" -ldl -Wl,--build-id=sha1 -o "$out/lib/arm64-v8a/libclientprobe.so"
cp "$XRGAME_DEV_ROOT/linux-steam/proot/proot-build.json" "$out/"
python3 - "$out" <<'PY'
import pathlib, sys, zipfile
out = pathlib.Path(sys.argv[1])
with zipfile.ZipFile(out / 'assets/root.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for p in sorted((out / 'root').rglob('*')):
        if p.is_file(): z.write(p, p.relative_to(out / 'root'))
PY
bt=$ANDROID_HOME/build-tools/36.0.0
jar=$ANDROID_HOME/platforms/android-36/android.jar
"$JAVA_HOME/bin/javac" -encoding UTF-8 -source 8 -target 8 -classpath "$jar" -d "$out/classes" "$here/ProbeActivity.java"
"$bt/d8" --lib "$jar" --min-api 29 --output "$out/dex" "$out/classes/com/tencentmalos/linuxsteamprobe/ProbeActivity.class"
"$bt/aapt2" link -I "$jar" --manifest "$here/AndroidManifest.xml" -A "$out/assets" -o "$out/unsigned.apk"
python3 - "$out" <<'PY'
import pathlib, sys, zipfile
out = pathlib.Path(sys.argv[1])
with zipfile.ZipFile(out / 'unsigned.apk', 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(out / 'dex/classes.dex', 'classes.dex')
    for p in sorted((out / 'lib').rglob('*.so')): z.write(p, p.relative_to(out))
PY
"$bt/zipalign" -f -p 4 "$out/unsigned.apk" "$out/aligned.apk"
"$bt/apksigner" sign --ks /work/signing/debug.keystore --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android --out "$out/probe.apk" "$out/aligned.apk"
"$bt/apksigner" verify "$out/probe.apk"
mkdir -p /work/artifacts/linux-steam-probe
cp "$out/probe.apk" "$out/proot-build.json" /work/artifacts/linux-steam-probe/
sha256sum "$out/probe.apk" "$out/proot-build.json"
