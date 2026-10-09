#!/usr/bin/env bash
set -euo pipefail
repository=$(cd "$(dirname "$0")/.." && pwd)
sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-"$HOME/Library/Android/sdk"}}
ndk=${ANDROID_NDK_HOME:-"$sdk/ndk/27.3.13750724"}
build=${XRGAME_NATIVE_BUILD:-"$repository/app/build/xrgame-native"}
output=${1:-"$repository/app/build/generated/xrgame-native"}
cmake_bin=${CMAKE:-"$sdk/cmake/3.22.1/bin/cmake"}
ninja_bin=${NINJA:-"$sdk/cmake/3.22.1/bin/ninja"}
test -x "$cmake_bin" && test -x "$ninja_bin"
test -f "$ndk/build/cmake/android.toolchain.cmake"
args=(-DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
args+=("-DXRGAME_BUILD_PROBES=${XRGAME_BUILD_PROBES:-OFF}")
if [[ -n "${XRGAME_OPENXR_AAR:-}" ]]; then
    mkdir -p "$build/openxr"
    unzip -q -o "$XRGAME_OPENXR_AAR" -d "$build/openxr"
    OpenXR_DIR="$build/openxr/prefab/modules/openxr_loader/libs/android.arm64-v8a/cmake/openxr"
fi
if [[ -n "${OpenXR_DIR:-}" ]]; then args+=("-DOpenXR_DIR=$OpenXR_DIR"); fi
"$cmake_bin" -S "$repository/app/src/picoXr/cpp" -B "$build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja_bin" -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$output" "${args[@]}"
"$cmake_bin" --build "$build" --parallel "${XRGAME_JOBS:-8}"
"$cmake_bin" --install "$build"
case "$(uname -s)" in
    Darwin) host_tag=darwin-x86_64 ;;
    Linux) host_tag=linux-x86_64 ;;
    *) echo 'Unsupported NDK build host' >&2; exit 2 ;;
esac
cp "$ndk/toolchains/llvm/prebuilt/$host_tag/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$output/arm64-v8a/"
pulse=${XRGAME_PULSE_DIR:-"$repository/build/xrgame-runtime/pulseaudio"}
python3 - "$pulse" "$output/arm64-v8a" <<'PY'
import hashlib, json, pathlib, shutil, sys
src, out = map(pathlib.Path, sys.argv[1:])
record = json.loads((src/'xrgame-pulse-build.json').read_text())
assert record['pulseaudioPin'] == '4166ad3e30ec2a29a37d5fc1eaafcc7e03ebfe55'
for name in ['libpulseaudio.so', 'libpulse.so', 'libpulsecommon-13.0.so', 'libpulsecore-13.0.so', 'libltdl.so', 'libsndfile.so']:
    assert hashlib.sha256((src/name).read_bytes()).hexdigest() == record['sha256'][name], name
    shutil.copy2(src/name, out/name)
PY
python3 - "$repository" "$output" "$ndk" <<'PY'
import hashlib, json, pathlib, subprocess, sys
repo, out, ndk = map(pathlib.Path, sys.argv[1:])
info = {'revision': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
        'dirty': bool(subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain'])),
        'ndk': (ndk/'source.properties').read_text(), 'libraries': {}}
for path in sorted(out.glob('arm64-v8a/*.so')):
    info['libraries'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
def revision(path):
    return {'revision': subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip(),
            'dirty': bool(subprocess.check_output(['git', '-C', str(path), 'status', '--porcelain']))}
# libxrimmersive links Foundation's reconstruction, foveation, eye gaze and logging sources.
foundation = repo/'foundation'
subtrees = ['basic/underlying/core', 'basic/underlying/math', 'basic/platform/public', 'basic/allocator/public',
            'basic/async/container/public', 'basic/modules/implements/log', 'modules/log', 'modules/property',
            'modules/utils/include', 'modules/foveation', 'modules/fsr1', 'modules/upscale', 'modules/xr/include',
            'modules/xr/src/XrEyeGazeTracker.cpp', 'third_party/openxr/openxr_header/openxr_pico']
if (out/'arm64-v8a/libxrgame_debugbus.so').exists():
    subtrees += ['modules/debugbus', 'modules/profiler_ring', 'third_party/profiler_sdk/sdk', 'third_party/lz4',
                 'third_party/nlohmann_json/include']
paths = [foundation/subtree for subtree in subtrees]
info['foundation'] = revision(foundation) | {
    'sources': {str(path.relative_to(foundation)): hashlib.sha256(path.read_bytes()).hexdigest()
                for root in paths for path in ([root] if root.is_file() else sorted(root.rglob('*')))
                if path.is_file()},
}
info['references'] = {name: revision(repo/'references'/name) for name in ['Vulkan-Headers', 'fmt']}
(out/'BUILD_INFO.json').write_text(json.dumps(info, indent=2)+'\n')
PY
