# Swan：Clang 版 OpenComposite 的 VR 验证与 MHR 拷贝（2026-10-12）

分支 `feature/malos/swan-xr-ahb`，基线 `dc877ba12`（另一台 Mac 的 Apple Container 构建与掌机输入法修复）。

## 运行时校验与 pin 不一致

- `dc877ba12` 把 `tools/xrgame/opencomposite-pin.json` 和 `apk-sources.json` 切到 Apple Container 里用 Clang（MSVC ABI）交叉编译的 DLL `f5a979e0`。构建记录见 [Odin3 记录](mhr-odin3-container-20261011.md)。
- 但 `WindowsVrPayloadManager.kt` 在安装 OpenComposite 时校验的仍是旧 MSVC 版 `8a8b65ea`。按这个状态出的包，一进 Windows VR 就会报 `OpenComposite payload checksum mismatch`。Odin3 上没有跑 VR，所以没有暴露。
- 修复：运行时校验改为 `f5a979e0`，与 pin 和打包检查一致。

## 构建与安装

| 项目 | 值 |
|---|---|
| APK | `47239195423bff0ba7735ff0f7d427a77b9fc197f02e2886f979e1c3f083c113` |
| OpenComposite DLL | `f5a979e0194eb567410587e3d05400f298ac6a54bc2223f8d26dcb0a995eb31d`，取自 Odin3 已安装的 Mac 构建包，与 pin 一致 |
| 组件清单 | `49a55d7850a6b72757fdca1a3cb4fe6b53d30e5d4aced17ddea287e323f58145`（bundle-xrg13，Turnip `turnip-adb7e30-xrg13`） |
| `libxrimmersive.so` Build ID | `f37a2e38dab62bebfefa36c52db4c30e3183ab8c` |
| 设备 | B3110，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `9cbe019b-9d6f-4a49-8a61-062e11b746e0` |
| 进程 | app PID 11858，`hlvr.exe` PID 29886 |

- 构建：WSL，`picoXrDebug`，打包检查 131 项、0 错误。
- 单元测试：`IMEInputReceiverTest` 7/7、`XrGameConsoleStateTest` 4/4、`XrGameDebugLaunchTest` 2/2。
- 安装后设备上回读的 APK 哈希一致。

## VR 验证（Alyx，无人值守）

用 DebugBus 跑完一轮：`launch 546560 load=s0/autosave` → 读档 → 自动暂停后注入扳机 → `game_state` 连续报告 `inScene` → `quickmenu exit`。设备时间 15:20:51 建立 2592x2400 投影 swapchain，15:22:49 通过菜单退出。

- OpenComposite 日志里，两只眼都只拷显示区域：`1298x1202 at 0,0` 和 `1300x1202 at 1294,0`（`of 3670x1699`）。
- HUD 显示游戏 31.7 fps，XR 90 fps。
- 结论：Clang 版 DLL 在 Swan 的 D3D11 OpenComposite 路径上可以进场景、出画面、正常退出。这是一次约 2 分钟的短测，没有做长时间游玩，也没有和 MSVC 版做性能 A/B。

## 刷新率：90 Hz 是正确设置

- 用户确认 Swan 的 XR 显示应当运行在 **90 Hz**。
- 2026-10-10 的测量（OpenComposite 拷贝和 SGSR 直接写入的 A/B、场景内 GPU 分布）都是在 72 Hz 下做的。当时运行时日志为 `Engine don't call setDisplayFrequency, using recommand refreshRate = 72`。
- 那几组对比在各自的同一局里成立，但绝对帧率和 GPU 占比不能直接套到 90 Hz。以后的测量以 90 Hz 为准。

## MHR 拷贝（Odin3 → Swan）

- 源：Odin3 `/sdcard/XRGameNative/Steam/steamapps/common/MonsterHunterRise`，共 185 个文件，约 34.5 GiB，其中 `re_chunk_000.pak` 为 32 GB。
- 方法：用两个 adb 管道直连设备，不经主机暂存。普通文件走 tar；大文件超出 ustar 的 8 GiB 上限，单独用 `dd`/`cat` 传。
- 拷贝中 Swan 的 USB adb 连接断开两次：15:20 和 15:30，adbd 报 `UsbFfs: connection terminated ... Broken pipe`。之后改为按 MiB 边界每次续传 2 GiB，断线后自动重试。
- 校验：两端文件列表和大小一致，185 个文件的 SHA-256 全部相同。Swan 剩余 22 GB。
- Swan 当时没有网络，Steam 登不上，云同步会被跳过。首次启动 MHR 前需要联网，否则拿不到云端存档。
