# 图形 API 录制与回放：MHR D3D12 首次设备验证（2026-10-04）

- 规格：[xrgame-native-api-replay-v1](../specs/xrgame-native-api-replay-v1.md)
- 状态：**D3D12 路径打通**：AYN 上录制，Windows 原生 D3D12 回放出正确画面。
  RenderDoc D3D12 图像导出及 draw 检查已补充验证（见 §5）。
  2026-10-05 补录了选角界面（"选择游戏数据"），见 §7。Vulkan 层模式和 MHW 尚未完成（见 §6）。
- 原始日志、截图、录制文件、回放帧都保存在仓库外的 `xrgame-native-evidence/api-replay/run-20261004/`
  和 `run-20261005/`，每个目录都附 `SHA256SUMS`。
  录制文件包含游戏资产，截图里有其他 app 的界面，所以都不提交。

## 1. 身份

| 项 | 值 |
|---|---|
| 源码 | `malos/main` @ `4ef46c6b` 加上 `feature/malos/api-replay` 的未提交改动。打这个 APK 时 D3D12 的默认跟踪模式还是 page_guard；表中 unassisted 的几轮都是用容器环境变量 `XRGAME_API_CAPTURE_MEMORY=unassisted` 显式指定的。默认值改成 unassisted 之后只重跑了单元测试，没有再上机 |
| APK | `app-picoXr-debug.apk`，在 WSL（Ubuntu 24.04）中从头打包，SHA-256 `7a9942c113a49bbe1e343495471dce9f46bc405a2548df49092845b23e72a09c` |
| 签名 | 证书 SHA-256 `ce9d5b4d…f8eb`，与设备上原有的 XRGame / shadPS4 APK 相同；`adb install -r` 覆盖安装，app 数据保留 |
| APK 审计 | `tools/audit-apk --validation-catalog build/xrgame-runtime/bundle/manifest.json`：129 个文件，0 个错误 |
| 单元测试 | WSL 中运行 `:app:testPicoXrDebugUnitTest --tests app.gamenative.xrgame.XrGameApiCaptureTest`：6 个用例，0 失败，0 错误（默认内存跟踪改动之后重跑） |
| 运行时 bundle | 从设备上原有 APK（`3379cd8b…`）中提取，catalog SHA-256 `e8f2780a71024d47…`，构建前逐项校验 |
| GFXR 源码 | `LunarG/gfxreconstruct` `dev` @ `6dc9b65` 加 `tools/xrgame/patches/gfxreconstruct-wine-capture.patch`（SHA-256 `0ab08034…`）。2026-10-05 起，这份改动以提交 `3868cd12` 的形式维护在 `tencentmalos/gfxreconstruct` 的 `feature/malos/xrgame-wine-capture` 分支，它相对 `6dc9b65` 的 diff 与补丁逐字节相同 |
| GFXR 截获 DLL（x64，静态 CRT） | `d3d12.dll` `1ed70901…`、`dxgi.dll` `e6ab5d5d…`、`d3d12_capture.dll` `91b40c37…`；只导入 Wine 自带的系统 DLL |
| GFXR Vulkan 层（arm64，已 strip） | `989b71f8…`，Build ID `5d6a7e218cde8e89547eeac85d5e1a885d94ace7`（本轮未使用） |
| Windows 回放工具 | `gfxrecon-replay.exe` `d45108dc…`（MSVC，动态 CRT），旁边需要 Agility SDK 的 `D3D12\D3D12Core.dll`（`eddf4cff…`） |
| 设备 | AYN Thor，`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`，boot_id `03c6cbd9-0c0a-4264-ad61-d18d4a63272f` |
| 游戏 | MHR（Steam 1446780），`MonsterHunterRise.exe` 文件版本 16.0.2.0，D3D12，VKD3D `11.0-212991f-arm64ec-xrg1` |
| 回放主机 | Windows 11，AMD Radeon 890M |

## 2. 设备侧接入是否生效（`WINEDEBUG=+loaddll`）

MHR 进程依次加载：
1. `system32\dxgi.dll`、`system32\d3d12.dll`（GFXR proxy）；
2. `C:\xrgame\gfxr\runtime\dxgi_ms.dll`（DXVK）、`d3d12_ms.dll`（VKD3D）；
3. `system32\d3d12_capture.dll`；
4. `system32\d3d12core.dll`。

