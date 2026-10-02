# MHR 开场黑屏：媒体插件缺失未被安装回执发现

## 现场与定位

2026-09-28 在 AYN Thor 重放用户报告的持续黑屏。首次连接时游戏和应用都已退出，
因此本记录的黑屏是随后受控启动所得，不是对用户先前进程的调用栈追溯。

- 原安装 APK：`aa002d66ee139b627811e9ee4f50ec61a74fef627ec08095196cfa5a0f694d69`。
- `.xrgame-base-sha256` 已匹配包内 imagefs，但清单中的
  `usr/lib/gstreamer-1.0/libgstdebug.so` 不存在。上轮迁移测试的同哈希备份仍在诊断目录。
  断连期间没有完整操作记录，不据此推断回执写入与插件缺失的先后。
- 清单 579 个文件中，缺失上述 1 个；另有 `libxcb-dri3.so`、`libxcb-present.so`
  与基础包不同，它们由固定 Turnip 组件覆盖和校验，是预期的所有权差异。
- 应用 PID `11052`，MHR PID `12245`，start ticks `137043476`（CLK_TCK=100）。
  游戏进程从创建到受控结束观察了 362.71 秒，最后仍是黑屏。HUD 显示约 60 FPS，
  但没有游戏内容；这不是有效游戏帧率验收。
- 媒体线程已建立：`typefind:sink`、`qtdemux0:sink`、两个 `multiqueue0:src`、
  八个 `av:h264:df*`。11.70 秒采样内这些线程 CPU ticks 均未增加。
  游戏的其他线程仍在工作；不能把整个进程描述为完全休眠。
- 同 UID 独立探针确认 `winegstreamer.so` 和 gst-libav 可加载，11 个必需元素中
  仅 `capssetter` 缺失。Wine `wg_parser.c` 创建视频输出链时要求此元素；创建失败
  立即返回 false。结合媒体线程停滞及修复对照，当前卡点在开场视频输出链建立，
  不是需要继续等待的着色器编译。
- 初次探针遗漏 Wine unixlib 的 `LD_LIBRARY_PATH`，误报 `ntdll.so` 不可加载；
  保留该失败输出，并使用正确路径重新验证。未把探针配置错误算作设备运行时缺库。

## 修改

`XrGameBaseImage.ensure` 不再仅凭 archive SHA 回执跳过检查：

1. 将从校验通过的 archive 提取的文件清单绑定到独立 SHA 回执；旧安装首次重验。
2. 启动前检查清单文件存在及内容哈希。Turnip 所有的两个 XCB 库仍由驱动安装路径
   校验，避免两个组件相互反复覆盖。
3. 缺失、损坏、旧 Media NDK 替身重新出现时，在没有运行中 Wine 进程的条件下，
   从包内验证过的 archive 修复；验证完成后原子更新回执。

沿用限定运行时目录的安装器，不修改游戏文件、prefix/home 或存档；没有清 shader
cache，也没有给 MHR 套用 MHW 的路径别名、bootstrap 或额外 Hook。

## 更新与验证

- `XrGameBaseImageTest` 6 项、`XrGameRuntimeFilesTest` 4 项通过。
  新回归覆盖：匹配回执但缺插件、同长度内容损坏、旧/更改的清单、Turnip 覆盖、
  已退役替身重新出现。已有测试覆盖保存目录保护、无效源及活动游戏修复门禁。
- `assemblePicoXrDebug` 从清空 APK 输出后构建通过；`audit-apk` 123 文件、0 错误。
- `adb install -r` 成功。新包自动恢复插件，哈希为
  `b09792c699f19408a2aec21e353397eea24da4421af43af999fc5e091ce5cacb`；
  没有手工把插件推回设备。媒体探针 11/11 元素通过。
- 新应用 PID `14246`，MHR PID `15401`，start ticks `137088486`。
  默认诊断环境下已显示启动画面、可关闭的 `MR-111-81` 提示、自动保存说明、
  动态 Sunbreak 标题、主菜单及原有三个存档槽。首次截图在发起启动约 70.6 秒时，
  因此它只是该时刻已有画面的证据，不是首次出图耗时测量。
- 原角色加载进入曙光村庄，菜单可操作。选择“保存并结束游戏”前已观察同一进程
  952.75 秒（包含菜单停留和工具操作时间，不是连续游玩时长）；随后保存完成，
  游戏、loader 和 wineserver 均退出，只余 Android 应用。

## 重启追加发现：Steam Cloud 上传异常

正常保存退出后的重启先显示云存档冲突：本地为本轮 17:30 的保存，远端为前一日
22:41 的版本。选择保留刚保存的本地版本后，17:34:44 应用 PID `14246` 因未捕获
`ConnectException` 退出。此轮未产生新的 MHR 进程，不算游戏重启成功。

`/::1:9` 是出站限制器的拒绝代理。核对相邻日志和 `beginFileUpload` 调用后，
实际目标是 Steam Cloud 经 CM 返回的
`steamcloud-tyo.s3.dualstack.ap-northeast-1.amazonaws.com`。它不在先前仅有香港/新加坡
存储的限定名单中；不是 GameNative 上游接口。原始签名 URL 和请求头仅在私有日志中。

