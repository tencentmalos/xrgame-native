#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Internal Windows-ABI cross-build in Apple Container. Does not change the validated pin.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/opt/steampsp/runtime-local}
project=${XRGAME_PROJECT_ROOT:-/project}
llvm=${XRGAME_LLVM_MINGW:-/opt/steampsp/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64}
sdk=${XRGAME_XWIN:-/opt/steampsp/xwin}
source_archive=${XRGAME_OC_SOURCE_ARCHIVE:-/work/downloads/opencomposite/source.tar.gz}
definition=${XRGAME_VULKAN_DEF:-/work/downloads/opencomposite/vulkan-1.def}
recipe="$project/references/opencomposite"
pin="$project/tools/xrgame/opencomposite-pin.json"
key=$(cat "$pin" "$0" | sha256sum | cut -c1-16)
work="$base/build/opencomposite-clangcl/$key"
src="$work/source"
mkdir -p "$work" "$base/output/opencomposite"
python3 - "$pin" "$project" <<'PY'
import hashlib,json,pathlib,sys
pin=json.loads(pathlib.Path(sys.argv[1]).read_text()); root=pathlib.Path(sys.argv[2])
for name,digest in pin['recipeFiles'].items():
    if hashlib.sha256((root/name).read_bytes()).hexdigest()!=digest: raise SystemExit('Changed recipe: '+name)
PY
if [[ ! -f "$work/source-ready" ]]; then
    test ! -e "$src" || { echo 'Incomplete source directory; inspect it before retrying' >&2; exit 2; }
    mkdir -p "$src"
    tar -xzf "$source_archive" --strip-components=1 -C "$src"
    cp "$recipe/vendor/openvr-2.15.6/openvr.h" "$src/OpenVRHeaders/openvr-2.15.6.h"
    for patch in background-support openvr-system-026 mailbox-forward displayed-bounds-copy eye-tracked-foveation vulkan-array-submit; do
        git -C "$src" apply --check "$recipe/patches/$patch.patch"
        git -C "$src" apply "$recipe/patches/$patch.patch"
    done
    # These are nested upstream dependencies, at the recipe's reviewed revisions.
    for entry in glm:g-truc/glm:bf71a834948186f4097caa076cd2663c69a10e1e openxr-sdk:KhronosGroup/OpenXR-SDK:91a8a8d9d70f4b469bca0726122c3b5a6096010e; do
        IFS=: read -r name repo revision <<< "$entry"
        curl -fL --retry 3 --connect-timeout 15 "https://codeload.github.com/$repo/tar.gz/$revision" -o "$work/$name.tar.gz"
        mkdir -p "$src/libs/$name"
        tar -xzf "$work/$name.tar.gz" --strip-components=1 -C "$src/libs/$name"
    done
    mkdir -p "$src/libs/vulkan/Include" "$src/libs/vulkan/Lib"
    cp -a "$project/references/Vulkan-Headers/include/." "$src/libs/vulkan/Include/"
    "$llvm/bin/llvm-dlltool" -m i386:x86-64 -d "$definition" -l "$src/libs/vulkan/Lib/vulkan-1.lib"
    touch "$work/source-ready"
fi
cat > "$work/clang-cl" <<EOF
#!/usr/bin/env bash
exec "$llvm/bin/clang" --driver-mode=cl --target=x86_64-pc-windows-msvc "\$@"
EOF
chmod +x "$work/clang-cl"
cat > "$work/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER $work/clang-cl)
set(CMAKE_CXX_COMPILER $work/clang-cl)
set(CMAKE_LINKER $llvm/bin/lld-link)
set(CMAKE_RC_COMPILER $llvm/bin/llvm-rc)
set(CMAKE_RC_FLAGS_INIT "/I$sdk/crt/include /I$sdk/sdk/include/ucrt /I$sdk/sdk/include/shared /I$sdk/sdk/include/um")
set(CMAKE_C_FLAGS_INIT "/imsvc$sdk/crt/include /imsvc$sdk/sdk/include/ucrt /imsvc$sdk/sdk/include/shared /imsvc$sdk/sdk/include/um /imsvc$sdk/sdk/include/winrt /clang:-Wno-microsoft-enum-value")
set(CMAKE_CXX_FLAGS_INIT "\${CMAKE_C_FLAGS_INIT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "/manifest:no /libpath:$sdk/crt/lib/x86_64 /libpath:$sdk/sdk/lib/ucrt/x86_64 /libpath:$sdk/sdk/lib/um/x86_64")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "\${CMAKE_EXE_LINKER_FLAGS_INIT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
export PATH="$llvm/bin:$PATH"
cmake -S "$src" -B "$work/native" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$work/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DOC_VERSION=a27e7e6-gamenative-xrgame-openvr2156-mailbox-bounds \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
cmake --build "$work/native" --target OCOVR -j "${XRGAME_JOBS:-4}"
cp "$work/native/bin/vrclient_x64.dll" "$base/output/opencomposite/opencomposite_x64.dll"
python3 - "$base/output/opencomposite" "$pin" "$source_archive" "$definition" "$work" "$0" <<'PY'
import hashlib,json,pathlib,sys
out,pin,source,definition,work,recipe=map(pathlib.Path,sys.argv[1:])
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
binary=out/'opencomposite_x64.dll'
data=binary.read_bytes()
for marker in (b'xrSendMailboxMessageGNX',b'Copying only the displayed bounds',b'Eye tracker for foveation centers',b'SubmitWithArrayIndex: eye %d layer %u'):
    if marker not in data: raise SystemExit('Missing patch marker: '+repr(marker))
record={'schema':1,'sha256':sha(binary),'validatedOnDevice':False,'recipePin':json.loads(pin.read_text()),
        'sourceArchiveSha256':sha(source),'definitionSha256':sha(definition),'recipeSha256':sha(recipe),
        'dependencyArchives':{p.name:sha(p) for p in work.glob('*.tar.gz')},
        'compiler':'bylaws LLVM 21.1.0 / clang-cl MSVC ABI','configuration':'Release x64 /MD'}
(out/'clangcl-build.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({k:record[k] for k in ('sha256','validatedOnDevice')}))
PY
