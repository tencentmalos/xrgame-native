# Monster Hunter Rise 启动退出（2026-09-27）

状态：原始闪退、视频初始化异常和缺插件黑屏均已复现。补齐媒体依赖后，
AYN 已播放 RE ENGINE 动画、进入标题菜单并读取原存档进入曙光村庄。新包已覆盖
安装，但随后 ADB 断开，包内自动升级与重启验证尚未完成。早期的间歇性
`NtRaiseHardError` 仍未定因，在线连接错误 `MR-111-81` 也未解决。

2026-09-28 续验：设备存在匹配回执但缺 `libgstdebug.so` 的状态，再次复现开场黑屏。
新增启动前完整性检查后，新包已自动恢复插件并越过黑屏；详见
[媒体修复与设备重放](mhr-black-screen-20260928.md)。以下保留 09-27 的历史边界。

## 身份

- Steam App ID `1446780`，原 WP1 安装目录 `MonsterHunterRise`，34.49 GiB。
- AYN Thor / Android 13 / API 33 / 4 KiB，app targetSdk 36。
- Build：`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`。
- Boot：`8bb14501-5800-4b9b-a9ab-7173603812f7`。
- APK SHA-256：`c3c220cd9d4999b031324055a54c63dbdd71fef2801a941be3a83bfae3250055`。
- Catalog SHA-256：`f4f34d00c5e207b65b583f4be1bedcd85a4ffffac6bd7e6c36a9443339c2159f`。
- ELF Build IDs：`libevshim.so` = `5d357cd7a9756016f8b6eb3d1bfc9cc23edb262d`；
  `libwinlator.so` = `e120a35c35b69960ed4fe4c8993045bddffe4c1b`；
  `libvulkan_renderer.so` = `36421bccc5e74ef19b0ff30b47b21ea071507eab`；
  `libgndownload.so` = `674e470eca24f48e67e09f5329fc7e7a58b98469`；
  `libxrgame_debugbus.so` = `7182d65276c0f44d49b55fa761b4ca5ceef1b19a`。
- 游戏 EXE SHA-256：`29fa23a9ce24613a9b05ce84f7ebfa3262aaf5053b377b43fd8dfc09071aa7fc`。
- Android PID `30567`。原始退出 MHR PID `29460`；图形报错复现 PID `31788`；
  进入编译的 PID `6828`，start ticks `130037626`，设备 CLK_TCK=100。
- 使用 Proton 11 / FEX xrg5 / VKD3D `212991fc2c266bc` / Turnip xrg5，
  初始 Shader Model 对照没有替换运行时；后续媒体库补入记录见下文。
  未改写游戏 EXE/DLL 或存档内容；游戏自身正常读写不在此限制内。

## 现场与对照

1. 用户报告启动后直接返回选游戏界面。21:24:59 启动，21:25:27 返回；
   loader 记录约 27.402 秒时子进程以 `0xdeadc0de` 退出。Android 主进程未崩溃。
2. 临时启用 Wine 异常、模块和进程日志，21:29 的复现显示
   `renderDeviceDX12.cpp:12878`、`Your GPU was not supported` 对话框。
   SteamAPI 已成功加载 prefix 内的 `steamclient64.dll`，并创建 VKD3D device。
   不是此前 MHW 的缺失 Steam 客户端 DLL。
3. 同轮 VKD3D 日志先报告 `Enabling support for SM 6.6`，随后报告
   `Overriding supported shader model: 6_0`。后者来自 `Container.DEFAULT_ENV_VARS`，
   被保存到本游戏 `.container`。它人为压低实际驱动报告的能力。
4. 删除该 override 后，两轮仍在更早阶段调用 `NtRaiseHardError(0x50000018,4,3,...)`，
   然后 `NtTerminateProcess(...,0xdeadc0de)`。这两轮不能算修复成功；原始日志保留。
   一轮同时减少了日志通道，随后保持原通道重试也出现退出，未把日志差异当作修复。
5. 后续相同自动 Shader Model 配置的 PID `6828` 已显示 MHR/Sunbreak 画面和
   `Compiling shaders...`，进度持续前进。实际环境中没有 `VKD3D_SHADER_MODEL`，
   `WINEDEBUG=warn+seh,+process`；没有 `XRGAME_BOOTSTRAP_*`，loader 注入目录为空。
   拟进行的额外 bootstrap 对照在检查到游戏仍运行时提前拒绝，未写入任何配置。

当前仍从 `A:\MonsterHunterRise.exe` 启动。没有套用 MHW 的目录别名或额外 hook，
不能从 MHW 的历史故障直接推断 MHR 也需要它们。着色器缓存由游戏自然生成，未清缓存。
HUD 在编译画面约 30 FPS，不作为实际游戏性能验收。

## 修正和验证边界

- 设备侧删除本游戏容器中的 `VKD3D_SHADER_MODEL=6_0`，保留其他用户配置。
- 源码 `Container.DEFAULT_ENV_VARS` 对 picoXr 新默认值不再设置此 override，
  由固定版本的 VKD3D/驱动检测 Shader Model。其余 upstream flavors 保留原默认。
  现有自定义容器和已持久化的全局配置不会被批量改写。
- `:app:compilePicoXrDebugJavaWithJavac` 通过（1m 1s），`git diff --check` 通过。
  此处指初始 Shader Model 对照；后续媒体修复包的构建与安装见末节。
- native-debugger 的只读检查先遇到已退出 PID，随后在仍存活 PID 上因 Wine 命令行
  与 Android package 名称不一致被 provider 拒绝。未建立 LLDB session、未设置断点、
  未暂停目标，因此也没有取得所计划的 `NtRaiseHardError` 参数或调用栈。
