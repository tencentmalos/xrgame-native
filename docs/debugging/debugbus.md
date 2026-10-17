# Foundation DebugBus

用户于 2026-09-27 授权引入私有 Foundation 子仓。正式构建依赖位于根目录 `foundation/`，
来源 `tencentmalos/foundation` 的共享 `main`，初始 gitlink 为
`d487242023e6899f680717d263cf35c7c1f9e6c2`，与 shadPS4 当日已提交版本一致。
不复制 shadPS4 的本地修改，也不初始化 Foundation 的 `reference/` 研究仓库。
2026-09-28 的阶段 gitlink 为 `c1d339c3a9b231ad23d02e4fd982e3e7120f7ef5`：
独立分支 `feature/malos/xrgame-litep-startup` 已 rebase 到共享 `main` 的
`9b29d357` 并推送，加入启动阶段 profiler ring 与 DebugBus 命令。

```sh
git submodule update --init foundation
```

仅内部 `picoXrDebug` 构建 `libxrgame_debugbus.so` 和 `DebugBusService`。CMake 单独加入
`foundation/modules/debugbus`；后续 Litep 接入还单独链接 profiler ring 与其 SDK/LZ4
依赖。没有接入音频、手柄或整个引擎。Foundation 当前未声明顶层公开分发许可证；此接入
用于用户授权的本地研究，不能作为公开发布 Foundation 源码或二进制的依据。公共 CI 若要
构建内部调试包，需要单独配置只读私仓访问，以及既有的完整运行时 bundle；本次未配置 CI 凭据。

## 使用

先正常打开 XRGame Native，再启动诊断服务。启动服务本身不会启动游戏或更改配置；
只有下文「沉浸式自动化」里的命令会改变状态：

```sh
python3 tools/xrgame/debugbus.py --serial <device> --start bridge
python3 tools/xrgame/debugbus.py --serial <device> help
python3 tools/xrgame/debugbus.py --serial <device> status
python3 tools/xrgame/debugbus.py --serial <device> runtime
python3 tools/xrgame/debugbus.py --serial <device> processes
python3 tools/xrgame/debugbus.py --serial <device> modules
python3 tools/xrgame/debugbus.py --serial <device> modules libxrgame_debugbus.so
python3 tools/xrgame/debugbus.py --serial <device> present
python3 tools/xrgame/debugbus.py --serial <device> present trace 120
python3 tools/xrgame/debugbus.py --serial <device> present trace 0
python3 tools/xrgame/debugbus.py --serial <device> present async_copy 1
python3 tools/xrgame/debugbus.py --serial <device> present async_copy 0
python3 tools/xrgame/debugbus.py --serial <device> present copy_pipeline 1
python3 tools/xrgame/debugbus.py --serial <device> present copy_pipeline 0
python3 tools/xrgame/debugbus.py --serial <device> --stop
```

原始入口是 `adb -s <device> shell dumpsys activity service
com.tencentmalos.xrgamenative/app.gamenative.xrgame.DebugBusService <command>`。
服务由 `android.permission.DUMP` 保护，Android dumpsys 本身要求 shell/系统权限；不监听 TCP，
无需端口转发。进程重启后需再次 `--start`；服务不做 sticky 后台重启。

