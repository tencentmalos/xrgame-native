#!/usr/bin/env bash
# One-way snapshot. Only this generated mirror may be deleted/overwritten.
set -euo pipefail
source /project/tools/xrgame/apple-container/environment.sh
exec 9>"$XRGAME_DEV_ROOT/.operation.lock"
flock -n 9 || { echo 'A Linux build/setup is still active.' >&2; exit 1; }
repo=$XRGAME_DEV_ROOT/project
[[ -d /project/.git ]] || { echo 'A standalone Git checkout is required (not a linked worktree).' >&2; exit 1; }
[[ -f /project/foundation/third_party/pinyinime/jni/share/pinyinime.cpp &&
   -f /project/foundation/third_party/pinyinime/res/raw/dict_pinyin.dat ]] || {
    echo 'Initialize pinyin on the host: git submodule update --init foundation; git -C foundation submodule update --init third_party/pinyinime' >&2
    exit 1
}
if [[ ! -e "$repo" ]]; then
    mkdir -p "$repo"
    touch "$XRGAME_DEV_ROOT/.managed-project"
fi
[[ "$repo" == /opt/steampsp/project && -f "$XRGAME_DEV_ROOT/.managed-project" ]] || exit 2
# Exclusions also protect existing Linux outputs/caches from --delete.
rsync -a --delete --no-owner --no-group \
    --exclude=build/ --exclude=target/ --exclude=.gradle/ --exclude=.cxx/ --exclude=.kotlin/ \
    --exclude=.DS_Store --exclude=__pycache__/ --exclude=.cache/ \
    --exclude=/local.properties --exclude=/keystore --exclude=/app/keystores/ \
    --exclude='/references/*/' --exclude='/foundation/references/' \
    /project/ "$repo/"
for dependency in references/JavaSteam references/opencomposite references/Vulkan-Headers references/fmt; do
    [[ -f /project/$dependency/.git ]] || { echo "Missing pinned submodule: $dependency" >&2; exit 1; }
    mkdir -p "$repo/$dependency"
    rsync -a --delete --no-owner --no-group --exclude=build/ --exclude=.gradle/ \
        --exclude=.DS_Store "/project/$dependency/" "$repo/$dependency/"
done
# Unmaterialized gitlinks remain empty, as with a normal uninitialized submodule.
git -C "$repo" ls-files --stage | awk '$1 == "160000" {print $4}' | while IFS= read -r path; do
    mkdir -p "$repo/$path"
done
cp "$XRGAME_DEV_ROOT/local.properties" "$repo/local.properties"
for component in bundle pulseaudio opencomposite; do
    [[ -d /project/build/xrgame-runtime/$component ]] || { echo "Missing staged runtime: $component" >&2; exit 1; }
    mkdir -p "$repo/build/xrgame-runtime/$component"
    rsync -a --delete --no-owner --no-group "/project/build/xrgame-runtime/$component/" "$repo/build/xrgame-runtime/$component/"
done
if [[ -d /project/build/xrgame-gfxr ]]; then
    mkdir -p "$repo/build/xrgame-gfxr"
    rsync -a --delete --no-owner --no-group /project/build/xrgame-gfxr/ "$repo/build/xrgame-gfxr/"
elif [[ -d "$repo/build/xrgame-gfxr" ]]; then
    # Do not silently reuse optional payloads left by a different source snapshot.
    mv "$repo/build/xrgame-gfxr" "$XRGAME_DEV_ROOT/gfxr-previous-$(date -u +%Y%m%dT%H%M%SZ)"
fi
[[ -d /project/build/xrgame-linux-steam ]] || { echo 'Stage the source-built Linux Steam userspace first.' >&2; exit 1; }
mkdir -p "$repo/build/xrgame-linux-steam"
rsync -a --delete --no-owner --no-group /project/build/xrgame-linux-steam/ "$repo/build/xrgame-linux-steam/"
mkdir -p "$repo/build/apple-container-provenance"
git -C /project rev-parse HEAD > "$repo/build/apple-container-provenance/host-head.txt"
git -C /project status --short > "$repo/build/apple-container-provenance/host-status.txt"
git -C /project submodule status > "$repo/build/apple-container-provenance/host-submodules.txt"
# Files copied from Mac may carry executable modes; all builds happen on Linux ext4.
printf 'Snapshot ready: %s\n' "$repo"
