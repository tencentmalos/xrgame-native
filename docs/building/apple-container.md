# Apple Container 本机开发环境

2026-10-08 用户指定：后续本机默认在 Apple Container 的 Linux 环境中编译。
源码仍在 Mac 编辑；容器用独立 Linux SDK 和 ext4 编译目录，产物导出回 Mac。
这沿用 Linux 配方，不是之前暂停的 macOS 原生工具链移植。

## 日常入口

在仓库根目录执行：

```sh
# 首次准备 / 补装固定工具链；当前机器已配置
./tools/xrgame/apple-container.sh setup

# 默认：同步源码、编译、校验、导出 APK
./tools/xrgame/apple-container.sh build

# 其他 Gradle 任务（首次先完成一次 build，生成 JavaSteam 本地依赖）
./tools/xrgame/apple-container.sh gradle :app:testPicoXrDebugUnitTest

# 只同步，或进入 Linux 编译工作区
./tools/xrgame/apple-container.sh sync
./tools/xrgame/apple-container.sh shell

./tools/xrgame/apple-container.sh status
./tools/xrgame/apple-container.sh stop
```

默认产物是 **`picoXrDebug` APK，原生 C/C++ 为 `RelWithDebInfo`**。
`build` 会删除 Linux 输出目录中的旧 APK，再重新打包，避免增量 ZIP 留洞。
随后运行 `tools/audit-apk`、APK 签名校验，保存 SHA-256、ELF Build IDs、运行时 catalog SHA、
原生 BUILD_INFO、CMake 配置、源码状态和完整日志。失败日志也保留。
`native-symbols/` 导出打包前的原生库，`compile_commands.json` 保留编译参数。
新编译的 C/C++ 库保留 DWARF；原有运行时预编译件保持原样。
调试器的源码路径映射为 `/opt/steampsp/project` → 当前 Mac 仓库绝对路径。

产物目录：`~/Library/Developer/SteamPSP/linux/artifacts/<UTC时间>/`。
当前仓库的 `build/apple-container` 软链接也指向这个持久目录；清理 Gradle build 不会删除它。
不自动安装到设备，不上传 APK。

## 当前配置

| 项目 | 配置 |
| --- | --- |
| 主机 | macOS 27.0.1 / 26A434，Apple Silicon，14 CPU、48 GiB RAM |
| Apple Container | 1.5.0，Homebrew 安装 |
| 镜像 | `steampsp-dev:ubuntu24-amd64-v1`，Ubuntu 24.04 / amd64 |
| 开发容器 | `steampsp-linux`，8 CPU、16 GiB RAM；amd64 用户态由 Rosetta 执行 |
| 持久卷 | `steampsp-dev`，128 GiB 容量的稀疏 ext4 卷，挂载 `/opt/steampsp` |
| JDK / Gradle | OpenJDK 17；主仓 wrapper 8.12.1，JavaSteam wrapper 8.12 |
| Android | platform 36，build-tools 35.0.0 / 36.0.0，CMake 3.22.1 |
| NDK | Linux r27d / 27.3.13750724，SHA 固定 |
| Rust | 1.98.1，`aarch64-linux-android`，cargo-ndk 4.1.2 |
| 其他工具 | bylaws LLVM-MinGW 20250920 Linux x86_64；固定版本 Meson / Mako 等 |

容器镜像的 OCI index digest：
`sha256:4948fb4c14cc555ec30aca2c33bb0f2817c0341da8e58714127bb79ebf74e022`。
Ubuntu apt 包版本记录在本地 `logs/ubuntu-packages.tsv`，不是仓库快照锁定，
因此不承诺跨日期逐字节可复现。

## 文件与缓存