- 为该准确主机加入 CM 回复限定的 HTTPS 客户端路由，仍禁止跨 origin 重定向，
  不放开通用 `amazonaws.com`，不加入全局域名白名单。
- picoXr 上传块遇到 `IOException` 时返回失败，沿原逻辑向 Steam 提交
  `transferSucceeded=false` / 失败 batch；保留本地文件，不再将连接失败抛到启动协程。
  HTTP 响应及时关闭，正常协程取消继续传播。
- 回归覆盖东京主机精确匹配、第三方/伪装域名/HTTP/非标准端口拒绝，连接失败、
  HTTP 成败与响应关闭，以及取消传播。
- 尝试启动 Litep ring 时，上一轮退出已停止 DebugBusService；该失败输出保留。
  本次没有取得可用于计算启动分段耗时的 profiler trace，不能引用不存在的计时结果。
- 加入云同步修复后，5 个相关测试类共 58 项通过（含 SteamAutoCloud 38 项）。
  再次从干净 APK 输出构建通过，包审计 123 文件、0 错误，覆盖安装成功。
  该轮 APK SHA-256：`3df0766811ed74a745957e04df55e554cae58358ff4916fc4ff4fe6680473161`；
  catalog、imagefs 及下列 ELF Build IDs 不变。前述 `c39ec...` 为媒体修复中间包。
- 应用 PID `27709` / MHR PID `29125` 再次越过媒体黑屏并进入动态标题。
  start ticks `137273340`，记录时已观察 282.38 秒。
  `libgstdebug.so` 和基础回执的 mtime 仍为首次修复时刻，没有再次提取基础包。
  该次 CM 返回其他已允许的存储节点，东京路由由精确 origin 单元测试覆盖；
  不能把此次设备成功算成东京上传实测通过。
- 同轮发现一次 `commitFileUpload=false`，另一文件为 true。原逻辑仅检查 HTTP
  传输成功，忽略 Steam 提交结果，可能错误前移同步缓存。追加修复要求 picoXr
  同时通过传输和提交才计入上传数、更新 change number/cache；拒绝则返回
  `UpdateFail`，本地存档保留。针对拒绝提交的集成测试验证零上传数、失败状态、
  旧缓存不前移及本地字节未变。该轮测试总计 59 项通过；初次测试编译缺 import
  的失败日志亦保留。云端拒绝的具体原因仍未定因，不宣称云存档完整验收。
- 包含提交结果校验的最终 APK：
  `02a6a78418336e1f6cab182a0b40b806986c08967887db5a1fcf7397f4cbe598`。
  构建、123 文件审计及覆盖安装均成功；catalog、imagefs 和 ELF Build IDs 保持不变。
- 最终包重放：应用 PID `31246`，MHR PID `32425`，start ticks `137320167`。
  关闭 `MR-111-81` 和自动保存说明后进入 Sunbreak 标题页；记录时进程已运行
  477.89 秒，包含提示等待和工具操作，不能作为启动耗时。该应用 PID 的日志中
  `FATAL EXCEPTION` 为 0，基础回执和插件 mtime 未变，没有再次提取基础包。
  最终停留标题页供用户继续操作；没有遗留 DebugBusService 或调试器会话。

## 身份与证据

- AYN Thor，Android 13 / API 33，4 KiB；app targetSdk 36。
- Build：`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`。
- Boot：`8bb14501-5800-4b9b-a9ab-7173603812f7`。
- 最终安装 APK SHA-256：`02a6a78418336e1f6cab182a0b40b806986c08967887db5a1fcf7397f4cbe598`。
- Catalog SHA-256：`e8619cda78b93fbde4e3f509157476f08db3ecb04749215e1fa4eb885984f353`。
- Imagefs archive SHA-256：`7f0901ab3ca2dd0a61dba63baeed4da38f18391d5d87741bc5e7f7ce59fd1092`。
- Imagefs 文件清单 SHA-256：`484b060cec7732f2cae01dcfafc238e5c92ce9eaf600b0cecc75b6ce4e38907b`。
- App ELF Build IDs：
  - `libevshim.so`：`5d357cd7a9756016f8b6eb3d1bfc9cc23edb262d`
  - `libwinlator.so`：`e120a35c35b69960ed4fe4c8993045bddffe4c1b`
  - `libvulkan_renderer.so`：`36421bccc5e74ef19b0ff30b47b21ea071507eab`
  - `libgndownload.so`：`674e470eca24f48e67e09f5329fc7e7a58b98469`
  - `libxrgame_debugbus.so`：`30a9c69fe7e2e2c13006718a8591f3d8619cdc23`
- 运行时版本保持[前一轮记录](mhr-startup-20260927.md)中的 Proton/FEX/VKD3D/Turnip。
  本次不涉及更换游戏文件、Wine 或驱动。
- 原始日志、线程采样、前后媒体探针、截图、测试与 APK 审计留在仓库外
  `xrgame-native-evidence/mhr/20260928/`，含失败与中间截图。账号/角色信息及 APK 不入仓。

本次没有建立 LLDB/guest debugger session。在线连接错误和早期偶发 hard-error
仍不能由此次媒体修复关闭；本记录也不构成长玩或性能验收。
