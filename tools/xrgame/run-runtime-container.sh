#!/usr/bin/env bash
# Linux x86_64 entry point. Refuse nonempty output roots; inputs may be cached.
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 /absolute/new-build-root [/absolute/download-cache]" >&2
    exit 2
fi
base=$1
cache=${2:-}
project=$(cd "$(dirname "$0")/../.." && pwd)
if [[ "$base" != /* || "$base" == / || -e "$base" ]]; then
    echo 'Build root must be an absolute, nonexistent directory' >&2
    exit 2
fi
if [[ -n "$cache" && ( "$cache" != /* || ! -d "$cache" ) ]]; then
    echo 'Download cache must be an absolute existing directory' >&2
    exit 2
fi
mkdir -p "$base/downloads" "$base/logs"
# Freeze the recipe inputs before a long build; edits in the caller's checkout
# must not change the source record halfway through the run.
mkdir -p "$base/project/tools" "$base/project/app/src/main/windows/openxr_runtime"
cp -a "$project/tools/xrgame" "$base/project/tools/"
cp -a "$project/app/src/main/windows/openxr_runtime/builtin" \
    "$base/project/app/src/main/windows/openxr_runtime/"
cp "$project/LICENSE" "$project/THIRD_PARTY_NOTICES" "$base/project/"
project="$base/project"
if [[ -n "$cache" ]]; then
    # Copy only known archives. Every one is checked by its recipe before use.
    for name in android-ndk-r27d-linux.zip llvm-mingw.tar.xz termuxfs-aarch64.tar \
                rustup-init libxcb-1.17.0.tar.xz xcb-proto-1.17.0.tar.xz; do
        if [[ -f "$cache/$name" ]]; then cp "$cache/$name" "$base/downloads/$name"; fi
    done
fi
docker build --build-arg "XRGAME_BASE_IMAGE=${XRGAME_BASE_IMAGE:-ubuntu:24.04}" \
    -f "$project/tools/xrgame/Dockerfile" -t xrgame-runtime-build "$project/tools/xrgame" \
    > "$base/logs/container-build.log" 2>&1
docker image inspect xrgame-runtime-build > "$base/logs/container-image.json"
docker run --rm --network "${XRGAME_DOCKER_NETWORK:-bridge}" \
    -v "$base:/work" -v "$project:/project:ro" \
    -e XRGAME_BUILD_ROOT=/work -e XRGAME_PROJECT_ROOT=/project \
    -e HTTPS_PROXY -e HTTP_PROXY -e ALL_PROXY -e NO_PROXY \
    -e "XRGAME_JOBS=${XRGAME_JOBS:-8}" xrgame-runtime-build bash -euo pipefail -c '
        dpkg-query -W > /work/logs/host-packages.txt
        bash /project/tools/xrgame/prepare-linux.sh > /work/logs/prepare.log 2>&1
        bash /project/tools/xrgame/build-runtime.sh
        bash /project/tools/xrgame/record-runtime.sh
    ' > "$base/logs/driver.log" 2>&1