| Mac / Linux 位置 | 用途 |
| --- | --- |
| 当前 Mac 仓库 → `/project`（只读） | 编辑的唯一源头，包括未提交修改 |
| Linux `/opt/steampsp/project` | 自动同步的编译镜像，禁止在这里保留手工修改 |
| Linux `/opt/steampsp/android-sdk` | Linux SDK、NDK、CMake |
| Linux `/opt/steampsp/{cargo,rustup,gradle}` | Linux 专用工具链及缓存 |
| Mac `~/Library/Developer/SteamPSP/linux` → `/work` | 下载缓存、签名暂存、日志、APK 导出 |

同步保留 Git 元数据及当前 pin；只复制 APK 构建需要的 JavaSteam、OpenComposite、Foundation
以及 Vulkan-Headers、fmt 和 app 原生子仓工作树，不初始化 Foundation 的 study references，也不自动更新子仓。
构建产物、`.gradle`、Rust target、Mac `local.properties` 和 release 密钥不进入源码同步。
容器单独生成 `local.properties`，不会改 Mac 的 SDK 配置。
同步采用受保护的 `rsync --delete`，只处理标记过的 Linux 镜像目录；Linux 编译输出被排除并保留。
同一环境的操作有锁，避免同步覆盖正在编译的源文件。

构建仍需要 Mac 端已有的以下经过校验的运行时暂存件，首次初始化不会擅自替换版本：

- `build/xrgame-runtime/bundle`：Proton、FEX、DXVK、VKD3D、Turnip、imagefs、Steam client。
- `build/xrgame-runtime/pulseaudio` 与 `build/xrgame-runtime/opencomposite`。
- `feature/linux_steam_support` 另需 `build/xrgame-linux-steam`：源码构建的 PRoot、会话探针、客户端兼容库和 ARM64 glibc userspace；不包含 Valve 二进制。
- 可选 `build/xrgame-gfxr`：仅存在时同步；缺失时不会复用上一轮遗留的录制载荷。

本机当前没有 staged GFXReconstruct 载荷。这不影响基础 APK 构建，但不能据此声明此 APK
具备完整 API 录制功能。要使用录制，先按 API replay 文档准备并校验 `build/xrgame-gfxr`。

`setup` 复制现有 `~/.android/debug.keystore` 到私有本地目录，权限 600，然后用于容器 debug 签名；
不生成另一把密钥，不使用仓库根部的 PKCS#12 `keystore`，不复制 release 签名配置。
密钥不会进入容器镜像、Git 或产物证据包。SDK license 使用本机已有的接受记录。

## 生命周期与配置

入口会启动 Apple Container 服务和已有开发容器；未配置系统登录自启动。
`stop` 只停止本项目容器，保留 SDK、缓存和源码镜像。不要删除 `steampsp-dev` 卷。
完整 `container system stop` 会影响其他项目，不作为日常脚本动作。

环境变量可覆盖新环境配置：
`XRGAME_CONTAINER_NAME`、`XRGAME_CONTAINER_VOLUME`、`XRGAME_CONTAINER_STATE`、
`XRGAME_CONTAINER_CPUS`、`XRGAME_CONTAINER_MEMORY`、`XRGAME_CONTAINER_IMAGE`、
`XRGAME_CONTAINER_DNS`、`XRGAME_DEBUG_KEYSTORE`。
默认是独立 Git checkout，不支持 `.git` 为指针文件的 linked worktree。
脚本会检查现有容器的源码路径、卷、镜像及只读属性，避免在错误 checkout 上构建。

更改 CPU、内存或镜像，需要在没有构建运行时重新创建**容器本身**，保留卷：

```sh
container stop steampsp-linux
container delete steampsp-linux
# 如需重新构建镜像，选用新 tag，避免静默复用旧镜像
XRGAME_CONTAINER_MEMORY=16g ./tools/xrgame/apple-container.sh setup
```

若终端被强制终止留下 `.operation-lock`，先检查该目录内 PID 及容器中的 Gradle/编译进程；
确认没有任务运行后，只删除这份锁目录再重试。Linux 侧还有文件锁保护仍在运行的构建。
`shell` 内的手动命令由操作者负责，不要同时从另一终端同步或启动构建。

