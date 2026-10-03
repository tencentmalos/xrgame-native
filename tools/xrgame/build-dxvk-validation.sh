#!/usr/bin/env bash
# Build the pinned ARM64EC DXVK plus a recorded local fix in an isolated source copy.
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
project=${XRGAME_PROJECT_ROOT:-"$base/project"}
pin=a6764047e587178283fcde4073ae6e1410af594f
reference="$base/src/dxvk"
patch="$project/tools/xrgame/patches/dxvk-present-deferred-clears.patch"
latency_patch="$project/tools/xrgame/patches/dxvk-waitable-frame-latency.patch"
completion_patch="$project/tools/xrgame/patches/dxvk-present-gpu-completion.patch"
latency_probe=${XRGAME_DXVK_LATENCY_PROBE:-1}
[[ "$latency_probe" == 0 || "$latency_probe" == 1 ]]
cross="$base/build/arm64ec.ini"
out=${XRGAME_DXVK_OUTPUT:-"$base/output/dxvk-clear-validation"}
test "$(git -C "$reference" rev-parse HEAD)" = "$pin"
test -f "$cross"
inputs=$( { printf '%s\n' "$pin" "$latency_probe"; cat "$patch" "$cross" "$0";
    if [[ "$latency_probe" == 1 ]]; then cat "$latency_patch" "$completion_patch"; fi;
} | sha256sum | cut -c1-16)
work="$base/build/dxvk-clear/$inputs"
src="$work/source"
export PATH="$base/toolchains/python/bin:$base/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin:$PATH"
mkdir -p "$src" "$out"
if [[ ! -f "$work/source-ready" ]]; then
    python3 - "$reference" "$src" "$work" "$pin" <<'PY'
import hashlib, json, pathlib, subprocess, sys
reference, src, work = map(pathlib.Path, sys.argv[1:4])
pin = sys.argv[4]
entries = [(reference, '.', pin)]
for line in subprocess.check_output(['git', '-C', str(reference), 'submodule', 'status', '--recursive'], text=True).splitlines():
    if not line.startswith(' '):
        raise RuntimeError('Submodule does not match its gitlink: ' + line)
    commit, path, *_ = line.strip().split()
    entries.append((reference / path, path, commit))
records = []
for index, (repo, rel, commit) in enumerate(entries):
    archive = work / f'source-{index}.tar'
    subprocess.run(['git', '-C', str(repo), 'archive', commit, '-o', str(archive)], check=True)
    dest = src / rel
    dest.mkdir(parents=True, exist_ok=True)
    subprocess.run(['tar', '-xf', str(archive), '-C', str(dest)], check=True)
    records.append({'path': rel, 'commit': commit, 'archiveSha256': hashlib.sha256(archive.read_bytes()).hexdigest()})
(work / 'sources.json').write_text(json.dumps(records, indent=2) + '\n')
PY
    git -C "$src" apply --check "$patch"
    git -C "$src" apply "$patch"
    if [[ "$latency_probe" == 1 ]]; then
        git -C "$src" apply --check "$latency_patch"
        git -C "$src" apply "$latency_patch"
        git -C "$src" apply --check "$completion_patch"
        git -C "$src" apply "$completion_patch"
    fi
    touch "$work/source-ready"
fi
if [[ ! -f "$work/native/build.ninja" ]]; then
    meson setup "$work/native" "$src" --cross-file "$cross" \
        --buildtype release --prefix "$out" --bindir arm64ec
fi
meson compile -C "$work/native" -j "${XRGAME_JOBS:-12}"
meson install -C "$work/native"
python3 - "$out" "$work" "$patch" "$0" "$cross" "$latency_probe" "$latency_patch" "$completion_patch" <<'PY'
import hashlib, json, pathlib, sys
out, work, patch, recipe, cross = map(pathlib.Path, sys.argv[1:6])
latency_probe, latency_patch = sys.argv[6], pathlib.Path(sys.argv[7])
completion_patch = pathlib.Path(sys.argv[8])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
binaries = {str(p.relative_to(out)): sha(p) for p in sorted((out / 'arm64ec').glob('*.dll'))}
if not binaries:
    raise RuntimeError('No DXVK DLLs were produced')
(out / 'xrgame-dxvk-build.json').write_text(json.dumps({
    'source': 'https://github.com/tencentmalos/dxvk',
    'sources': json.loads((work / 'sources.json').read_text()),
    'patchSha256': sha(patch), 'recipeSha256': sha(recipe), 'crossFileSha256': sha(cross),
    'latencyProbePatchSha256': sha(latency_patch) if latency_probe == '1' else None,
    'gpuCompletionPatchSha256': sha(completion_patch) if latency_probe == '1' else None,
    'architecture': 'arm64ec-windows', 'purpose': 'private-validation', 'releaseReady': False,
    'binaries': binaries,
}, indent=2) + '\n')
PY
