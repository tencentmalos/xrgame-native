# Sourced inside the Linux development container.
export XRGAME_DEV_ROOT=/opt/steampsp
export ANDROID_HOME=$XRGAME_DEV_ROOT/android-sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.3.13750724
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export RUSTUP_HOME=$XRGAME_DEV_ROOT/rustup
export CARGO_HOME=$XRGAME_DEV_ROOT/cargo
export RUSTUP_TOOLCHAIN=1.98.1
export GRADLE_USER_HOME=$XRGAME_DEV_ROOT/gradle
export XRGAME_JOBS=${XRGAME_JOBS:-4}
export CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-4}
export PATH="$CARGO_HOME/bin:$ANDROID_HOME/cmdline-tools/16111833/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmake/3.22.1/bin:$XRGAME_DEV_ROOT/python/bin:$PATH"