`gfxrecon.log` 记录了 `Initializing GFXReconstruct capture layer` 和版本 `1.0.5-dev (xrgame-wine-capture:6dc9b65*+dx12)`。
VKD3D 通过 DXGI 正常建出交换链：`Got 4 swapchain images`，1280×720。

## 3. 各轮结果

| 轮次（主机时间） | 设置 | 结果 |
|---|---|---|
| 22:07 | d3d12，page_guard | 约 28 秒后 guest 退出，黑屏，没有留下原因；日志也没开 |
| 22:09 | d3d12，page_guard，`+loaddll` | 进程持续运行，但 5 分钟以上一直以 60 FPS 输出黑帧。在 22:15:56 触发录制，见 §4 第 1 份录制。后来进程退出，原因没记下：logcat 采集到了时间上限 |
| 22:21 | 关闭录制 | 第 60 秒显示 Sunbreak 主视觉图，第 150 秒弹出 `[MR-111-81]` 连接错误对话框，**渲染正常** |
| 22:25 | d3d12，page_guard，DXVK info 日志 | 仍然黑屏。DXVK 只生成了 DXGI 日志，没有 D3D11 设备，排除了"开场视频走 D3D11 失败"的推断 |
| 22:27–22:33 | d3d12，unassisted | 第 1 次：在 SteamAPI 初始化后 16 秒出现 `NtRaiseHardError 0x50000018 4 0x3`，随后退出（与 09-27 记录的偶发错误相同，不开录制也会发生）。第 2 次：22:31:44 系统内存压力回收，同时杀掉了 SteamService，以及当时在后台运行的 citron BOTW 会话。第 3 次：启动成功 |
| 22:33 起 | d3d12，unassisted | **渲染正常**：先显示 MR-111-81 对话框；按空格依次经过自动保存提示和加载页，游戏载入最近的存档，进入埃尔迦德据点 |

**结论：** 在 Wine/FEX 下，GFXR 的 page_guard 内存跟踪会让 MHR 的 D3D12 渲染变黑，设备上和 Windows 回放里都是黑的。
我推断是游戏写入的数据留在了 GFXR 的影子内存里，没有到达 VKD3D 的堆，这一点未验证。
unassisted 模式下渲染正确，所以 `XrGameApiCapture` 现在对 D3D12 默认使用 unassisted。

## 4. 录制与 Windows 回放

触发方式都是 `debugbus.py api_capture start`，每次录 3 帧，录完立即 `stop`。
回放方式都是 `api_replay.py replay`，即 `gfxrecon-replay --screenshot-all`。

| 录制 | 跟踪模式 | 游戏帧号 | 文件大小 | 录制用时 | Windows 回放 |
|---|---|---|---|---|---|
| `…T221556.gfxr` | page_guard | 19251–19253 | 110,069,625 B | 10 秒以内 | exit 0，恢复状态用了 179 秒；3 帧全黑（所有像素为 0），与设备上一致 |
| `…T223644.gfxr` | unassisted | 9130–9132 | 299,076,711 B | 10 秒以内 | exit 0，4.4 秒；3 帧哈希各不相同，**MR-111-81 对话框完整渲染** |
| `…T224046.gfxr` | unassisted | 18544–18546 | 1,047,227,349 B | 约 20 秒 | exit 0，51.7 秒；3 帧哈希各不相同，**据点场景、角色、随从和 HUD 完整渲染**，与设备截图一致 |

- `gfxrecon-info` 显示，录制时的适配器是 VKD3D 报告的 `Turnip Adreno (TM) 740`（vendor 0x5143）。
- 在 AMD 上回放时，GFXR 提示 "Recorded instructions contain data aimed for the capture-time GPU"，但这三份都回放成功了。

## 5. RenderDoc

