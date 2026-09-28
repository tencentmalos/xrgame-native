#!/usr/bin/env bash
# Collect and bind build/source evidence without publishing or enabling a catalog.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
tag=${XRGAME_RUNTIME_TAG:-runtime-20260926-preview1}
mkdir -p "$base/logs"
python3 "$project/tools/xrgame/collect-runtime-sources.py" --root "$base" --output "$base/sources" \
    > "$base/logs/runtime-sources.log" 2>&1
python3 "$project/tools/xrgame/fetch-source-archives.py" --output "$base/downloads/dependency-sources" \
    > "$base/logs/dependency-fetch.log" 2>&1
python3 "$project/tools/xrgame/bundle-dependency-sources.py" \
    --cache "$base/downloads/dependency-sources" \
    --imagefs-record "$base/packages/$tag/imagefs-record.json" --output "$base/dependency-sources" \
    > "$base/logs/dependency-sources.log" 2>&1
python3 "$project/tools/xrgame/finalize-runtime.py" --root "$base" --tag "$tag" \
    --sources "$base/sources" --dependency-sources "$base/dependency-sources" \
    > "$base/logs/finalize.log" 2>&1
python3 "$project/tools/xrgame/verify-runtime-packages.py" --packages "$base/packages/$tag" \
    > "$base/logs/package-validation.log" 2>&1
