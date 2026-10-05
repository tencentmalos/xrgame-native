# 图形 API 录制与回放 v1（API replay）

- 日期：2026-10-04
- 状态：**D3D12 路径已在 AYN 上打通**（2026-10-04）：MHR 在设备上录制，Windows 原生 D3D12 回放出正确画面。
  见[验证记录](../validation/api-replay-20261004.md)。在 Wine/FEX 下，D3D12 必须用 unassisted 内存跟踪，用 page_guard 会黑屏。
- 参照：本机 citron `docs/plans/20261004-gpu-deterministic-replay.md`（未提交），shadPS4
  `docs/specs/gpu-replay-20261004.md`（`f75c59eef`）。

## 1. 目标

1. **正确绘制参照。** 在 AYN 上录制 PC 版 MHR / MHW 的一段画面（例如选角界面），在 Windows 上
   用原生 D3D12 驱动回放，用 PIX / RenderDoc 逐 draw 分析，作为 citron（MHR Switch 版）和
   shadPS4（MHW PS4 版）效果修正的参照。
2. **翻译层对照。** 与 citron / shadPS4 的 replay 一样，录制后脱离游戏确定性重放，用于比较
   VKD3D / DXVK / Turnip 的不同版本。
3. 录制在 Android，回放首先在 Windows；在设备上回放放到后面（§6 P2）。

## 2. 与 citron / shadPS4 的对应关系

| | citron | shadPS4 | 本仓 |
|---|---|---|---|
| guest 交给 GPU 的接口 | GPFIFO / pushbuffer | PM4 | D3D8–12 API |
| 翻译层 | video_core（Maxwell→Vulkan） | video_core（GCN→Vulkan） | DXVK、VKD3D-Proton、Wine、FEX、Turnip |
| 录制内容 | GPFIFO 提交 + guest 内存快照与页面增量 | PM4 提交、等待结果 + 内存快照与增量 | API 调用流 + 映射内存增量，触发时写状态快照（GFXR trim） |
| 回放 | citron-cmd，不加载游戏 | shadps4.exe，不加载游戏 | `gfxrecon-replay`，不运行游戏 |
| 格式 | `.cgpurply` | `.sgpurply` | GFXR `.gfxr` |

两边的格式和容器代码目前没有共享。本仓沿用它们的输出约定：逐帧 PNG、`frames.txt`（帧序号、哈希、
宽、高）、`replay_summary.txt`，以及 DebugBus 的 `*_capture / status / cancel` 命令风格。

## 3. 录制层次（用户 2026-10-04 决定）

| 游戏使用的 API | 录制位置 | Windows 回放 |
|---|---|---|
| D3D12 | GFXR 的 `d3d12.dll` / `dxgi.dll` proxy，在 API 层录制 | 原生 D3D12 |
| D3D8 / 9 / 10 / 11、原生 Vulkan | Wine 进程里加载的 GFXR Vulkan 层，录 DXVK 转换后的 Vulkan | Vulkan |

- D3D12 游戏也可以选择在 Vulkan 层录一份，用来判断问题出在 VKD3D 还是 Turnip。
- 不使用 apitrace。
- Vulkan 层录到的是转换后的调用：分析时看到的是 DXVK 的输出，而不是游戏原始的 D3D11 调用。

## 4. GFXReconstruct

- 来源：`LunarG/gfxreconstruct` `dev` @ `6dc9b65`，MIT 许可。
  计划 fork 为 `tencentmalos/gfxreconstruct`（由用户创建），补丁放在
  `feature/malos/xrgame-wine-capture` 分支，之后作为 `references/gfxreconstruct` 子模块接入。
- 补丁（都是 Wine / Android 宿主适配，不改变录制格式）：
  1. `GFXRECON_DX_RUNTIME_DIR`：proxy 从这个目录加载 `<dll>_ms.dll`（`framework/encode/dx12_dll_initializer.h`）。
     原有的两种方式（exe 旁边放改名的 DLL；从 `System32` 复制到 `%APPDATA%`）在 proxy
     替换了 system32 里的 DLL 后会加载到它自己。
  2. `GFXRECON_CAPTURE_TRIGGER_FILE`：文件存在时触发截取（`framework/encode/capture_settings.cpp`）。
     原来的运行时触发只在 Android 上编译，而且依赖系统属性。
  3. Android 版 `GetEnv`：属性没有设置时，回退读取同名的 `GFXRECON_*` 环境变量（`framework/util/platform.h`）。
     这样可以按容器传设置，不改动全局属性。
- 构建产物：
  - Windows x64，MSVC，静态 CRT（`/MT`，不依赖 Wine 的 `msvcp140`）：
    `d3d12.dll`、`dxgi.dll`、`d3d12_capture.dll`、`gfxrecon-replay.exe` 和 info / convert 工具。
  - Android arm64：用 NDK 27.3 编 `libVkLayer_gfxreconstruct.so`。

## 5. 设备侧接入（仅 picoXr debug）

- **容器设置：** 环境变量 `XRGAME_API_CAPTURE=off|d3d12|vulkan`，`XRGAME_API_CAPTURE_FRAMES=N`（默认 3）。
- **D3D12 模式**（在 `XrGameRuntime.installGraphics` 之后安装）：
  - 把 VKD3D 的 `d3d12.dll` 复制为 `C:\xrgame\gfxr\runtime\d3d12_ms.dll`，DXVK 的 `dxgi.dll` 复制为 `dxgi_ms.dll`；
  - 把 GFXR 的 proxy 和 `d3d12_capture.dll` 放进 system32；
  - 下次不开录制启动时，`installGraphics` 会按 SHA 把 system32 恢复原样。
