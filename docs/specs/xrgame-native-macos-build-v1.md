# XRGame Native：macOS 独立构建与预编译依赖规格 v1

> 2026-09-28 用户最新调整：暂停实施（ROI 低），先推进 Present fence 异步化与 MHW 对照。以下评估保留供后续恢复。

日期：2026-09-28。状态：**评估完成，迁移待实施；不代表全组件已在 Mac 重建通过**。

本规格响应用户“新增 spec，系统评估 Mac / NDK 构建所有相关组件及预编译件入库”的要求。
随后确认：允许使用配套 LLVM-MinGW，**以 Mac 上能完整构建、出包为先，进一步替换工具链或
源码化困难依赖按需进行**。因此近期出口允许明确列出的预编译例外，不能称为全源码重建。

本文补充[个人版 v2](xrgame-native-personal-v2.md)，不改变 P1–P3 的游戏运行目标、AYN 验证范围、
Swan 延后及 `malos/main` 主干政策。此次交付仅包括评估和规格，没有迁移生产脚本、更新运行时
gitlink、上传二进制或切换设备运行时。此前 Linux 迁移延期对本轮规格评估不再构成限制；
去 X11、替换 imagefs 架构、接入 Foundation 音频/输入不属于本规格。

## 1. 目标、边界与结论

目标主机是 **Apple Silicon macOS**。在该主机上完成宿主工具运行、交叉编译、运行时归档和
`picoXrDebug` APK 组装；正式构建路径不调用 Ubuntu、Docker/Linux VM、远程 SSH、Windows
构建机或 Android 设备来完成编译。Android 设备仅负责验收。Intel Mac 不属于首轮出口。

“Mac + NDK”包含必要的 macOS 构建工具：Xcode Command Line Tools、JDK/Android SDK、
Gradle、Rust/cargo-ndk、CMake/Ninja、Python/Meson、Autotools 和 Windows 交叉工具链。
它不表示只有 NDK 一个软件包，也不要求本轮自举编译 JDK、NDK 或所有 Maven 依赖。

结论：

1. App、X server、Presenter、DebugBus 和 XR 桥已有 Mac 构建路径；不必先改运行架构。
2. FEXCore 和 Turnip 有 shadPS4 的 Mac/NDK 构建经验。XRGame 的 FEX 是 Windows DLL +
   Android unixlib，Turnip 带 Wine ICD 和 X11/AHB 扩展，不能用 shadPS4 产物直接替换。
3. Wine、FEX PE、DXVK/VKD3D 的 macOS 构建具有可行路径；真正的工作是宿主生成器、
   Windows ABI/运行库、交叉 sysroot 和现有补丁的完整衔接，不是把 `linux` 字符串替换掉。
4. **主要未闭合项是 imagefs 依赖链和 GBE 客户端。** 当前 imagefs 使用 Termux 快照；GBE
   loader 可交叉构建，但已有客户端 DLL 是 MSVC 源码构建，尚无完整的 Mac 重建配方。
5. 本轮没有证明某个开源运行时组件在原理上无法由 Mac 构建。下文的“待验证/例外”不能写成
   “必须 Ubuntu”。保留当前已验证产物，是工程安排，不是技术不可行的证明。

### 两个独立出口

| 出口 | 要求 | 能否包含预编译运行时 |
|---|---|---|
| **M：Mac 独立出包（近期必做）** | Mac 构建可维护组件，其他依赖有显式锁定、来源及例外记录；整个构建过程不需要其他 OS | 可以，清单必须完整；构建报告标记 `macos-with-prebuilts` |
| **S：受管运行时全源码重建（后续按需）** | 本文组件矩阵及 imagefs 运行依赖全部从固定源码在 Mac 生成，官方二进制依赖也逐项重建或列为基础 SDK 例外 | 不能复用 Linux/Windows 生成的运行时；未关闭例外不得宣称 S 完成 |

下载固定的 SDK、源码和预编译例外不等于依赖远端执行构建。填充缓存后还应能离线重复出包。
“Mac 上构建 Android 游戏运行时”不表示这些游戏可直接在 macOS 运行。

## 2. 已核实的起点

