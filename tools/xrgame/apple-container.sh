#!/usr/bin/env bash
# macOS entry point: edit on Mac, build in a persistent Linux x86_64 mirror.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
state=${XRGAME_CONTAINER_STATE:-"$HOME/Library/Developer/SteamPSP/linux"}
name=${XRGAME_CONTAINER_NAME:-steampsp-linux}
volume=${XRGAME_CONTAINER_VOLUME:-steampsp-dev}
image=${XRGAME_CONTAINER_IMAGE:-steampsp-dev:ubuntu24-amd64-v1}
cpus=${XRGAME_CONTAINER_CPUS:-8}
memory=${XRGAME_CONTAINER_MEMORY:-16g}
command=${1:-build}
[[ $# == 0 ]] || shift
usage() {
    echo "Usage: $0 {setup|build|gradle [args...]|sync|shell|status|stop}"
    echo 'setup: install pinned SDK/Rust tools; build: sync, build RelWithDebInfo debug APK, audit and export'
}
case "$command" in setup|build|gradle|sync|shell|status|stop) ;; -h|--help) usage; exit 0 ;; *) usage >&2; exit 2 ;; esac
[[ $(uname -s) == Darwin ]] || { echo 'Run this entry point on macOS.' >&2; exit 1; }
case "$command" in
    setup)
        python3 "$root/tools/xrgame/check-apk-inputs.py" --init-submodules --sources-only
        ;;
    build|gradle|sync)
        python3 "$root/tools/xrgame/check-apk-inputs.py"
        ;;
esac
command -v container >/dev/null || { echo 'Install Apple Container first: brew install container' >&2; exit 1; }
mkdir -p "$state/logs" "$state/artifacts"
if [[ "$command" == status ]]; then
    container inspect "$name"
    container volume inspect "$volume"
    exit
fi
# Serialize snapshot/build/provision operations across terminals; never overwrite an active build.
if ! mkdir "$state/.operation-lock" 2>/dev/null; then
    echo "Another operation is active (or an interrupted lock remains): $state/.operation-lock" >&2
    exit 1
fi
printf '%s\n' "$$" > "$state/.operation-lock/pid"
trap 'rm -f "$state/.operation-lock/pid"; rmdir "$state/.operation-lock"' EXIT
if [[ "$command" == stop ]]; then container stop "$name"; exit; fi
container system start --disable-kernel-install >/dev/null
if ! container image inspect "$image" >/dev/null 2>&1; then
    [[ "$command" == setup ]] || { echo "Run $0 setup first." >&2; exit 1; }
    dns=${XRGAME_CONTAINER_DNS:-$(scutil --dns | awk '/nameserver\[[0-9]+\]/ && $3 !~ /^(127\.|::1$)/ {print $3; exit}')}
    dns_args=()
    [[ -z "$dns" ]] || dns_args=(--dns "$dns")
    container build "${dns_args[@]}" --platform linux/amd64 --cpus 4 --memory 4g --progress plain \
        -t "$image" -f "$root/tools/xrgame/apple-container/Dockerfile" "$root/tools/xrgame/apple-container" \
        2>&1 | tee "$state/logs/image-build-$(date +%Y%m%dT%H%M%S).log"
fi
if ! container inspect "$name" >/dev/null 2>&1; then
    container volume inspect "$volume" >/dev/null 2>&1 || container volume create -s 128G "$volume"
    dns=${XRGAME_CONTAINER_DNS:-$(scutil --dns | awk '/nameserver\[[0-9]+\]/ && $3 !~ /^(127\.|::1$)/ {print $3; exit}')}
    dns_args=()
    [[ -z "$dns" ]] || dns_args=(--dns "$dns")
    container run -d --name "$name" --platform linux/amd64 --rosetta --cpus "$cpus" --memory "$memory" \
        "${dns_args[@]}" --label xrgame.dev=1 \
        --mount "type=bind,source=$root,target=/project,readonly" \
        --mount "type=bind,source=$state,target=/work" \
        -v "$volume:/opt/steampsp" --workdir /opt/steampsp \
        "$image" sleep infinity
