#!/usr/bin/env bash
# Build the pinned runtime set. No publishing, APK upload or catalog activation.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
export XRGAME_BUILD_ROOT="$base" XRGAME_PROJECT_ROOT="$project"
tag=${XRGAME_RUNTIME_TAG:-runtime-20260926-preview1}
mkdir -p "$base/logs" "$base/output"
phase() {
    local name=$1
    shift
    printf '%s start %s\n' "$(date -u +%FT%TZ)" "$name" | tee -a "$base/logs/phases.log"
    if "$@" > "$base/logs/$name.log" 2>&1; then
        printf '%s passed %s\n' "$(date -u +%FT%TZ)" "$name" | tee -a "$base/logs/phases.log"
    else
        local result=$?
        printf '%s failed %s (%s)\n' "$(date -u +%FT%TZ)" "$name" "$result" | tee -a "$base/logs/phases.log"
        tail -n 40 "$base/logs/$name.log"
        return "$result"
    fi
}
phase wine bash "$project/tools/xrgame/build-wine.sh"
phase fex bash "$project/tools/xrgame/build-fex.sh"
phase d3d bash "$project/tools/xrgame/build-d3d.sh"
phase turnip bash "$project/tools/xrgame/build-turnip.sh"
phase xcb bash "$project/tools/xrgame/build-xcb.sh"
phase pulseaudio bash "$project/tools/xrgame/build-pulseaudio.sh"
phase gst-libav bash "$project/tools/xrgame/build-gst-libav.sh"
phase components python3 "$project/tools/xrgame/package-components.py" --root "$base" --tag "$tag"
phase imagefs python3 "$project/tools/xrgame/package-imagefs.py" --tag "$tag"