- 用 Pico 版 RenderDoc 1.27（`32fef253`）的 `ExecuteAndInject` 加 `QueueCapture`，抓到了第 3 份录制回放时的第 2 帧：`mhr_elgado_frame2.rdc`，D3D12，779,744,559 B。
- 当时那个构建不能回放 D3D12：`LocalReplaySupport = Unsupported`，renderdoc MCP 的 `capture_open` 返回 `APIUnsupported`。
- **2026-10-05 补充：** 换用带 D3D12 回放的 MCP 构建 `renderdoc_mcp-0.1.0-local.20261005.d3d12.70d50ee` 后：
  - **`capture_open` 成功：** D3D12，5321 个根动作，5994 个事件，5093 个资源。
  - **数据库查询正常：**
    - 全帧共 2691 个 draw（事件 42086–60069），大量绘制通过 `ExecuteIndirect` 下发，中间穿插 compute dispatch；
    - 后台缓冲是资源 `267`（1280×720，`R8G8B8A8_UNORM`），只在事件 60343 被写入一次。
  - **原生 API 回放正常：** 用同一构建自带的 qrenderdoc Python API，`LocalReplaySupport = Supported`，`OpenCapture` 成功；`SetFrameEvent(60343)` 后 `SaveTexture(267)` 导出的就是完整的据点画面，与设备截图和 GFXR 回放截图一致。
  - **MCP 工具层失败：** `texture_export`、`review_texture`、`render_target_export`、`draw_inspect` 对这份 capture 一律返回 "An error occurred invoking"，资源 `267` 和 `221` 都试过，`capture_messages` 为空，也没有生成 journal。
  - 当时推断 MCP 回放状态工具存在 D3D12 问题；后续用原始调用参数复现，确认是调用参数／前置状态错误和不透明错误提示，见下方更正。

### 2026-10-05 MCP 复核与修订

对照原 Claude 会话调用记录，用相同 `mhr_elgado_frame2.rdc` 和相同旧安装版本启动独立 MCP 进程：
`texture_export`、`review_texture`、`draw_inspect`、`render_target_export` 在正确参数下均成功。
因此，前述失败不能归因为 D3D12 回放实现损坏。逐项复现出的原因是：

| 原调用条件 | 实际错误 | 修正方法 |
|---|---|---|
| `capture_open` 后直接预览资源 `267`，未传事件 | 尚未选择事件 | 先 `event_select`，或在请求内传 `eventId` |
| `eventId: 60343` | JSON 数字无法绑定到字符串参数，工具还未执行 | 使用 `eventId: "60343"`；资源 ID 同样使用十进制字符串 |
| 指定 evidence 目录下的 `outputPath` | 路径不在配置的 artifact root 内；修正类型后才会到达这一检查 | 省略 `outputPath`，使用返回的 `artifactPath`；或预先配置 `RENDERDOC_ARTIFACT_ROOT` |
| `draw_inspect` 使用事件 `60343` | 该事件不是 draw；修正类型后仍会拒绝 | 选择真实 draw，例如已验证的 `60069` |

旧版将这些异常全部隐藏成 `An error occurred invoking ...`，且它们不是原生回放失败，不能依靠
`capture_messages` 或 capture-open journal 解释。MCP 修订版
`0.1.0-local.20261005.d3d12fix1.70d50ee` 已安装到本机 Codex、Claude Code 和 Claude Desktop：
保留字符串 ID 和 artifact root 规则，在 `isError: true` 响应中返回错误码、具体原因与修正建议。
Native DLL 与前一版相同；更新的是托管工具错误处理与客户端说明。已有客户端会话需要重新连接 MCP。

在同一会话中先 `capture_open`，然后可直接使用这些已验证的工具参数：

| 工具 | 参数 |
|---|---|
| `texture_export` | `{"request":{"resourceId":"267","eventId":"60343","format":"png"}}` |
| `review_texture` | `{"request":{"resourceId":"267","eventId":"60343","format":"png"}}` |
| `draw_inspect` | `{"eventId":"60069"}` |
| `render_target_export` | `{"request":{"eventId":"60069","attachment":"color","attachmentIndex":0,"format":"png"}}` |

安装后实际复测：五种错误调用返回可操作错误；上述四种正常工具成功；`review_texture` 返回两张
MCP image（原始 RGB 预览和诊断曝光图）；最后 `capture_close` 后 `capture_status.isOpen = false`。
事件 `60343`、资源 `267` 导出的 1280×720 PNG 为 2,901,460 字节，SHA-256
`6b5ef8e73549197ef8dcc9d0d25621f900adc90fa1203bbe77d402064dd09c53`，与前述原生 Python
`SaveTexture` 导出的 PNG 完全相同；已查看 MCP 原始 RGB 预览，据点、角色、随从和 HUD 正常。
这只验证所列图像／draw 工具，不代表完整 D3D12 分析工具或最终显示链路验收。