| 命令 | 返回内容与边界 |
| --- | --- |
| `bridge` | Foundation 固定提交、宿主 PID、transport、scope |
| `status` | 宿主 PID/UID、系统与目标 API、设备 build/boot id、服务起始时刻、活跃 Steam App ID、X server 是否存在 |
| `linux_steam status/open/start/stop/check/trace` | 官方 Linux Steam 独立客户端：打开页面、启动/停止前台会话，或异步检查官方 Bionic 客户端能否连接。`check` 的 0=会话已登录、5=无 IPC pipe、6=客户端未运行或不能连接全局 user、7=尚未登录/离线、8=账号不匹配、9=超时；打开页面不能证明在线或游戏联机通过。`trace` 另返回最近 64 条 X11 请求记录，每项为 `[connection, sequence, opcode, data, payloadBytes, complete]`；不含正文、输入文本、账号或票据，游戏服务器默认不记录 |
| `runtime` | APK catalog SHA、默认组件版本、安装回执是否对应 catalog；**不重新哈希大文件，不证明已装文件完整或该进程实际加载了全部默认项** |
| `processes` | 当前 UID 可见进程的 PID/PPID、comm、state、start-time ticks；进程退出竞争会计入 skipped，超量/超时标 truncated |
| `modules [exact-basename]` | 当前 Android host 已加载 ELF 的名称、基址、GNU Build ID；超过 256 个时显式标记 truncated，可按完整文件名查询；不是 Wine PE/FEX guest 模块表 |
| `present` | 当前 host Present 的待归还队列、CPU pacing 队列、限帧、trace 余量；近似并发快照，不获取 GPU 锁 |
| `present trace N` | 当前会话最多记录 N 帧（0–3600），0 停止；仍通过 `XRGamePresentTrace`/原生 present trace 输出 logcat |
| `present async_copy 0/1` | 当前会话的 AHB GPU copy 对照开关，初始关闭；在已接收任务归还后切换。查看 `asyncCopyRequested/Active`、`outstandingCopies` 和完成/跳过/失败计数 |
| `present copy_pipeline 0/1` | 实验性两阶段 copy，默认关闭、仅当前会话；需 `asyncCopyActive=true`。同队列的 GPU barrier 保证 copy→draw 顺序，独立线程等待 copy fence 后才通知 Complete/Idle。查看 `copyPipelineRequested/Active`；模式切换等待已接收任务归还 |
| `api_capture [status\|start\|stop] [container]` | GFXReconstruct 录制（仅带 `assets/xrgame-gfxr` 的 debug 包）：`start` 在 `files/imagefs/xrgame-captures/<容器>/` 创建触发文件，下一帧开始截取 `XRGAME_API_CAPTURE_FRAMES` 帧；`stop` 删除触发文件；`status` 列出各容器的模式与 `.gfxr` 文件。只反映触发文件状态，不代表录制已经完成；容器缺省为当前 Steam 游戏。见 [API 录制规格](../specs/xrgame-native-api-replay-v1.md) |
| `instrumentation [off\|coarse\|detail]` | host Litep 桩点层级；默认 coarse，仍需显式开始采集；off 不结束已开始的采集会话 |
| `profiler_ring [status\|start\|stop\|dump N]` | 每线程 1 MiB 的有界 ring；dump 0 允许零帧启动，等待准确 dump_id 完成后收集 |
| `profiler_capture [status\|stop\|file MiB seconds]` | 有界 Streaming 文件，完成后恢复此前 ring 状态；保留 PROF 与 sidecar |

Litep 的主流程覆盖、开销控制、跨线程/截断语义和采集示例见 [Litep 桩点说明](litep.md)。

### 沉浸式自动化（2026-10-09）

这组命令用于自动化测试：进游戏、按键、开关快捷菜单和 HUD。

```sh
python3 tools/xrgame/debugbus.py --serial <device> --start launch 546560
python3 tools/xrgame/debugbus.py --serial <device> launch status
python3 tools/xrgame/debugbus.py --serial <device> input btn a
python3 tools/xrgame/debugbus.py --serial <device> input btn menu 800
python3 tools/xrgame/debugbus.py --serial <device> input axis ly -1 300
python3 tools/xrgame/debugbus.py --serial <device> quickmenu open
python3 tools/xrgame/debugbus.py --serial <device> quickmenu nav down
python3 tools/xrgame/debugbus.py --serial <device> vr_hud on
python3 tools/xrgame/debugbus.py --serial <device> launch 546560 load=s0/autosave
python3 tools/xrgame/debugbus.py --serial <device> game_state
python3 tools/xrgame/debugbus.py --serial <device> input axis rt 1 300
python3 tools/xrgame/debugbus.py --serial <device> vr_oc full_copy=1
python3 tools/xrgame/debugbus.py --serial <device> quickmenu exit
```