else
    container start "$name" >/dev/null 2>&1 || true
fi
# Fail on unrelated/older containers rather than deleting or changing their data.
container inspect "$name" > "$state/logs/container.json"
python3 - "$state/logs/container.json" "$root" "$state" "$volume" "$image" <<'PY'
import json, os, sys
config = json.load(open(sys.argv[1]))[0]['configuration']
mounts = {m['destination']: m for m in config['mounts']}
assert config.get('labels', {}).get('xrgame.dev') == '1', 'Container is not managed by this helper'
assert config['image']['reference'] == sys.argv[5], 'Container image differs; recreate it using the documented steps'
for destination, source in [('/project', sys.argv[2]), ('/work', sys.argv[3])]:
    assert os.path.realpath(mounts[destination]['source']) == os.path.realpath(source), f'Wrong {destination} source'
assert 'ro' in mounts['/project']['options'], 'Source mount must be read-only'
assert mounts['/opt/steampsp']['type']['volume']['name'] == sys.argv[4], 'Wrong development volume'
PY
container exec "$name" bash -c 'test -d /opt/steampsp && test -f /project/tools/xrgame/apple-container/environment.sh && command -v javac' >/dev/null || {
    echo 'Existing container has an incompatible configuration; see docs/building/apple-container.md.' >&2; exit 1;
}
# A persistent container retains the DNS server from its creation network. Refresh
# only this managed container when the Mac moves between office/VPN/home networks.
dns=${XRGAME_CONTAINER_DNS:-$(scutil --dns | awk '/nameserver\[[0-9]+\]/ && $3 !~ /^(127\.|::1$)/ {print $3; exit}')}
if [[ -n "$dns" ]]; then
    container exec "$name" bash -c 'printf "nameserver %s\n" "$1" > /etc/resolv.conf' bash "$dns"
fi
if [[ "$command" == setup ]]; then
    mkdir -p "$state/signing"
    chmod 700 "$state/signing"
    debug_key=${XRGAME_DEBUG_KEYSTORE:-"$HOME/.android/debug.keystore"}
    [[ -f "$debug_key" ]] || { echo "Existing debug signing key required: $debug_key" >&2; exit 1; }
    cp "$debug_key" "$state/signing/debug.keystore"
    chmod 600 "$state/signing/debug.keystore"
    sdk=${ANDROID_HOME:-"$HOME/Library/Android/sdk"}
    if [[ -d "$sdk/licenses" ]]; then
        mkdir -p "$state/sdk-licenses"
        cp "$sdk/licenses/"* "$state/sdk-licenses/"
    fi
    container exec "$name" bash /project/tools/xrgame/apple-container/provision.sh \
        2>&1 | tee "$state/logs/provision-$(date +%Y%m%dT%H%M%S).log"
    exit
fi
container exec "$name" test -f /opt/steampsp/.provisioned || { echo "Run $0 setup first." >&2; exit 1; }
if [[ "$command" == shell ]]; then
    container exec -it -w /opt/steampsp/project "$name" bash --rcfile /project/tools/xrgame/apple-container/environment.sh
    exit
fi
container exec "$name" bash /project/tools/xrgame/apple-container/sync.sh
case "$command" in
    sync) ;;
    build)
        run=$(date -u +%Y%m%dT%H%M%SZ)
        container exec "$name" bash /project/tools/xrgame/apple-container/build.sh "$run"
        echo "Artifacts: $state/artifacts/$run"
        ;;
    gradle)
        container exec "$name" bash -c \
            'source /project/tools/xrgame/apple-container/environment.sh; cd /opt/steampsp/project; exec flock -n /opt/steampsp/.operation.lock ./gradlew --no-daemon --console=plain --max-workers=4 "$@"' bash "$@"
        ;;
esac