本地证据在 `D:\workspace\my_mcp_tools\artifacts\renderdoc-d3d12-mhr-20261005\`：
`before/results.json`、`invalid-before/stderr.log`、`installed/results.json` 和 `tests.log`。
60 项 MCP 合约测试通过；8 个已安装 MCP 的 initialize/tools/list 以及 Codex／Claude MCP 列表验证通过。
原始会话和图像证据留在本地，不提交游戏数据。

## 6. 未完成与限制

- 10-04 这一轮没录到选角界面，因为游戏直接载入了最近的存档。10-05 已补录，见 §7。MHW 和 Vulkan 层模式都还没测。
- AYN 在拉取第 3 份录制之后从 adb 断开，后续设备测试暂停，10-05 重新连接。
- MHR 容器目前（10-05）仍保留测试设置：`XRGAME_API_CAPTURE=d3d12`、unassisted、`WINEDEBUG=warn+seh`、DXVK 和 VKD3D 的诊断日志。
  原始配置备份在 `files/imagefs/xrgame-captures/STEAM_1446780/container.before-api-replay`，
  可以用 `api_replay.py mode --mode off` 或 `restore` 恢复。
- 设备同时被 citron / shadPS4 的测试占用：内存压力和前台切换会影响结果，22:31 那次已经影响到了 citron 的会话。
- 这一轮只看了出画面和回放是否成功：只录 3 帧，没有做逐帧对比、长时间运行或性能测量。
  据点场景开着跟踪时，HUD 显示 20.3 FPS，这个数字只作为当时的观察，不代表性能结论。

## 7. 2026-10-05 补录：选角界面（"选择游戏数据"）

证据目录 `xrgame-native-evidence/api-replay/run-20261005/`，`SHA256SUMS` 覆盖 18 个文件。

**环境**
- APK、设备和 boot_id 都与 §1 相同：APK `7a9942c1…`，安装时间 2026-10-04 22:07:22；boot_id `03c6cbd9-…`，说明设备没有重启过。
- 用户自己启动 MHR、进入选角界面后停住。进程是 PID 10540（`A:\MonsterHunterRise.exe`），录制结束时已运行 3 分 54 秒，仍在运行。
- 这次启动的 `gfxrecon.log` 和 `capture.json` 确认了录制设置：d3d12、unassisted、触发后录 3 帧、ZSTD 压缩，GFXR 版本 `1.0.5-dev (xrgame-wine-capture:6dc9b65*+dx12)`。

**触发与拉取**
- `debugbus.py api_capture status` 返回 `No services match: …DebugBusService`，原因没有查。
  因此改用 `api_replay.py start`，它通过 run-as 直接创建触发文件，主机时间 17:44:18。
- 17:44:19 开始写录制文件。17:44:35 第一次轮询时，日志里已有 `Finished recording graphics API capture`。随后用 `stop` 删除了触发文件。
- 日志里有两条警告：
  - `Skipping resource data capture for multi-sampled resource(s)`；
  - `CopyDescriptors was called with a source descriptor that may not have been initialized`。
- `api_replay.py pull` 会拉取目录里所有的录制文件。为了不重复拉 10-04 的三份，这次逐个文件用 `exec-out run-as cat` 拉取，每个文件的大小都与设备上一致。

**录制与 Windows 回放**

| 项 | 值 |
|---|---|
| 录制 | `capture_trim_trigger_20261005T174419.gfxr`，761,704,485 B，SHA-256 `bbb36ad44f19990b72b66fa4f9a18c3532b7e91b7e79402b1fc99af697645114` |
| `gfxrecon-info` | 共 3 帧，游戏帧号 10393–10395，D3D12 适配器 vendor 0x5143 |
| 回放 | `gfxrecon-replay` `d45108dc…`：exit 0，用时 7.6 秒；3 帧都是 1280×720，三帧像素哈希各不相同 |
| 画面对比 | 第 3 帧与触发时的设备截图一致：存档卡片、猎人、随从、篝火和光照都完整。设备截图左上角的性能浮层由 app 在 Android 侧绘制，不在录制里 |
| RenderDoc | 用 `renderdoc_mcp-0.1.0-local.20261005.d3d12fix1.70d50ee` 自带的 qrenderdoc，通过 `ExecuteAndInject` 注入回放进程，再用 `QueueCapture` 抓第 2 帧 |
| RenderDoc 抓帧结果 | `mhr_save_select_frame2.rdc`，D3D12，510,371,230 B，SHA-256 `fd1e3664ccf9b9b9cbc48447953d2a40d0c44b6fecf973234c7d73e9aa549342` |

**未做的**
- 这份 `.rdc` 还没有在 renderdoc MCP 里打开分析。
- 没有做逐帧像素对比或性能测量。
- MHR 容器仍保留 §6 所列的测试设置。
