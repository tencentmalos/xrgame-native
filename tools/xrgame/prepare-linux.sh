#!/usr/bin/env bash
set -euo pipefail
base=${XRGAME_BUILD_ROOT:-/work}
mkdir -p "$base/downloads" "$base/toolchains" "$base/src" "$base/output"
fetch() {
    local url=$1 sha=$2 file="$base/downloads/$3"
    if [[ ! -f "$file" ]]; then curl -fL --retry 3 "$url" -o "$file.part"; mv "$file.part" "$file"; fi
    printf '%s  %s\n' "$sha" "$file" | sha256sum -c -
}
fetch https://dl.google.com/android/repository/android-ndk-r27d-linux.zip \
    601246087a682d1944e1e16dd85bc6e49560fe8b6d61255be2829178c8ed15d9 android-ndk-r27d-linux.zip &
ndk_job=$!
fetch https://github.com/bylaws/llvm-mingw/releases/download/20250920/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64.tar.xz \
    8dd8c34fc051a50c2fae86015f35057f8aae93fe1e19b34537ef1269a8b4c772 llvm-mingw.tar.xz &
llvm_job=$!
fetch https://github.com/GameNative/termux-on-gha/releases/download/build-20260218/termuxfs-aarch64.tar \
    b088df560c9395d7c2cc797d208d8a727d1f06fc70e5ea34eef6118ef64a7e42 termuxfs-aarch64.tar &
termux_job=$!
for job in "$ndk_job" "$llvm_job" "$termux_job"; do wait "$job"; done
if [[ ! -d "$base/toolchains/android-ndk-r27d" ]]; then unzip -q "$base/downloads/android-ndk-r27d-linux.zip" -d "$base/toolchains"; fi
if [[ ! -d "$base/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64" ]]; then tar -xJf "$base/downloads/llvm-mingw.tar.xz" -C "$base/toolchains"; fi
if [[ ! -d "$base/termuxfs/aarch64/data" ]]; then
    mkdir -p "$base/termuxfs/aarch64"
    tar -xf "$base/downloads/termuxfs-aarch64.tar" -C "$base/termuxfs/aarch64"
fi
checkout() {
    local name=$1 url=$2 revision=$3
    if [[ ! -d "$base/src/$name/.git" ]]; then
        git init "$base/src/$name"
        git -C "$base/src/$name" remote add origin "$url"
    fi
    test "$(git -C "$base/src/$name" remote get-url origin)" = "$url"
    if ! git -C "$base/src/$name" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$base/src/$name" -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=60 \
            fetch --depth 1 origin "$revision"
    fi
    if ! git -C "$base/src/$name" rev-parse --verify HEAD >/dev/null 2>&1; then
        git -C "$base/src/$name" checkout --detach "$revision"
    fi
    test "$(git -C "$base/src/$name" rev-parse HEAD)" = "$revision"
    git -C "$base/src/$name" diff --quiet HEAD --ignore-submodules=all
}
checkout proton-wine https://github.com/tencentmalos/proton-wine.git 5d0d333e7beef02fbb455cc3163b4e8e25615458
checkout ntsync-android https://github.com/tencentmalos/ntsync-android.git 7ce6435e5979b1cb5341aa4b299f31e8937fe121
checkout FEX https://github.com/tencentmalos/FEX.git 3f1f30a060b633980ed8e7674eb8d8997457edad
checkout mesa-turnip https://github.com/tencentmalos/mesa-mirror.git d15b7c019c8daa17e80051258077d9b2d5146a2b
checkout dxvk https://github.com/tencentmalos/dxvk.git a6764047e587178283fcde4073ae6e1410af594f
checkout vkd3d-proton https://github.com/tencentmalos/vkd3d-proton.git 212991fc2c266bc0d59f4c4ce8f80f7126508d71
git -C "$base/src/FEX" submodule update --init --depth 1 External/vixl External/fmt \
    External/xxhash External/range-v3 External/unordered_dense External/rpmalloc Source/Common/cpp-optparse
export RUSTUP_HOME="$base/toolchains/rustup"
export CARGO_HOME="$base/toolchains/cargo"
export PATH="$CARGO_HOME/bin:$PATH"
if [[ ! -x "$CARGO_HOME/bin/rustup" ]]; then
    fetch https://static.rust-lang.org/rustup/archive/1.29.1/x86_64-unknown-linux-gnu/rustup-init \
        dda7234360b7f578ca8b0ddcb80145646fa61a67c1720a5abc7051b35c9fcb71 rustup-init
    chmod +x "$base/downloads/rustup-init"
    "$base/downloads/rustup-init" -y --profile minimal --default-toolchain 1.98.1 --no-modify-path
fi
rustup target add --toolchain 1.98.1 aarch64-linux-android
if [[ ! -x "$base/toolchains/python/bin/python" ]]; then
    python3 -m venv "$base/toolchains/python"
fi
"$base/toolchains/python/bin/python" -m pip install \
    meson==1.8.3 Mako==1.3.10 PyYAML==6.0.2 packaging==25.0 PySocks==1.7.1 MarkupSafe==3.0.3
printf '%s\n' 'Toolchains and pinned sources prepared.'