## 当前网络

初测默认网关 DNS 解析失败，公共 DNS 也超时；本机网络的非回环 DNS 可以正常使用。
脚本从 `scutil --dns` 选择非回环 nameserver，也可显式设置 `XRGAME_CONTAINER_DNS`。
每次使用入口时会刷新本项目容器的 `/etc/resolv.conf`，避免办公室/VPN/家庭网络切换后
仍使用创建容器时的 DNS；不修改 Mac 的 DNS 或其他容器。

## 2026-10-11 分支组件补齐

切换 `feature/malos/swan-xr-ahb` 后，旧缓存的 Turnip xrg5 和 OpenComposite DLL
不满足新分支的版本/哈希校验。已在本机容器重新构建 Turnip xrg13（Wine X11/AHB 与
Android 两个版本）及 XCB，沿用 `build-turnip.sh`、`build-xcb.sh` 和固定源码补丁。
工作目录为持久 ext4 卷内 `/opt/steampsp/runtime-local`，不会使用 Mac SDK。

OpenComposite 的既有 Windows 配方依赖 ATL，因此本次使用 **Clang 的 MSVC ABI +
微软 SDK/CRT** 交叉编译，未改写 ATL 或修改 OpenComposite 源码。工具保存在
`/opt/steampsp/xwin`：xwin 0.10.0 获取 SDK 10.0.26100、CRT/ATL 14.44.17.14；
Clang/LLD 来自既有 bylaws LLVM-MinGW 20250920，但这个 DLL 不使用 MinGW CRT。
SDK 大小写别名补齐 `SDKDDKVer.h`、`OpenGL32.lib`、`Dbghelp.lib`。
源码仍为 pin 中的 OpenOVR commit + 四个补丁，`Release /MD`、`OCOVR`。

本地完整构建命令、源文件哈希、下载包哈希、PE imports/exports 和 DLL 保存在
`~/Library/Developer/SteamPSP/linux/runtime-candidate-20261011/opencomposite/`。
Turnip 包及打包脚本在其上一级目录。容器中保留源码和编译目录，重编译命令为：

```sh
container exec steampsp-linux cmake --build \
  /opt/steampsp/runtime-local/opencomposite/build-clangcl --target OCOVR -j4
container exec steampsp-linux bash -lc \
  'source /project/tools/xrgame/apple-container/environment.sh; XRGAME_BUILD_ROOT=/opt/steampsp/runtime-local XRGAME_PROJECT_ROOT=/project bash /project/tools/xrgame/build-turnip.sh'
```

新 DLL 使用独立哈希，记录在 `opencomposite-pin.json` 和 APK 审计清单；pin 中保留
旧 MSVC 制品的验证记录。此替代制品通过编译、PE 检查和 OpenVR ABI 编译检查，
**尚未验证 VR 游戏**。MHR 是非 VR 测试，不能补足这一项。APK 构建入口只消费已暂存且
哈希匹配的制品，不会自动下载并改 pin。原暂存目录保留为
`build/xrgame-runtime/bundle-before-swan-20261011`，本机已有缓存可以回滚。
没有修改 Mac 的 DNS/代理配置，具体 IP 不提交仓库。

Apple Container 会复用 builder，给已有 builder 再传 `--dns` 不保证改变其配置。
换网后需要检查 builder 的 DNS；仅在没有别的构建使用它时停止并重新创建 builder。
不要把 `docker` 全局别名为 `container`；完整运行时的 Docker 入口仍单独保留。

## 官方 Linux Steam 候选（2026-10-17）

此分支只为官方客户端添加 glibc userspace。游戏继续使用既有 Bionic Wine/FEX，
不进入 PRoot；JavaSteam 游戏库、下载和云同步仍保留。登录与在线游戏验收见
[进行中的验证记录](../validation/linux-steam-20261016.md)。