- 失败与成功轮的日志、loader 输出、配置快照、截图及编译记录保存在仓库外
  `xrgame-native-evidence/mhr/20260927/`。原始日志含账号/云信息，不进入公共仓库。

后续已观察标题菜单与原存档场景；仍待验证关闭诊断日志后的新包重启。
间歇性 hard-error 仍需在可用的 guest 调试入口捕获参数，不能因单次进入画面将其关闭。

## 媒体路径根因与迭代

- 21:47:40 和 21:57:12：`MFCreateSourceReaderFromByteStream` 失败后，
  游戏在 `MonsterHunterRise.exe + 0x473469b` 触发访问异常。后一轮完整 MF 日志
  先记录 `media_source_create` 返回 `0x8007000e`。源码表明解析器创建失败也会
  使用该错误码，不能据此认定物理内存耗尽。
- 应用 UID 下的加载探针确认 `winegstreamer.so` 因缺少 `libEGL.so.1` 无法加载。
  原打包器把“Wine 可选 dlopen 库”和“其他库的必需 DT_NEEDED”一起排除了。
  `WINE_GST_NO_GL=1` 不会消除 ELF 的链接依赖。
- 原 imagefs 还遗漏整个 GStreamer 插件目录。补入基础插件后，22:06:54 的
  PID `21087` 正确识别 1920×1080 H.264 与 48 kHz 六声道 AAC，但报缺 AAC
  解码器；随后转入没有配置替代媒体文件的 Proton converter 并退出。
- 从已 fork 的 GStreamer 1.28.0 构建 gst-libav。完整插件会额外引入
  libavfilter/libplacebo，而 snapshot 的 libplacebo 要求 NDK r27 C++ 库没有的
  `__from_chars_floating_point<float>`。最终构建省去可选的 deinterlace/comparison
  FFmpeg filters，保留解码、编码和封装处理；Wine 使用 GStreamer good 的
  `deinterlace` 元素。没有替换全局 C++ 或 Vulkan 库；试验添加的 libavfilter
  依赖按哈希核实后清除，失败构建和探针输出保留。
- 原包中的 Termux `libmediandk.so` 替身遮住 Android 系统库，造成 bionic
  `libhwui.so` version-needed 错误。将 Media NDK 列为系统依赖后，实际设备上的
  `winegstreamer.so` 和 gst-libav 均可加载，H.264/AAC factory 均可找到。
- PID `26518` 的下一轮不再异常退出，但缺 `capssetter`，视频输出链未完成，
  画面持续黑屏。补齐 `debug`、`deinterlace`、`videofilter` 插件后，必须真正
  结束该旧进程再启动；仅发 SIGTERM/深链未建立新进程的轮次不算重启验证。
- 新 PID `31933` 已播放 RE ENGINE 动画。在线连接提示 `MR-111-81` 可关闭，
  随后显示自动保存说明；该结果只证明越过媒体卡点，未验证在线联机。
- `probe-media-runtime.c` 在应用 UID 下检查两库和 11 个必需元素，全数通过。
  新 APK 增加 base image 升级：仅更新清单内运行时文件，按旧哈希移除已知
  Media NDK 替身，不重置 prefix/home/save。7 项升级与运行时文件测试通过。

## 村庄与阶段包状态

- 手工媒体依赖修正后的同一 PID `31933`（start ticks `130312773`）已进入动态
  Sunbreak 标题、主菜单、原有角色列表，并加载原角色进入曙光村庄。菜单输入有效；
  在线连接错误可关闭。选择游戏内“结束游戏”后，游戏与 Wine 进程均退出。
  本轮未做固定时长的长玩验收，不能据截图 HUD 给出稳定帧率。
- 随后的候选 APK SHA-256 为
  `aa002d66ee139b627811e9ee4f50ec61a74fef627ec08095196cfa5a0f694d69`，
  catalog 为 `e8619cda78b93fbde4e3f509157476f08db3ecb04749215e1fa4eb885984f353`，
  imagefs archive 为 `7f0901ab3ca2dd0a61dba63baeed4da38f18391d5d87741bc5e7f7ce59fd1092`。
  DebugBus Build ID 变为 `0f072a97496811fb0963369b588b1197665d635d`；上述其他四个
  app ELF Build ID 不变。包审计通过，123 项检查、0 错误；`adb install -r` 返回成功。
- 为检查真实升级行为，在所有游戏/Wine 进程退出后，恢复了已知旧版 Media NDK
  替身，并按哈希核验后移出 `libgstdebug.so`；诊断环境变量也恢复默认，保留删除
  SM 6.0 限制的配置。随后发起新包启动，但 ADB 断开，未取得新 PID、迁移回执或
  新包画面，不能将前一轮手工补库的成功算成自动升级通过。
- 失败、手工修正、角色/村庄截图、正常退出检查和升级 fixture 的原始记录继续留在
  仓库外。下一次连接设备首先核查 `.xrgame-base-sha256`、旧替身移除和媒体元素探针，
  再做无诊断配置的启动重放。阶段源码提交不替代这项设备验证。

## 2026-09-29 场景呈现与 UI 后续

同进程同步/异步/同步对照确认 MHR 也受 X 请求线程 fence 等待影响。已加入每游戏的
“编辑容器 → 图形 → 异步呈现（实验性）”选项，仍默认关闭；测量、版本及限制见
[异步呈现验证](mhr-present-async-20260929.md)。此优化不替代本页的启动媒体修复。