评估主仓为 `2ac13287bb3b5d6becd5287ce8a615f445ae3773`，工作区已有 MHR/MHW 修改保留。
组件尺寸、当前 catalog、81 个实际 imagefs 包、82 项依赖锁及编译探针见
[机器可读评估记录](../validation/macos-build-assessment-20260928.json)。

- 当前验收 catalog SHA-256：`e8619cda78b93fbde4e3f509157476f08db3ecb04749215e1fa4eb885984f353`。
  只统计它选中的 7 个归档，不统计本机 `bundle/components` 内遗留的旧版本。
- 源码基线与维护分支须分开记录：例如已验证配方仍基于 Wine `5d0d333e`、FEX `3f1f30a0`、
  Turnip `d15b7c01`、GBE `7a319f0b` 加补丁；当前 gitlink 的 Wine、Turnip、GBE 已经前进。
  主机迁移首轮保持原配方语义，不能把 rebase 后源码和旧补丁重复应用。
- shadPS4 的 `scripts/android/build-fexcore-android` 使用 NDK r29、`BUILD_FEXCORE_ONLY=ON`；
  文档记录 Mac 构建与 bionic 探针。对应静态库嵌入路线不是 Wine ARM64EC DLL 路线。
- shadPS4 的 `references/mesa-turnip-xr-fdm2/bin/build-android-turnip-kgsl.sh` 已处理 Darwin
  host tag；XRGame 还需移植 XCB/DRI3/Present 和 Wine 导出配置。
- 本机 NDK r27d、r29 的 clang 都含原生 arm64 Mach-O slice；目录名 `darwin-x86_64`
  不表示必须 Rosetta。SDK 内其他实际执行的工具仍须逐一检查，不能据 clang 推及全部 SDK。

## 3. 工具链方案

### 3.1 采用成套 macOS 工具链，先保留运行基线

| 输出 | 工具链 | 约束 |
|---|---|---|
| Android arm64 ELF、JNI、unixlib | macOS NDK r27d 为当前基线 | r29 只在具体组件需要时单独验证；不连带升级整个 App |
| Windows x64、ARM64、ARM64EC PE | macOS LLVM-MinGW，含 MinGW 头文件、导入库、CRT/C++ 库和工具 | 初始候选为与 Linux 基线同发布版本的 `bylaws/llvm-mingw 20250920` macOS 包 |
| Wine 的 `widl`、`winebuild`、`winegcc`、`wrc`、`makedep` 等 | 当前 Wine 配方源码，在 Mac 编译为宿主工具 | 必须可在 Mac 执行，不能复用 Linux ELF 生成器 |
| 其他生成器：`protoc`、GLib/ICU 工具、`wayland-scanner` 等 | 与目标依赖版本匹配的 macOS 工具 | 与 Android/Windows target 库分别构建并记录 |

这里选择 `20250920` 是为了减少首轮迁移变量，**不是要求永久锁定该版本或该 fork**。
用户已选择先跑通 Mac，不继续开展“只保留一套 LLVM”的工具链优化项目。
后续版本能通过相同 Wine/FEX/D3D ABI 和游戏回归即可替换；采用的新直接源码依赖须先 fork
到 `tencentmalos`。本轮下载公开工具链用于评估，不构成已采用新运行时版本。

### 3.2 已运行的小探针及其边界

已在当前 Mac 下载并核对 GitHub asset digest：

| 工具链 | 压缩包字节数 | SHA-256 | 本轮结果 |
|---|---:|---|---|
| bylaws 20250920 macOS universal；LLVM 21.1.0 | 118750648 | `30681fa016b4a8a5dad4918193f5f4f99a01361b9f44810cf2337cf74fdbf81c` | x64、ARM64、ARM64EC 的含 Windows 头文件、STL、TLS、try/catch 的 DLL 均编译链接成功 |
| mstorsjo 20260922 macOS universal；LLVM 23.1.2 | 124289672 | `52e5f5a7b131021d0c39a37a38fa380a1da7885cd04bd61afd0cd4ecfb8bc1f3` | 同上；仅作为替代能力对照，不自动升级配方 |