客户端构建使用两个本地容器：现有 amd64 `steampsp-linux` 做 NDK 交叉编译，
独立 ARM64 Ubuntu `steampsp-linux-steam-rootfs` 生成客户端 userspace。
ARM64 容器使用 Ubuntu digest
`sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55`，
仓库只读挂载到 `/project`，本机 artifacts 目录挂载到 `/output`。它不安装 Wine、FEX、
游戏或 Valve 客户端。包版本及许可证保存在 userspace 和导出的 `linux-steam-rootfs-packages.tsv`。
APT 包未做仓库快照锁定，不能声明跨日期字节复现。

Steam VGUI 即使禁用 CEF GPU 仍要求 GLX。客户端候选由 `launch-client.py` 管理
Xephyr（`:1`、Unix socket only）和官方 Steam，Xephyr 用 Mesa llvmpipe 软件渲染，
最终图像通过 `:0` 的既有 Android X server 显示；仅这一客户端路径需要额外 CPU 拷贝。
游戏继续在 PRoot 外使用 Bionic Wine/FEX/AHB，不连接 `:1`。这不是游戏 X server
或 Turnip 的替换。候选的 GLX/字体/输入和生命周期都须设备验证，不能仅以打包通过验收。

桌面图标进入 `LaunchActivity`：Official Steam 自动启动 Big Picture，Custom Steam
进入现有游戏库/设置。官方界面在每次启动时跟随 Android 系统语言；userspace 包含
Noto CJK（SC/TC/JP/KR）并生成 fontconfig 缓存，不能只带 DejaVu。升级 userspace 时
保留根目录外的 Steam HOME 和 machine-id，不重置用户登录。

构建步骤（在各自容器执行；输出路径为示例，Wine 适配库要求新建目录）：

```sh
# ARM64 容器
bash /project/tools/xrgame/linux-steam/build-rootfs.sh

# amd64 开发容器
source /project/tools/xrgame/apple-container/environment.sh
bash /project/tools/xrgame/build-linux-steam-proot.sh /opt/steampsp/linux-steam/proot
python3 /project/tools/xrgame/linux-steam/build-wine-client.py \
  /opt/steampsp/linux-steam/wine-client-new \
  --base /project/build/xrgame-runtime/bundle/components/proton-11.0-2-arm64ec-xrg3.wcp
python3 /project/tools/xrgame/linux-steam/stage.py \
  --proot /opt/steampsp/linux-steam/proot \
  --wine /opt/steampsp/linux-steam/wine-client-new \
  --rootfs /work/artifacts/linux-steam-rootfs.tar.gz \
  --output /work/artifacts/linux-steam-staged
```

现有开发容器需补装 `gawk gcc-aarch64-linux-gnu g++-aarch64-linux-gnu x11proto-dev`；
Dockerfile 已列入这些包，新建镜像会安装。PRoot 与 DroidDeck 必须是主仓 gitlink 指定版本，
不使用 `submodule update --remote`。`stage.py` 输出逐文件 SHA manifest；复制整个输出到 Mac
`build/xrgame-linux-steam` 后，正常 `apple-container.sh build` 会同步、校验和打包。
userspace 的 APK 资产后缀必须是 `.tgz`：aapt 会自动解压 `.gz` 并去掉后缀，破坏逐字节校验。

Wine 候选通过 `package-proton-validation.py --release linux-steam1` 打包，明确绑定 xrg3
基线 SHA、Wine pin 和路径补丁，只替换 `lsteamclient.so`，再用 `prepare-runtime-bundle.py`
更新私有 catalog。不能把这个增量说成完整 Proton 重编，或声称包含主干后续 xrg6/xrg7。
客户端首次打开时从 Valve CDN 下载；不会把 Valve 程序写进公开仓库或 APK。