| 命令 | 作用与边界 |
| --- | --- |
| `launch <steamAppId>` / `launch status` | 与点「开始游戏」走同一路径：向 `MainActivity` 发 `app.gamenative.LAUNCH_GAME`（Int `app_id`，`game_source=STEAM`）。已有游戏、沉浸式会话或 X server 时返回 `game_running`。Android 只允许应用有可见窗口时由服务拉起 Activity，否则请求会被系统静默拦下，可改用 `adb shell am start -a app.gamenative.LAUNCH_GAME -n com.tencentmalos.xrgamenative/app.gamenative.MainActivity --ei app_id <id> --es game_source STEAM`。登录、未安装、云存档冲突等对话框仍需人工处理。`status` 返回最近请求、活跃 App ID 和沉浸式会话状态 |
| `launch <steamAppId> load=<存档> [trigger=auto\|off]` | 只对这一次启动追加 `+load <存档>`（Source 2，如 Alyx 的 `s0/autosave`），不经主菜单直接读档；不改容器的启动参数，只在下次启动该游戏时生效一次。存档名为 `名字` 或 `目录/名字`（字母、数字、下划线）。`status` 的 `pendingArguments` 显示尚未用掉的参数。默认 `trigger=auto`（2026-10-15）：游戏读完档发出提示（Alyx 的 `hlvr/interstitials` `show_message`，即「按下 Trigger 以开始」）时，约 1 s 后自动扣一次右扳机（同 `input axis rt 1 600`；戴着头显时 300 ms 曾连续 3 次没被游戏收到），提示还在就 2 s 后再扣，最多 3 次；logcat 记为 `DebugBus automatic trigger`。只对用掉这组参数的那次启动生效，之后任何不带参数的启动（包括用户手动启动）都会清掉；`trigger=off` 留给人来按。`status` 的 `autoTrigger` 显示当前启动是否开着 |
| `game_state` | 从运行中游戏的 Source 2 控制台日志（`-condebug`，Alyx 为 `game/hlvr/console.log`）判断是否已进场景：`hostState`/`target`（如 `Restoring Save`/`s0/autosave`、`Loading`/`startup` 即主菜单）、尚在加载的 `pendingState`、`paused`、`inScene`。只返回状态名、存档/地图名和时间戳，不返回日志原文（含玩家名）。日志最后写入早于本次沉浸式会话时 `current=false`，`inScene` 恒为 false。Alyx 每次读档约 3 s 后自动暂停，扣扳机（`input axis rt 1 300`）后继续；测量前应确认 `inScene` 持续为 true |
| `vr_oc [full_copy=0\|1]` | OpenComposite 同局 A/B 开关：`full_copy=1` 在当前游戏容器的 `C:\gamenative-xr\` 下创建 `opencomposite-full-copy`，恢复整张纹理拷贝；`0` 删除。DLL 每秒检查一次。测完务必设回 0 |
| `input btn <名>[_<名>…] [ms]` | 按住手柄键，默认 150 ms，最长 10 s。键名 `a b x y lb rb back start l3 r3 menu`，组合用 `_` 连接。`menu` 不足 600 ms 相当于给游戏按 Start，600 ms 及以上切换快捷菜单 |
| `input axis <轴> <-1..1> [ms]` | 覆盖控制器快照里的一个轴，默认 300 ms：`lx ly rx ry`（摇杆；快捷菜单把 `ly>0` 当作向下，游戏原样收到这个值作为 XInput Y）、`lt rt`（扳机）、`lg rg`（握把，>0.5 同时算 LB/RB） |
| `input release` / `input status` | 立即释放 / 查看当前覆盖和剩余时间 |
| `quickmenu [status\|open\|close\|toggle]` | 在主线程开关沉浸式快捷菜单（最多等 1 秒），返回菜单、暂停、指针模式、立体（Windows VR）、直通渲染和 overlay 尺寸 |
| `quickmenu exit` | 打开快捷菜单（如未打开），直接执行「退出」项本身的动作，游戏按菜单退出的原路径结束（退出云同步、OpenComposite DLL 还原）。立即返回 `exitRequested`，约 0.5 s 后执行；用 `launch status` 确认会话已结束。比连续 `nav down` 再按 A 可靠。2026-10-14 前它合成确认键，按键会送到持有焦点的 Android View（游戏中是 TouchpadView），两次落空、菜单停在暂停的游戏上 |
| `quickmenu nav <up\|down\|left\|right\|ok\|back\|next\|prev>` | 用 `input` 发一次菜单导航：左摇杆、A、B、RB、LB |
| `vr_hud [status\|on\|off]` | 头显性能 HUD（见下）。`on/off` 同时改状态层开关（`PrefManager.xrgameStatusLayer`，即启动面板的「状态层」，默认开；游戏画面里的 2D 性能 HUD 仍由 `PrefManager.showFps` 控制）；`status` 返回是否已创建图层、重绘/复用次数、最近与最长重绘耗时、当前 FPS 和面板位置，以及面板上 10 个格子的当前内容（`cells`，Foundation `StatusCells`）和它们的新鲜度（`cellsAgeMs`），供 PC 旁观窗口的跟随面板显示 |
| `guest_debugger start target=<pid>,<startTicks> wine=<winePid> dbg=<sha16>,<port>[,<fcsd>]` / `status` / `stop` | 从 app 进程启动 x64 WineDbg（`--gdb --no-start`），使它与游戏同在 `untrusted_app` 域；经 `run-as` 启动时是 `runas_app`，Swan 上拿不到 wineserver 的 fd。先按 UID 和 start ticks 核对目标，环境取自游戏进程（去掉继承的 `WINESERVERSOCKET`），可执行文件必须已在前缀 `C:\xrgame-debugger\xrgame-winedbg-<sha16>.exe` 且 SHA 前缀一致。标志：`f` 放行首次异常，`c` 记录挂起/恢复，`s` 软件断点（实验性，fixture 与 Swan 上的 Alyx 已验证），`d` 开 `+winedbg`。输出写 `files/xrgame-debugger/winedbg.log`，回复里只有状态和路径名。`stop` 向这个子进程发 SIGKILL（Android 的 `destroyForcibly` 只发 SIGTERM，WineDbg 不理会），kill-on-exit 已关，游戏继续运行。通常由 `tools/xrgame/run-guest-debugger.py` 调用。host 调试器停住 app 时 DebugBus 无法响应，此时用 helper 的 `--launcher run-as-app-domain`（需 root 与 patched run-as） |

`input` 在原生 `syncControllerInputs` 中与真实手柄合并，所以平面游戏的 XInput、快捷菜单导航、
指针模式双击和 Windows VR（OpenXR action）都会收到。只在沉浸式 XR 会话运行时生效，到期自动释放。

**无人值守进场景（2026-10-10，Swan + Alyx 验证）**：GPU 测量必须在游戏场景里做。
1. 先 `am start` 打开 SteamPSP，再 `--start`，然后 `launch 546560 load=s0/autosave`。
2. 轮询 `game_state`，直到 `current=true` 且 `hostState=Restoring Save`。
3. 读档后的「按下 Trigger」提示由 `trigger=auto`（默认）自动应答；若仍是 `paused=true`，再手动发 `input axis rt 1 300`。
4. 等 `inScene` 持续为 true 后开始测量。
5. 结束时用 `quickmenu exit`。

设备无网络时 Steam 登录不上，启动和退出都会跳过云同步，下次联网启动时再同步。

**头显性能 HUD**：仿 shadPS4 的 XR 状态面板。独立 ImGui context，由 Foundation 的
`XrImguiVulkanLayer` 画进单独的 OpenXR quad 层。面板固定在 LOCAL 空间，不跟随头部：

- 每次打开 HUD（以及 LOCAL 重新居中后），取当时头部的偏航方向放置面板。
- 立体模式下（2026-10-13 起）：
  - 面板中心距头部 1.0 m，水平居中，大小 0.8×0.08 m；下沿比平视时的视野上沿高 4°（按运行时报告的向上视场计算，Swan 为 43°，面板中心仰角 49.3°；拿不到视场时为 35°），抬头可见。日志 `XR eye FOV` 和 `Performance HUD placed … deg up` 记录实际值。
  - quad 层总画在场景之上；放近是为了让场景物体很少挡在它前面，避免辐辏冲突。
  - 面板保持竖直，平行于偏航方向的正前平面。
- 2D 大屏模式：
  - 面板在屏幕左上角上方，视角大小与立体模式相同，下沿与屏幕上沿相隔 0.6 个面板高。
  - 平面屏时与屏幕平行。
  - 曲面屏时沿圆弧左对齐，贴在圆柱面上朝向圆心；高度按面板两端算，保证两端也不落到圆弧上沿以下，避免与曲面穿插。
- `status` 的 `anchorLocal` 是面板中心在 LOCAL 中的坐标。
内容为游戏 FPS（只计带来新游戏画面的投影帧）、XR 帧率/显示刷新率、CPU、GPU、内存、电池、
CPU/GPU 温度、游戏眼图尺寸与重建方式。设备指标来自 Foundation `DeviceMetricsSampler`，
读不到的项显示 `--`。每秒最多重绘 4 次，GPU 未完成时复用上一张图，不等待 fence。
只在 Vulkan 合成后端提供；GLES 后端不显示。开关跟随快捷菜单的「性能 HUD」。

Windows VR 调参命令 `vr_tuning`、`vr_upscale`、`vr_grip` 的参数见 `help` 输出，
验证记录见 [Swan XR 合成](../validation/swan-xr-composite-20261009.md)。`vr_upscale` 同样作用于
2D 大屏的重建（滤镜、锐度、注视点）。`eye=` 是 VR 游戏的 XR 输出分辨率档位（shadPS4 同名设置：
推荐 / 推荐到上限的中点 / 上限，`XRGAME_XR_EYE_BUFFER`），运行中可改；各档单眼尺寸来自上一次
XR 会话记录的眼视图，启动面板据此显示。2D 大屏是交给 XR 合成器的 quad 图层，它能显示的上限是
大屏在眼图上覆盖的像素：推荐眼尺寸约为 1:1，眼尺寸上限为有效上限（1.5 m 处 80 英寸约 1402 到
2250 像素宽）。大屏图层按同一档位取 Wine 屏幕与该档覆盖宽度中的较大者，且不超过有效上限，在会话
开始时确定；Wine 屏幕小于图层时才从 Wine 屏幕重建到图层。状态层分两栏显示：游戏图像
（大屏为 WINE SCREEN，VR 为 GAME EYE）与 XR 输出（大屏为 XR LAYER，VR 为 EYE BUFFER）及滤镜。

`fg=` 是 2D 大屏的插帧（Foundation `lossless_fg`，`XRGAME_XR_FRAMEGEN`），值为输出相对游戏帧率的
最大倍数，实际倍数按游戏帧间隔与头显刷新周期取整；`flow=` 是光流分辨率百分比
（`XRGAME_XR_FRAMEGEN_FLOW`，默认 50）。X server 把场景渲染进 4 块 AHB 组成的环
（`xr_frame_exchange.h`），fence 完成后发布最新帧；渲染器不等会话，会话只读已完成且自己占用的
slot。插帧在 Wine 屏幕尺寸上直接采样这些 slot（真实帧不拷贝），生成帧与真实帧走同一次重建
（SGSR/FSR1），所以插帧在超分之前。只在游戏内容更新时推进插帧历史，仅光标变化的帧不算。状态层
第一栏开启插帧后显示为 `GAME / FG Nx`：游戏帧率 / 大屏每秒显示的图像数；logcat 每 10 秒记一行
`frame generation Nx, game … fps, shown … fps`。

`async_copy` 首轮只覆盖无 wait/idle X fence、无偏移、未启用 Present 限帧的 AHB copy；
直接采样及其他路径不纳入这次优化。picoXr 可在游戏详情 → 编辑容器 → 图形中切换
**异步呈现（实验性）**，保存并重启游戏后生效；默认关闭。需要 Vulkan、DRI3，
并关闭快捷菜单 FPS 限制器，游戏内限帧可保留。配置沿用容器环境变量
`XRGAME_PRESENT_ASYNC_COPY=1`；UI 关闭会写 `0`，已有 opt-in 可直接读回。
运行中启用快捷菜单限帧会在已接收 copy 归还后切回同步，关闭限帧则恢复用户的异步选择。
`requestedFrameRateLimit` 为请求值，`frameRateLimit` 为当前生效值。
`copy_pipeline` 不改变游戏 VSync、DXGI 最大在途帧数或源 AHB 归还时机，也不启用直接采样。
它移除 renderer 在 copy 与 draw 之间的 CPU fence 等待；命令池释放仍在 renderer 上，
回收线程只等待 fence 并发出完成通知。队列容量包含已提交但尚未完成的 copy；
窗口取消不能提前归还在途 AHB，GPU 等待失败时保留资源到设备销毁，不发 Idle。
Litep 的 `host.present.copy` 此时是提交阶段，`host.present.retire_wait` 是回收线程的等待；
跨线程 `host.present.queue_to_complete` 仍覆盖完整请求寿命，不能把两个阶段的并行时间相加。
验证与限制见 [MHW](../validation/mhw-present-async-20260928.md) 和
[MHR / UI 验证](../validation/mhr-present-async-20260929.md)。

### 截图与录屏（scrcpy capture SDK，2026-10-14）

头显上的系统截图和面板镜像拿不到 VR 会话的内容（镜像带镜头畸变），所以沉浸式会话自己产出画面：
私有 `my_mcp_tools` 的 capture SDK（`dev_tools/mcp/scrcpy/capture-sdk/native`），经 Foundation
`modules/capture` 链接，只进带 SDK 构建的内部 debug 包。没有 SDK 的包上这三个命令回答
`status: unavailable`。命令与 shadPS4 相同，回答是 SDK 的 `key: value` 行而不是 JSON，
可以直接用 `capturectl.py --app steampsp` 或 scrcpy MCP 的 `capture_app_screenshot` /
`record_app_video`（`service=com.tencentmalos.xrgamenative/app.gamenative.xrgame.DebugBusService`）。

| 命令 | 作用与边界 |
| --- | --- |
| `capture_source [canvas\|xr\|status]` | `canvas`：会话自己画的图，2D 大屏的 quad 图（mono），或 Windows VR 两只眼的投影图并排（`stereo_sbs`）。`xr`：佩戴者看到的画面去掉镜头畸变：把本会话提交的图层（投影、大屏 quad/曲面、叠加层、状态层、键盘）按眼做光线求交合成，两眼并排；截图为运行时推荐眼尺寸（Swan 每眼 2592x2400），录像和直播默认为其一半（1296x1200）。直通和 Pico 系统自己的图层不在其中（直通处为黑）。截图或录像进行中拒绝切换 |
| `capture_screenshot request TOKEN [left\|right\|both]` / `status\|cancel TOKEN` | 无损 PNG，全尺寸（`xr` 为推荐眼尺寸，VR 的 `canvas` 为投影 swapchain 尺寸，大屏 `canvas` 为大屏图层尺寸，2D 窗口为窗口尺寸），TOKEN 为 32 位小写十六进制。下一帧取图，写到 `files/captures/snapshot-TOKEN.png`，用 `run-as` 取回。截图帧用单独的全尺寸图像，写完后释放；录像跳过这一帧。需要沉浸式 Vulkan 合成会话，或前台的 SteamPSP 窗口（见下） |
| `capture_video start [EYE]` / `start_live TOKEN [EYE]` / `stop` / `status` | MediaCodec H.264：文件模式写 `files/captures/embedded-<ns>.h264` 和 `.frames.csv`；live 模式走 `scapture.TOKEN` 本地 socket。默认每隔一个 XR 帧编码一次（72 Hz 下 36 fps，90 Hz 下 45 fps），12 Mbps，关键帧间隔 2 s；编码器没有空闲图像时这一帧不编码、下一个 XR 帧重试，不阻塞 XR 线程。`status` 给出 `skipped_busy`（编码器没还图像）、`skipped_pending`（我们的上一次提交未完成）、`compose_latency_ms_avg/max`、`encoder_images`、`realtime`。会话结束即停止录像 |
| `capture_config [video_bit_rate=8M] [max_size=1024] [max_fps=30] [i_frame_interval=2] \| reset \| status` | 与 scrcpy 的 `--video-bit-rate`/`--max-size`/`--max-fps` 对应，0 表示默认；对之后开始的录像和直播生效，截图不受影响。`max_size` 限制编码画面的长边（编码前线性缩小），`max_fps` 低于默认节奏时按时间间隔编码。`capturectl.py`、`liveview.py` 和 scrcpy MCP `record_app_video` 的同名参数通过它设置，用完恢复原值 |

实现在 Foundation `modules/capture`（2026-10-14 下沉，供其他模拟器复用，接入步骤见 Foundation `docs/guides/emulator-capture.md`）：命令协议 `CaptureHub`、XR 图层 `XrLayerCapture`、2D 窗口 `WindowCapture`。`app/src/main/cpp/xrimmersive/xr_capture.cpp` 只把 `vulkan::Context` 转成 `VulkanBinding`，并配置产品名 `steampsp`、属性前缀 `debug.xrgame.capture` 和日志 tag `xrimmersive`。
- 只在被截取的帧上做额外工作：有待取的截图，或录像中每隔一帧。
- 每个图层在自己的命令缓冲里、`xrReleaseSwapchainImage` 之前用计算着色器按采样方式拷贝进 RGBA8 图像（sRGB 视图采样后再编码回去，字节不变），不需要 swapchain 的 TRANSFER_SRC。投影层在截取帧上不复用上一张图，保证拷到的就是显示的那张。
- 状态层和键盘通过 Foundation `XrImguiVulkanLayer::SetCaptureTarget` 拷贝；它们最多每秒重绘 4 次，所以整个会话都挂着目标图（面板小、拷贝频率与重绘相同），任何一帧的截图都带得上它们。
- `xrEndFrame` 之后在同一队列上做合成（compute）、PNG 回读和编码器 blit；回读数据在完成线程上取出，PNG 在单独线程写，XR 线程不等 GPU。
- 编码器的输入 Surface 走 Vulkan swapchain（合成设备在带 SDK 的构建里打开 `VK_KHR_android_surface`/`VK_KHR_swapchain`），acquire 超时为 0。Swan 上编码器（`c2.qti.avc.encoder`，硬件）要求至少 18 张图，我们建 20 张。
- 编码器按实时任务配置（`priority=0`、`operating-rate` 为编码帧率的两倍、`latency=1`、无 B 帧）：Alyx 场景里编码器自己的输入预处理在 GPU 满载时会压住图像。单眼录像和直播只合成、拷贝那一只眼。
- A/B 开关：`debug.xrgame.capture.legacy=1` 回到最初的流程（只在偶数帧编码、少一张备用图、两眼都合成），`debug.xrgame.capture.realtime=0` 关掉实时配置，`debug.xrgame.capture.spare=N` 改备用图数。
- `max_fps` 按理想时间线排帧：72 Hz 下 30 fps 交替隔两帧、三帧，平均 30 fps（不再固定隔三帧成 24 fps）。

进游戏前的 2D 窗口（游戏库、启动面板、对话框）也能截取：没有沉浸式会话、SteamPSP 自己的窗口在前台时，
`XrPanelCapture`（Kotlin）接管同一组命令，`producer` 为 `steampsp.panel`，`view` 为 `panel`，
`capture_source` 的选择对它不起作用。画面是前台 activity 的窗口加上其上的对话框窗口
（`WindowInspector`，含遮罩），不含 Pico 环境和系统浮层。录像默认按窗口尺寸、30 fps，`capture_config` 同样生效；
截图为窗口全尺寸。沉浸式会话开始时它让出生产者位置，回到 SteamPSP 窗口时重新接上，`liveview.py --follow`
据此自动跟随。
- 我们自己的 `HardwareRenderer` 把窗口画进一个 `AImageReader`，Foundation `WindowFeed` 的线程只取最新一张，经 GLES blit 送进编码器的输入 Surface，旧帧直接丢弃。HWUI 绝不直接画进编码器 Surface：RenderThread 是全进程共用的，编码器一旦不收帧，`dequeueBuffer` 会卡住整个 app 的界面和 DebugBus（run116 的实际故障）。
- 编码器失败、提前结束或 3 s 不收帧时结束这次录制，`status` 为 `failed` 并给出原因；app 不受影响。

构建：`-PxrGameCaptureSdk=<…/capture-sdk/native>` 或环境变量 `XRGAME_CAPTURE_SDK`（只作用于 debug 构建），
`BUILD_INFO.json` 的 `captureSdk` 记录其源文件哈希和 `XRGAME_CAPTURE_SDK_REVISION`。SDK 源码不进本仓库。
Swan 上的验证（Alyx 投影、Hades II 大屏）见 [截图与录屏记录](../validation/swan-xr-capture-20261014.md)。

JSON 返回 `schema: 1`。Kotlin provider 的 `sampledAtBootNs` 使用 Android elapsed realtime
（含休眠）；现有 Present trace `mono_ns` 使用 `System.nanoTime`（CLOCK_MONOTONIC），两者
不可直接相减。`startTimeTicks` 是 Linux `/proc/PID/stat` 字段 22；结合 boot id 使用，不能
只凭 PID 认定调试对象身份。查询不返回 cmdline、环境变量、Steam 账号/票据或任意文件内容。

## 接入边界

```
ADB dumpsys → DebugBusService → JNI → Foundation DebugCommandRegistry
                                      ├─ bridge / host ELF inventory
                                      └─ host providers: runtime / process / Present