NDK r27d/r29 可生成最小 ARM64EC COFF 对象，但独立使用时找不到 `windows.h`。
这证明编译器后端与完整 Windows 开发环境是两个层次。上述 DLL **尚未在 Wine 执行**，
不证明完整 FEX、异常跨模块传播或 ARM64X 混合 DLL 已可用。

`-marm64x` 直传不同 clang target 的成功、拒绝或 unused warning 均保留在评估记录中；
Wine 的 `winegcc` 自己处理该参数，因此这些小探针不能替代 Wine 实际链接流程，也不能仅凭
参数被拒绝判定某工具链无法构建 Proton。ARM64X 出口必须检验最终 PE 和设备混合调用。

工具链来源：[bylaws 发布资产](https://github.com/bylaws/llvm-mingw/releases/expanded_assets/20250920)、
[官方 LLVM-MinGW macOS 配方](https://github.com/mstorsjo/llvm-mingw/blob/20260922/release-macos.sh)。

## 4. 组件覆盖与迁移评估

“已有”指已有 Mac 路径/既往记录；“可行待验”指源码和工具链存在可行路线，未完整重建；
“例外候选”指 M 出口可先使用固定预编译件。难度是相对工作量判断，不是工时承诺。

| 组件/输出 | 当前状态 | Mac 路线、风险与最低验收 | 预编译安排 |
|---|---|---|---|
| App、JavaSteam、安装/云同步逻辑 | 已有；低 | JDK17、SDK36、Gradle，本地 JavaSteam Maven；禁止回退 SNAPSHOT | Maven/SDK 属基础构建输入；自有 JavaSteam 继续源码构建 |
| `libgndownload.so`、Rust ntsync 静态库 | 前者已有，后者待验；低–中 | Mac Rust + `aarch64-linux-android` + NDK；锁 Cargo.lock/vendor，校验 FFI/链接；不能误用 host Rust 库 | ntsync 若受阻记录原因，不能伪称 Wine 全源码 |
| X server 连接器、共享内存、Vulkan Presenter、evshim、7-Zip 解包器及其他 picoXr CMake targets | 已有；低 | 原有 CMake/NDK；保持 AHB、4 KiB 行为及 ELF Build ID | 不作为默认例外，日常修改必须可本机编译 |
| adrenotools、LSFG 编译依赖、XR native 模块 | 已有构建入口；低–中 | 保留 CMake 实际依赖和原 gitlink；只评估 picoXr 审计后的活跃路径 | 不因此纳入上游闭源驱动或未启用二进制 |
| Foundation DebugBus/profiler ring | 已有；低 | 使用授权的私有子仓，最小模块集；必要修改另切分支 | 不得把 Foundation 二进制转存公共预编译仓 |
| XR x86/x64 CRT-less PE、Android unixbridge | 已有；低 | NDK clang、手写导入 `.def`、CMake；Wine builtin 归下一行 | 无需为这些桥接 DLL 引入 MSVC |
| Proton 11/Wine、wineserver、ntdll、winevulkan、winex11、XR builtin、bootstrap | 可行待验；高 | Mac 宿主 Wine tools + NDK + LLVM-MinGW + Android sysroot；分别验证 ELF/PE、ARM64X、加载/重启和安装目录 | 整包短期回退候选；有例外时标记为预编译 Proton |
| WineDbg x64 guest debugger、Windows probes | 可行待验；中 | 复用 Mac Wine tools/MinGW；guest attach、暂停、恢复和清理回归 | guest debugger 可暂存已验证 helper，符号与源码必须匹配 |
| FEX ARM64EC/ARM64 DLL、Android unixlib | 可行待验；中 | PE 用 LLVM-MinGW，unixlib 用 NDK；保留 4 KiB、callback guard、single-step 补丁；检查 Windows 输出安装目标 | 不用 shadPS4 静态库顶替；失败可回退固定完整 FEX 组件 |
| FEXCore NDK 静态嵌入路线 | shadPS4 已有；不是替代任务 | 可借鉴宿主脚本、符号和诊断能力；其 NDK r29 `atomic_ref` 要求不自动作用于 Wine PE 构建 | 不纳入当前 DLL 替换出口 |
| DXVK D3D8–11、DXGI | 可行待验；中 | Mac Meson + MinGW + Wine widl；保留 deferred-clear 修复及当前 DLL 集 | 不准靠删 D3D10/11 等 DLL 获得出包通过 |
| VKD3D-Proton D3D12 | 可行待验；中 | 同上；验证库、生成器及 ARM64EC ABI，D3D12 游戏回归 | 可暂时整体固定，不混入另一版 DXGI |
| Turnip Wine ICD + Android HAL、X11/AHB WSI | 可行待验；中 | 基于 shadPS4 分支，Mac NDK/Meson，保留 ICD export、KGSL、Present-wait、AHB、X11 配置 | 先编当前相同配置，不借机更换驱动基线 |
| XCB DRI3/Present 客户端及 X11 依赖 | 可行待验；中 | Mac Python xcb-proto 生成器 + Autotools + NDK；封闭 pkg-config，不能链接 Homebrew X11 | 必须与 Turnip/Wine 协议配置一致；不是 App 的 X server |
| PulseAudio daemon、libpulse、AAudio sink、sndfile/ltdl | 可行待验；中 | 改 Linux host 路径、GNU 工具假设和 host/target 生成器；保持 App 路径、AAudio 模块、归档结构 | M 首轮允许保留现有 Pulse 产物及逐文件 hash |
| GStreamer、gst-libav、FFmpeg 及媒体插件 | 部分源码 + 快照；高 | 先 Mac 重建已有 gst-libav 配方；完整依赖按 §5 拆分。保留全部 18 个显式插件根 | imagefs 例外的一部分；不得遗漏曾导致 MHR 黑屏的 `libgstdebug.so` |
| GBE `steamclient_loader_x64.exe` | 可行待验；低–中 | 现有脚本允许指定 MinGW；保持 lifetime/remote-export/process-family 补丁 | 可先用已验证 loader，之后本机重建 |
| GBE `steamclient64.dll`、`steamclient_extra_x64.dll` | **现有产物为 MSVC 构建**；高 | 需移植实际 Windows targets 及静态依赖，不是运行 `build_linux_premake.sh` 生成 Linux 库；确认 MSVC 扩展、ABI、TLS、hook、protobuf/curl/mbedtls 等依赖 | 优先例外候选；完整依赖许可声明尚未关闭，不能直接公开上传 |
| base imagefs、字体、配置和共享库链 | 部分源码 + 快照；高 | Mac 可重写归档器和依赖扫描；“重新打包快照”不等于源码重建 81 个包 | **近期主要例外候选**；当前完整对应源码证据仍不完整 |
| 官方 AAR/JNI：OpenXR loader、AndroidX 两库、libarchive-jni、zstd-jni | 当前使用官方预编译；中 | M 保留官方坐标/哈希；S 逐一对应源码版本、fork、Mac Gradle/NDK 重建；不能只统计七个 runtime 包 | 明确 SDK/Maven 例外，避免宣称 APK 全部 native 均自编 |
| NDK `libc++_shared.so`、公共后缀表、字体数据 | 基础工具链/数据输入 | NDK NOTICE、MPL/字体许可、版本与 hash；数据复制与编译分别标记 | 可按原许可固定输入；不是无法 Mac 编译的组件 |
| OpenComposite/OpenVR | 尚未选用 | 延续原计划，选定 OpenVR 后新增组件配方与验收 | 不拿历史脚本下载的 DLL 作为默认兜底 |

APK 覆盖范围最终由 `tools/xrgame/apk-sources.json`、`picoXr` CMake、Gradle resolved artifacts、
catalog 与归档内文件的并集决定。发现未分类 ELF/PE/JAR/AAR 要使审计失败，而非默默保留。

## 5. imagefs：完整依赖范围与真正难点

本次检查锁文件有 **82 项**，当前 media6 imagefs 实际归属 **81 个包、579 个记录文件**；
差项为 `libandroid-stub`（锁中的构建输入，未作为当前包归属进入 imagefs）。不能把旧 README
中的“81”当成完整构建依赖数。逐项版本、许可原文、配方和分类均在评估 JSON 的 `dependencies`。

| 组 | 包范围 | Mac 迁移判断和阻塞原因 |
|---|---|---|
| 基础库 | brotli、bz2、expat、ffi、iconv、lzma、png、xml2、pcre2、zlib | 常规交叉编译候选；锁 Android 补丁/编译开关，不能用 macOS 同名 dylib |
| Android 兼容层 | execinfo、glob、posix-semaphore、shmem、stub、support、sysv-semaphore | 小型 NDK 库，需纳入配方目录内源码。没有独立 source URL 不等于没有源码 |
| Host 生成器/字体 | glib、icu、fontconfig、freetype、ttf-dejavu | GLib/ICU 生成器必须是 Mac 可执行文件，目标库与数据版本匹配；字体为数据复制 |
| X11/窗口/图形辅助 | libx11/xau/xdmcp/xcursor/xext/xfixes/xi/xrandr/xrender/xss、xkbcommon、wayland、decor、glvnd、glslang、vulkan-loader-generic、ocl-icd、sdl2 | 分离协议生成器和 Android 库；审计动态加载根。不能因未使用 Wayland 窗口就删掉仍在 DT_NEEDED 链上的库 |
| 音频/媒体框架 | alsa-lib、pulseaudio、libsndfile、gstreamer、gst-plugins-base/good、ffmpeg | 完整功能配置与传递依赖多；目前不能把重编 gst-libav 当成已重编整个媒体栈 |
| 编解码/媒体数据 | game-music-emu、aom、bluray、dav1d、flac、mp3lame、mpg123、ogg、opencore-amr、openmpt、opus、rav1e、soxr、theora、udfread、vo-amrwbenc、vorbis、vpx、webp、x264、x265、svt-av1、xvidcore、littlecms | C/C++/Rust 多构建系统；rav1e 的 Rust/vendor、架构汇编和编译期开关需冻结。先保留兼容配置，不以删 codec 回避问题 |
| 通信/密码/系统辅助 | dbus、gmp、gnutls、idn2、nettle、sodium、srt、ssh、unistring、zmq、openssl | 交叉 configure、汇编与特性检测；不允许执行 Android conftest 或误发现 Homebrew 库 |

这些分组是风险初筛，**未逐包完成 Mac 编译**，不能给整个组标为通过。
直接把 Termux 构建框架搬到 Mac 还要处理 Linux 宿主工具、固定前缀、sysroot 补丁及
configure 缓存。近期保留已验证依赖 sysroot；后续按实际要改的库拆出 Mac 配方，不先移植
完整 Termux 包管理系统。框架流程可参考 [Termux 构建文档](https://github.com/termux/termux-packages/wiki/Building-packages)。

完整依赖发现必须同时包括：Wine `config.h` 的 SONAME/dlopen 根、ELF DT_NEEDED、Vulkan ICD、
GStreamer 插件根、字体/配置与 GBE 静态库。仅 `readelf -d` 递归不能发现所有运行依赖。
保留 libwebp 的旧配方覆盖；跟踪 ICU 许可替换、rav1e vendor 及所有构建期二次下载。
源码锁中 `completeCorrespondingSource: false` 不能在换主机后自动改成 true。

## 6. 失败与例外规则

| 情况 | 原因分类 | 处理 |
|---|---|---|
| NDK 单独构建完整 Windows C++ 组件 | 缺 Windows headers/CRT/import libs；已观察到 `windows.h` 缺失 | 使用配套 LLVM-MinGW；不再为“NDK only”改造工具链 |
| 原封不动调用当前 Ubuntu 脚本 | Linux 可执行工具、绝对路径、GNU/BSD 差异 | 改宿主选择及工具发现；不是组件本身不可移植 |
| 在 Mac 原生执行 MSVC `cl.exe` 或 Linux ELF 生成器 | 可执行格式/宿主 OS 不匹配 | 移植目标到 Mac 交叉工具链；或将最终目标产物登记为预编译例外 |
| GBE 完整客户端/复杂媒体链短期未通过 | 尚无完整 Mac 配方，或有实际编译/ABI 失败 | 保留失败命令和日志，记录精确组件、传递依赖、失效条件与后续入口；不能写笼统“Linux 必需” |
| Foundation 或闭源驱动转存公共仓 | 现有授权/源码约束不允许 | 不得借预编译件入库绕过；Foundation 仍是授权私有构建依赖，闭源驱动不进入路径 |

例外必须写明：组件/架构/版本/hash、构建来源、为什么本轮不重建、是否可修改、相应源码
和补丁位置、与谁有 ABI 耦合、公开/内部范围、升级时必须重验的场景。
缺少配方或本机工具未安装只记“未验证”，不能推断“不可能”。

## 7. 预编译件直接存仓库的可行性

### 7.1 当前真实体积

以下是当前 catalog 选中且 SHA 校验通过的压缩产物；不含 APK、工具链、符号、源码归档、
PulseAudio 的独立 JNI/模块文件和 Maven 缓存。

| 组件 | 字节数 | MiB |
|---|---:|---:|
| Proton | 80556080 | 76.824 |
| imagefs | 45766516 | 43.646 |
| GBE | 9201615 | 8.775 |
| DXVK | 5353864 | 5.106 |
| Turnip | 2957396 | 2.820 |
| VKD3D | 1848776 | 1.763 |
| FEX | 1314124 | 1.253 |
| **合计** | **146998371** | **140.189** |

GitHub 普通 Git 对超过 50 MiB 的单文件提示警告，超过 100 MiB 拒绝；当前 Proton 超过警告线，
七包各自都未超过拒绝线。把整套再封装成一个归档时必须重新测量大小，不能假定可直接提交。
两个评估用 macOS LLVM-MinGW 压缩包均超过 100 MiB，不适合普通 Git。
规则核对日期为 2026-09-28，见 [GitHub 大文件说明](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github)。

| 方式 | 技术上可行性 | 成本/限制 | 本项目建议 |
|---|---|---|---|
| 主仓普通 Git 直接提交 runtime 包 | 当前单包大小允许 | 频繁替换压缩二进制会累积历史；删除当前文件不删历史 | 仅考虑少量、稳定、许可和来源已闭合的小型例外；不默认加入整套 |
| Git LFS | 可存大文件，提交中保留指针 | 需要 LFS 下载、存储/流量预算；普通 clone 或源码 zip 不必然带实体 | 用户坚持随仓获得产物时优先；CI 必须检测未展开指针，离线前先填充对象 |
| 独立 `tencentmalos` 预编译仓 + 固定 gitlink，可配 LFS | 隔离主仓历史，可独立设私有权限 | 多一层 pin/授权；普通 Git 本身仍累积二进制历史 | **预编译例外较多时首选仓库方案**；名称待实施确定，本轮不创建 |
| Release/制品存储 + 主仓 lockfile | 适合大组件和工具链；现有 catalog 可复用 | 首次需获取，必须提供离线缓存；公开发布仍受源码门槛约束 | 适合完整运行时/工具链；不上传 APK、Foundation 或无权再分发的内容 |

按每次整套压缩内容完全变化估算，10 套仅目标包原始体积约 1.37 GiB；这是容量预算，
不是已测 Git pack 增长。LFS 将实体放在独立存储，不能保证任意离线 clone 都完整。
参见 [Git LFS 机制](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-git-large-file-storage)。

### 7.2 哪些现有产物适合兜底

- 优先候选：复杂媒体/imagefs 依赖、GBE 客户端、PulseAudio；Proton 或 guest debugger
  只有在本轮 Mac 构建实际受阻时才加入例外。FEX/Turnip/D3D 是常修改组件，应优先可本机构建。
- **现有私有验证归档不能直接视为可公开入库。** imagefs 的完整来源归档未闭合；GBE
  provenance 明确完整依赖声明仍是发布前置项。公开仓库内提交二进制同样属于分发行为，
  不会因放在 Git 而免除相应许可要求。
- FEX/Mesa/DXVK 的宽松许可仍须保留对应 copyright/license；Wine、VKD3D、PulseAudio、
  GBE 和具体启用的 FFmpeg/codec 组合，须按各自 COPYING/构建配置提供所需对应源码、
  修改和构建材料。静态 LGPL 依赖另核对重链接材料，不能只附一份顶层 LICENSE。
- 私有仓库解决访问和存储范围，不自动解决来源或向接收者提供材料的问题。个人本机缓存
  与公开/向他人分发分开处理；本轮评估不触发发布。既有 release audit 继续有效。
- 系统/vendor 库由 Android 设备提供，不拷入仓库；游戏、存档、账号、APK、签名文件和
  私有 Foundation 不进入公共预编译仓。

### 7.3 预编译锁与更新契约（拟新增）

每个例外记录：`id`、版本、target triple/格式/API、源仓+commit、patch hashes、源码归档
hash、构建配方/参数、宿主工具链版本及 hash、license/notices、产物 byte size/SHA-256、
ELF Build ID 或 PE/PDB 身份、符号位置、直接及静态依赖、兼容组、使用原因、分发范围、
验证记录、`sourceRebuildStatus` 和 `releaseReady`。

Wine/FEX/D3D/sysroot 按经过验证的兼容组取用；不能混用 MSVC 和 MinGW 静态库，不能只按
文件名认定 ABI 相同。共享 CRT 对象/内存跨 DLL 的所有权及 C++ 异常尤其需要检查。
更新必须先验证新包，再原子更新 lock/catalog；保留旧锁以便回滚。旧设备存档和游戏文件不变。

## 8. 构建结构与实施顺序

以下入口和锁文件是**拟实现设计**，本轮没有创建可执行的迁移流程。

```text
Mac 宿主工具 + 固定 SDK/MinGW
  ├─ Mac 生成器（Wine tools / 协议和数据生成器）
  ├─ Android sysroot（源码输出 + 已声明预编译例外）
  ├─ NDK → Android ELF / JNI / unixlib
  └─ LLVM-MinGW + Wine tools → Windows PE / FEX / D3D / helper
          ↓
  组件 manifest + 符号 + 许可证 → runtime bundle + Pulse 产物
          ↓
  Mac Gradle / JavaSteam / Rust → 内部 APK → AYN 验收
```

拟统一入口 `tools/xrgame/build-macos.sh` 支持 `doctor / fetch / build / package / verify`，
以及单组件构建；基础 SDK、Windows toolchain、host generators、Android sysroot、PE 库
分别设置目录，避免 PATH/pkg-config 污染。工作目录用 gitignored `build/xrgame-macos/`，
也允许外部绝对路径。现有 Linux 参考目录继续是 `/data00/xrgame-native`，不修改其磁盘安排。

| 阶段 | 工作及产出 | 关闭条件 |
|---|---|---|
| **M0：固定输入** | 完整组件/动态依赖/静态依赖清单；工具链 lock、sysroot/预编译例外清单；doctor | 无隐式 Linux 工具；每个输入有状态和来源；fresh checkout 可准备输入 |
| **M1：Mac 出包闭环** | 先使用声明过的运行时/Pulse/GBE 预编译输入，在 Mac 重建 App/X server/native/XR/Rust/JavaSteam 并组装 APK | 无远端构建或手工残留目录依赖；hash/audit 通过；清楚标记 M，不能称全源码 |
| **M2：常改组件可重建** | Mac 编 FEX DLL/unixlib、Turnip/XCB、DXVK/VKD3D、Wine host tools；逐一替换并回归 | 本机修改能进入对应新产物；失败项具体登记，禁止静默取旧缓存 |
| **M3：Wine 与辅助完整链路** | Proton/ARM64X、ntsync、WineDbg、XR builtin/bootstrap、GBE loader；尽量完成 Pulse/gst-libav | 混合 ABI、显示、媒体、重启、双 debugger 通过；GBE 客户端和复杂 sysroot 可保留例外 |
| **S：按需要收敛例外** | GBE 完整客户端、81 项 runtime 依赖和 AAR native 源码重建 | 全部受管组件来源/配方/输出闭合；仍有未关闭例外就继续保留 S 未完成 |

执行优先级是先获得 M1 的可用结果，再根据要修改的组件推进 M2/M3；不因某个 codec 或
编译器升级卡住整个 Mac 出包。此次规格不承诺各阶段工时或编译速度。

实现时必须处理：Darwin host tag、GNU make/tar 与 BSD 工具差异、Python hash/readelf
替代、可执行位/符号链接、ZIP/TAR 时间戳、case-sensitive 路径碰撞、带空格路径、
`SOURCE_DATE_EPOCH`、宿主 code generator。为 host 和 target 分别指定编译器，设置
`PKG_CONFIG_LIBDIR`/sysroot；不能执行目标 conftest，也不能从默认搜索路径引入 macOS 库。
工具链升级涉及同进程 `libc++_shared.so` 和 ABI 时按兼容组验收，不能随便混装 NDK r29 库。

## 9. 验收与回退

### M 出口必须满足

1. 空构建目录，从受控源码 checkout、固定 SDK 和声明过的输入完成构建；不复用 Ubuntu
   build tree、手动复制的未登记 `.so` 或临时 Wine headers。所有运行的生成器可在 Mac 执行。
2. 记录实际 host OS/CPU、工具身份、版本、输入和例外。若 SDK 工具确实需要 Rosetta，显式
   记录；不能暗中引入 Linux VM，更不能将“在 Mac 启动 Docker”计为本出口。
3. ELF 检查架构、Build ID、NEEDED、RPATH、页对齐；PE 检查机器类型、导入/导出及 ARM64X
   metadata；保持当前 Android API 和 AYN 4 KiB 语义。host Mach-O 不得误装进 APK。
4. `prepare-runtime-bundle.py` 和 APK audit 通过，必备 Proton/FEX/D3D/Turnip/imagefs/GBE
   均有匹配 hash；runtime 缺失/篡改须失败或按既有修复机制恢复，不能静默替代。
5. 干净打包：先移除本次 flavor/type 的旧 APK 输出，再 assemble。第二个空构建目录复验
   输入/文件集/行为；如 SHA 不同须解释，不预设跨 OS 位级相同。缓存填充后离线重放通过。
6. AYN 上以 app 运行域验证 console/GDI/D3D11/D3D12；MHW 场景、原存档、Iceborne DLC、
   DX11↔DX12 重启和正常退出；MHR 媒体/进入场景；Hades II 显示；native/guest debugger
   attach、暂停、恢复、清理和既有受支持步进探针。保持当前限制，不擅自启用未验收断点策略。
7. 验收记录包括 APK SHA、loaded `.so` Build IDs、component manifest SHA、设备身份/boot、
   PID/时长，原始数据放仓外；失败和回滚证据保留。性能按同场景比较，不能把编译成功当成
   FPS、长玩或 Swan/XR 验收。

构建流程不得清空设备游戏、shader cache 或存档以制造“干净结果”。新运行时使用新 catalog
和独立验证安装范围；保留已验证 Linux 构建产物作为对照和回退，恢复 hash 后复测。

### S 出口附加条件

所有受管 runtime、静态依赖、生成器和官方 native AAR 对应源码能由 Mac 构建；基础 SDK、
字体/数据复制等边界单独列明。预编译例外表为空（除明确基础工具链输入），对应源码材料
完整且重放通过。源码分发门槛仍与“能在个人设备运行”分开，不能因为 M 完成就开放发布。

## 10. 关联实现与证据

- 当前主流程：[build-runtime.sh](../../tools/xrgame/build-runtime.sh)、[prepare-linux.sh](../../tools/xrgame/prepare-linux.sh)。
- 已有 Mac 路径：[native](../../tools/build-picoxr-native.sh)、[XR payload](../../tools/build-picoxr-xr-payload.sh)。
- 组件：[Wine](../../tools/xrgame/build-wine.sh)、[FEX](../../tools/xrgame/build-fex.sh)、[D3D](../../tools/xrgame/build-d3d.sh)、[Turnip](../../tools/xrgame/build-turnip.sh)、[XCB](../../tools/xrgame/build-xcb.sh)。
- 依赖：[imagefs 打包](../../tools/xrgame/package-imagefs.py)、[82 项源码锁](../../tools/xrgame/termux-sources.lock.json)、[APK 来源规则](../../tools/xrgame/apk-sources.json)。
- 当前缺口：[运行时构建记录](../validation/wp3-build-pipeline-20260926.md)、[阶段 gitlink 与实际产物区别](../validation/stage-20260928.md)、[随包运行库](../validation/bundled-runtime-20260927.md)。
- 本轮静态评估/实测：[macos-build-assessment-20260928.json](../validation/macos-build-assessment-20260928.json)。原始工具链和探针输出留在 gitignored `build/macos-spec-assessment/`。