- **Vulkan 模式：** 层库放在 app 私有目录，层清单放进 imagefs 的 `explicit_layer.d`，并设置 `VK_INSTANCE_LAYERS`。
  注入方式和已经验证过的 RenderDoc 层相同。
- **环境变量：** `GFXRECON_CAPTURE_FILE`、`_TRIGGER_FILE`、`_TRIGGER_FRAMES`、`_COMPRESSION_TYPE=zstd`、
  `_LOG_FILE`、`_MEMORY_TRACKING_MODE`。
  内存跟踪默认值：D3D12 用 `unassisted`，因为 page_guard 在 Wine/FEX 下会让渲染变黑；Vulkan 暂用 `page_guard`，还没测过。
  可以用 `XRGAME_API_CAPTURE_MEMORY` 覆盖。
- **输出：**
  - 写到 app 私有的 `files/imagefs/xrgame-captures/<容器>/`（Wine 里是 `Z:\xrgame-captures\...`），
    避开 `/sdcard` 的 FUSE 写入开销；
  - 同目录写一份 `capture.json`，记录 APK 版本、catalog SHA、默认组件版本、GFXR 产物 SHA、
    游戏 exe 的路径和大小、设备型号 / build / boot_id、录制模式与参数。
- **DebugBus 命令：** `api_capture [status|start|stop|list]`。
  `start` 创建触发文件，`stop` 删除它；帧数在启动时就固定了（v1）。
- **主机脚本**（`tools/xrgame/api-replay/`）：
  - 拉取录制文件（`run-as` 读出）；
  - 在 Windows 上回放并生成截图、`frames.txt`、`replay_summary.txt`；
  - 用 RenderDoc 抓取回放中的指定帧，交给 renderdoc MCP 分析。

### 5.1 实现位置（2026-10-04，未经设备验证）

| 部分 | 位置 |
|---|---|
| 录制设置、安装、环境变量、sidecar | `app/src/main/java/app/gamenative/xrgame/XrGameApiCapture.kt`；`XServerScreen` 在 `installGraphics` 之后调用 `install`，在合并容器环境变量之后调用 `configure` |
| 图形设置下拉项（仅当 APK 带 GFXR 文件时显示） | `GraphicsTab.kt`，文案在 `strings_xrgame.xml` |
| DebugBus `api_capture` | `app/src/picoXr/cpp/debugbus.cpp`、`app/src/picoXrDebug/.../DebugBusService.kt` |
| 打包 | `tools/xrgame/stage-gfxr.py` 生成 `build/xrgame-gfxr/`（gitignore），Gradle 只加到 `picoXrDebug` 的 assets；`tools/xrgame/apk-sources.json` 逐字节比对 |
| GFXR 补丁 | `tools/xrgame/patches/gfxreconstruct-wine-capture.patch`（fork 建立前的版本化记录） |
| 主机工具 | `tools/xrgame/api_replay.py`：`deploy`（不改 APK 的 Vulkan 层部署）、`start/stop/status`、`pull`、`restore`、`replay` |
| 单元测试 | `app/src/test/java/app/gamenative/xrgame/XrGameApiCaptureTest.kt` |

用法（APK 已带 GFXR）：在游戏的"编辑容器 → 图形 → 图形 API 录制"选择模式并重启游戏，到达目标画面后执行
`python tools/xrgame/debugbus.py --serial <设备> api_capture start`；完成后用
`api_replay.py pull` 拉回，再用 `api_replay.py replay` 在 Windows 上回放。

## 6. 阶段与验收

- **P0：** 补丁与 Windows / Android 构建；设备上手工部署：
  - Vulkan 层不需要改 APK；
  - D3D12 需要新 APK，或者经用户同意后临时把 proxy 放进游戏目录。
  - 验收标准：在 AYN 上录到 MHR 选角界面，在本机回放出画面。
- **P1：** app 接入（§5）、主机脚本、`docs/validation` 记录；同一份录制回放两次，帧哈希一致。
- **P2：** 在 AYN 上用 Wine / VKD3D / Turnip 回放；D3D12 游戏的 Vulkan 层对照录制。
- **P3：** ARM64EC 构建的截获 DLL，降低在 FEX 下的录制开销。

## 7. 约束与风险

- **不进仓库：** 录制文件包含游戏的纹理和着色器，属于游戏资产，只放在仓库外的证据目录。
  GFXR 二进制只进 debug APK，不进生产 catalog；接入时补充 `THIRD_PARTY_NOTICES`。
- **待验证的风险：**
  - x64 截获代码在 FEX 下模拟运行，录制会变慢。citron 录制时也只有约 5 fps，因为录制本身不依赖时序，这不影响结果。
  - page-guard 内存跟踪依赖 Wine/FEX 的异常和信号处理。
  - 在 Turnip 设备上录的文件拿到 AMD 上回放，兼容性不保证。
  - Wine 进程里加载层：同样方式加载 RenderDoc 层时曾阻止 explorer 启动 Steam 加载器，原因未查清。