官方 Steam 的设备条件：Android 12+ 需检查开发者选项的子进程限制。AYN/API 33
实测 Steam + Wine 超过默认数量后，系统以 `Trimming phantom processes` 杀掉 PRoot；
本机验证使用 `adb shell settings put global settings_enable_monitor_phantom_procs false`。
这是设备级设置，不由 APK 自动写入；Android 12/API 31 的 DeviceConfig 路径不同，
参考 pinned DroidDeck 的 `PhantomProcessLimit.kt`。原值与恢复方式见
[共享库验收记录](../validation/steam-shared-library-20261017.md)。

## 验证边界

初始安装阶段验证过 arm64/amd64 Ubuntu、只读源码挂载、可写产物目录，
并用 Linux NDK 对仓库实际的 `wine_exec.c` / `wine_mapper.c` 做了交叉编译。
日志保留在持久目录，包括初始默认 DNS 失败和开发环境 bootstrap 重试。

首次完整 APK 构建通过：JavaSteam 冷构建 `5m 24s`，主 APK 冷构建 `12m 34s`
（Gradle 输出，含首次配置/依赖获取；不是远端主机性能对照）。`tools/audit-apk` 检查
126 个库/资源条目，0 错误；原生 `CMAKE_BUILD_TYPE=RelWithDebInfo`。
APK 身份为 `com.tencentmalos.xrgamenative` / `SteamPSP` / targetSdk 36 / arm64-v8a，
与原 Mac APK 的签名证书一致，未进行设备安装或游戏验收。

| 首次构建证据 | 值 |
| --- | --- |
| 源码基线 | `51717b60c8458876583b9476981c845b196c38c7`，另含本次未提交的构建工具及文档 |
| 本地证据目录 | `~/Library/Developer/SteamPSP/linux/artifacts/20261008T123944Z/` |
| APK SHA-256 | `6e14c723520c2f8c6ee0dcacbda29d097609509533aa6c4d0fa112c361a49886` |
| Runtime catalog SHA-256 | `e8f2780a71024d47b4f12ff1b74616354f588573d77ae105d8c93162337732bb` |
| 签名证书 SHA-256 | `ce9d5b4d6524b793e48e00658f4d6c9d2ccfa1b1059dbb1fb34244c68cd1f8eb` |
| `libxrgame_wine_exec.so` Build ID | `d3249af2c39a46757c14457ea207817ac6f6385a` |
| `libxrgame_wine_mapper.so` Build ID | `294c1d545d7b0b35291b1b6a22eff39a8af424fe` |
| `libxrgame_debugbus.so` Build ID | `083449e15e2efdc5fc8aa1396e6397af9b708bae` |

全部 ELF Build IDs 和文件 SHA 保存在 `apk-audit.json`；主机状态记录与容器配置在同目录。
停止并重启容器后再次构建通过：JavaSteam `18s`，APK `32s`（Gradle 阶段耗时，
未包含源码同步及审计），主 APK 71 项任务中 70 项复用、1 项重新打包。
第二份证据在 `artifacts/20261008T130014Z/`，126 项审计再次通过，APK SHA-256
与首次完全一致。该目录还包含约 31 MiB 的 `native-symbols/` 与编译参数；
已检查新编译的 Wine mapper 保留 `.debug_info` / `.debug_line`。
源码镜像刻意不复制仓库根部 keystore，镜像的 Git 状态会显示该文件缺失；
以 `host-status.txt` 区分开发者实际修改和这种签名隔离。

此流程从源码重编 JavaSteam、libgndownload、
Android 原生核心和 Windows XR payload；沿用已有校验的嵌入式运行时。
完整 Proton/Wine、FEX、Turnip、DXVK/VKD3D 重新构建尚未在此环境验收，远端 Ubuntu 保留为回退。
也未比较两种主机的同负载速度，不把“省去 SSH”推定为“大型构建更快”。

官方资料：[Apple Container 1.5.0 CLI](https://github.com/apple/container/blob/1.5.0/docs/command-reference.md)、
[多架构/Rosetta](https://github.com/apple/container/blob/1.5.0/docs/multiplatform-images.md)、
[Android SDK 工具](https://developer.android.com/tools/sdkmanager)。
