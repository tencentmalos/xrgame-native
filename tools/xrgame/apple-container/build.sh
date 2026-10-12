#!/usr/bin/env bash
set -euo pipefail
source /project/tools/xrgame/apple-container/environment.sh
exec 9>"$XRGAME_DEV_ROOT/.operation.lock"
flock -n 9 || { echo 'A Linux build/setup is still active.' >&2; exit 1; }
cd "$XRGAME_DEV_ROOT/project"
run=${1:?artifact directory name required}
[[ "$run" =~ ^[A-Za-z0-9._-]+$ ]] || exit 2
out=/work/artifacts/$run
mkdir -p "$out"
# Keep failed and successful build logs in the host artifact directory.
exec > >(tee "$out/build.log") 2>&1
cp build/apple-container-provenance/* "$out/"
java -version
rustc --version
cargo ndk --version
# The dependency is rebuilt from source; Gradle handles incremental reuse.
bash tools/build-javasteam.sh
# Always package a fresh APK (avoid incremental ZIP holes).
find app/build/outputs/apk/picoXr/debug -maxdepth 1 -name '*.apk' -delete 2>/dev/null || true
./gradlew --no-daemon --console=plain --max-workers=4 :app:assemblePicoXrDebug
mapfile -t apks < <(find app/build/outputs/apk/picoXr/debug -maxdepth 1 -name '*.apk' -type f)
[[ ${#apks[@]} == 1 ]] || { echo 'Expected one validation APK' >&2; exit 1; }
apk=${apks[0]}
python3 tools/audit-apk "$apk" --validation-catalog build/xrgame-runtime/bundle/manifest.json --output "$out/apk-audit.json"
"$ANDROID_HOME/build-tools/36.0.0/apksigner" verify --verbose --print-certs "$apk" > "$out/signature.txt"
cp "$apk" "$out/"
cp app/build/generated/jniLibs/buildXrGameNativePicoXrDebug/BUILD_INFO.json "$out/native-build-info.json"
cp app/build/xrgame-native/CMakeCache.txt "$out/native-cmake-cache.txt"
grep '^CMAKE_BUILD_TYPE:STRING=RelWithDebInfo$' "$out/native-cmake-cache.txt"
cp /work/logs/container.json "$out/container.json"
mkdir -p "$out/native-symbols"
cp -a app/build/generated/jniLibs/buildXrGameNativePicoXrDebug/arm64-v8a/. "$out/native-symbols/"
cp app/build/generated/jniLibs/buildGnDownloadPicoXrDebug/arm64-v8a/libgndownload.so "$out/native-symbols/"
cp app/build/xrgame-native/xr-unixlib/gamenative_xr_unixbridge.so "$out/native-symbols/"
cp app/build/xrgame-native/compile_commands.json "$out/compile_commands.json"
sha256sum "$out/$(basename "$apk")" | tee "$out/SHA256SUMS"
printf 'Verified APK: %s\n' "$out/$(basename "$apk")"
