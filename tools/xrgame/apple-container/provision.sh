#!/usr/bin/env bash
set -euo pipefail
source /project/tools/xrgame/apple-container/environment.sh
exec 9>"$XRGAME_DEV_ROOT/.operation.lock"
flock -n 9 || { echo 'A Linux build/setup is still active.' >&2; exit 1; }
mkdir -p /work/downloads "$XRGAME_DEV_ROOT" "$ANDROID_HOME/cmdline-tools" "$ANDROID_HOME/ndk" /root/.android
fetch() {
    local url=$1 hash=$2 name=$3
    local file="/work/downloads/$name"
    if [[ ! -f "$file" ]]; then
        curl -fL --retry 3 --connect-timeout 30 "$url" -o "$file.part"
        mv "$file.part" "$file"
    fi
    printf '%s  %s\n' "$hash" "$file" | sha256sum -c -
}
fetch https://dl.google.com/android/repository/commandlinetools-linux-16111833_latest.zip \
    0877a1d048fe4a24efe2eff536ca4223f7adeb58648bb81909d33c446918cfa8 commandlinetools-linux-16111833_latest.zip
if [[ ! -x "$ANDROID_HOME/cmdline-tools/16111833/bin/sdkmanager" ]]; then
    stage=$(mktemp -d "$ANDROID_HOME/cmdline-tools/.unpack.XXXXXX")
    unzip -q /work/downloads/commandlinetools-linux-16111833_latest.zip -d "$stage"
    mv "$stage/cmdline-tools" "$ANDROID_HOME/cmdline-tools/16111833"
    rmdir "$stage"
fi
fetch https://dl.google.com/android/repository/android-ndk-r27d-linux.zip \
    601246087a682d1944e1e16dd85bc6e49560fe8b6d61255be2829178c8ed15d9 android-ndk-r27d-linux.zip
if [[ ! -f "$ANDROID_NDK_HOME/source.properties" ]]; then
    stage=$(mktemp -d "$ANDROID_HOME/ndk/.unpack.XXXXXX")
    unzip -q /work/downloads/android-ndk-r27d-linux.zip -d "$stage"
    mv "$stage/android-ndk-r27d" "$ANDROID_NDK_HOME"
    rmdir "$stage"
fi
# Reuse this developer's existing SDK license acceptances, without copying the Mac SDK.
if [[ -d /work/sdk-licenses ]]; then
    mkdir -p "$ANDROID_HOME/licenses"
    cp /work/sdk-licenses/* "$ANDROID_HOME/licenses/"
fi
sdkmanager --sdk_root="$ANDROID_HOME" 'platforms;android-36' 'build-tools;35.0.0' 'build-tools;36.0.0' 'cmake;3.22.1'
fetch https://static.rust-lang.org/rustup/archive/1.29.1/x86_64-unknown-linux-gnu/rustup-init \
    dda7234360b7f578ca8b0ddcb80145646fa61a67c1720a5abc7051b35c9fcb71 rustup-init
if [[ ! -x "$CARGO_HOME/bin/rustup" ]]; then
    # The host share need not support executable permissions.
    install -m 755 /work/downloads/rustup-init "$XRGAME_DEV_ROOT/rustup-init"
    "$XRGAME_DEV_ROOT/rustup-init" -y --profile minimal --default-toolchain 1.98.1 --no-modify-path
fi
rustup toolchain install 1.98.1 --profile minimal
rustup target add --toolchain 1.98.1 aarch64-linux-android
if [[ $(cargo ndk --version 2>/dev/null || true) != 'cargo-ndk 4.1.2' ]]; then
    cargo install cargo-ndk --version 4.1.2 --locked
fi
# Runtime rebuild tools; their presence alone does not validate a full runtime rebuild.
fetch https://github.com/bylaws/llvm-mingw/releases/download/20250920/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64.tar.xz \
    8dd8c34fc051a50c2fae86015f35057f8aae93fe1e19b34537ef1269a8b4c772 llvm-mingw.tar.xz
if [[ ! -d "$XRGAME_DEV_ROOT/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64" ]]; then
    tar -xJf /work/downloads/llvm-mingw.tar.xz -C "$XRGAME_DEV_ROOT"
fi
[[ -x "$XRGAME_DEV_ROOT/python/bin/python" ]] || python3 -m venv "$XRGAME_DEV_ROOT/python"
"$XRGAME_DEV_ROOT/python/bin/pip" install meson==1.8.3 Mako==1.3.10 PyYAML==6.0.2 packaging==25.0 PySocks==1.7.1 MarkupSafe==3.0.3
if [[ ! -f /work/signing/debug.keystore ]]; then
    echo 'Missing host debug keystore: preserve signing identity before building.' >&2
    exit 1
fi
install -m 600 /work/signing/debug.keystore /root/.android/debug.keystore
printf 'sdk.dir=%s\n' "$ANDROID_HOME" > "$XRGAME_DEV_ROOT/local.properties"
java -version
rustc --version
cargo ndk --version
"$ANDROID_HOME/cmake/3.22.1/bin/cmake" --version
printf 'provisioned-v1\n' > "$XRGAME_DEV_ROOT/.provisioned"
dpkg-query -W > /work/logs/ubuntu-packages.tsv