```

Foundation 管理命令分发。Android Service 承担 dumpsys transport，因此无须另绑定进程全局
`DumpsysBridge` 注册表；每个请求的注册表/回调只活到 JNI 返回，没有持久 JNI 引用。
单 worker、一个排队槽、2 秒等待上限；进程枚举另有 1 秒预算、128 进程上限，ELF 模块上限
256，回复上限 128 KiB。未知命令返回帮助，非法参数明确报错。查询超时不销毁正使用的注册表，
后续查询可返回 busy；它不保证强行中断系统调用。停止服务释放 worker，不持有 Activity/renderer。

**合适的后续扩展点：**

- X server/renderer：在现有 Present 和 AHB lease 所有者处发布有界状态，供 provider 读取；
  不让 dumpsys 在等待 GPU 的 renderer 锁上卡住。
- Wine/FEX：是独立 Linux 进程。需要其自有诊断端点和 session/PID/start-time 身份校验，
  再由 host 汇总；本次没有接通 guest 寄存器、断点或内存读写。调试控制仍走 native/guest debugger。
- DXVK：在 `CreateBuffer`/资源创建失败处记录描述符、HRESULT、guest 模块信息，由 guest 端点
  汇总。当前 MHW 的 `CreateBuffer(E_INVALIDARG)` 是直接适用的下一项；不能用 host ELF 列表
  推断 guest 的 DXVK 调用参数。

实机证据见 [首次接入验收](../validation/debugbus-20260927.md)。

### 共用 Steam 目录诊断（2026-10-17 候选）

`linux_steam library` 返回每个 ACF 的 app ID、完成状态、depot 数以及存档冲突 app ID；不返回账户身份。`status` 的 `mode=official|custom` 是管理归属，`javaRunning/javaConnected/javaLoggedIn` 用于核验官方运行时 JavaSteam 全部关闭；`libraryOwned` 表示 Official 持有同一游戏目录写入权。旧 `cloud <appId>` 已移除：它建立 AppID 管道后会令 Steam 登记 PID 0，干扰真正的启动。新的 `check` 只查询会话，不携带游戏身份。见[目录共用记录](../validation/steam-shared-library-20261017.md)。

需要诊断官方读取安装记录时，可在客户端停止后创建 app-private `files/linux-steam/library-trace` 空 marker 再启动。只在客户端进程记录有界的 ACF/libraryfolders 文件访问元数据，不记录文件内容；结束后删除 marker 并重启客户端。Steam 更新可能以 42 退出后重开，活动日志会从 `client-0.log` 变为 `client-1.log`，应以运行进程的输出文件为准，不能只检查旧日志。
