# Swan：转换层纹理质量与阴影渲染缩放（2026-10-13）

规格见 [D3D 转换层的渲染缩放与纹理质量 v1](../specs/xrgame-d3d-render-scale-texture-quality-v1.md)。本记录覆盖：

- vkd3d-proton（D3D12）在 MHR 上的首轮验证；
- DXVK（D3D11）在 Half-Life: Alyx 上的首轮验证。

组件是开发期替换进设备的，不是新打包的组件版本。

## 环境

| 项目 | 值 |
|---|---|
| 设备 | Swan（Pico B3110），系统 6.2.0，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys` |
| boot id | `6d4335a5-667c-426d-815b-1a82edaa0fbb` |
| APK | 开发期替换阶段：run98，`71d610f2b914287e8716c06b8476a36ff52b459939996478f000d31869367625`；打包后：run100，见下文 |
| 显示 | 2D 大屏（XR），插帧 2x，显存预算 4G（MHR）；Alyx 为 Windows VR |

### 替换的组件文件

做法：替换设备上已安装组件目录里的 DLL，同步改写组件的 `xrgame-build.json` 哈希和安装回执。原文件备份在 app 私有目录 `files/xrgame-dev-backup/`。

| 组件 | 文件 | 第 1 版 | 第 2 版（只缩放深度数组） |
|---|---|---|---|
| VKD3D `11.0-212991f-arm64ec-xrg1` | `d3d12core.dll` | `01119dd1…aaddfe` | `c4baca07…a1c1` |
| VKD3D `11.0-212991f-arm64ec-xrg1` | `d3d12.dll` | `6c87e1ea…ab32e` | 同第 1 版 |
| DXVK `11.0-a676404-arm64ec-xrg4` | `d3d11.dll` | `65016b2e…8e248b` | `70d4d0e2…ff4e` |

补丁（第 2 版，即仓库里的版本）：

- `vkd3d-xrgame-resource-scale.patch`：`a86deb67bfe9ac721c13e6c09ae6c491f92c2954938da6b226ffc7a9fabd38b2`
- `dxvk-xrgame-resource-scale.patch`：`3d7f5af359640ea25cd421f4762ac31c2d19250111286a4a1c064a86ac67d9e0`

构建：远端 `xrgame-runtime-build` 容器，llvm-mingw arm64ec，release。改动的文件没有新增编译警告。

### 采样方法

- 每 2 秒采样一次 KGSL 的 mapped + unmapped、RSS、swap、整机可用内存、dmabuf 页池和 PSI。
- 采样脚本是 `mhr-memmon.sh` / `game-memmon.sh`，需要 adbd 为 root。第一轮在设备重启后没有 root，GPU 一列为 0，`adb root` 之后补采。
- unmapped 超过 2 GiB 时 32 位回绕成负数，下文已按 +4096 MiB 换算。

## MHR（D3D12，vkd3d-proton）

三轮都从启动跑到标题画面：默认 A、低 + 50% 的 B、再跑一次默认 A2。每轮之间先经快捷菜单退出游戏，并在 app 停止后改容器环境变量。

vkd3d 日志（B）：

- `XRGame texture quality low, shadow render scale 50%.`
- 4 张阴影深度数组缩小：
  - 2 × `1024x1024x32 -> 512x512`，各省 97.8 MiB；
  - 2 × `2048x2048x32 -> 1024x1024`，各省 382.5 MiB；
  - 合计 960 MiB。
- 之后又重建了 2 张 2048 的数组，累计计数为 6 张。
- 纹理质量：到标题时 64 张，省 39.5 MiB。
- 拷贝、resolve、barrier 的「跳过」分支一次都没有触发；也没有出现「缩放深度图和渲染目标绑在一起」的告警。

| | A 默认 | B 低 + 50% | A2 默认 |
|---|---|---|---|
| 加载阶段 GPU（mapped + unmapped） | （未采到） | 670 + 555 ≈ 1.20 GiB | 671 + 1344 ≈ 1.97 GiB |
| 标题稳定后 GPU | 1058 + 1904 ≈ 2.89 GiB | 1090 + 1054 ≈ 2.09 GiB | 1072 + 1904 ≈ 2.91 GiB |
| 标题稳定后 MHR 驻留 / 换出 | 3.85 / 0.74 GiB | 3.58 / 1.02 GiB | 2.97 / 1.61 GiB |
| 进标题时 LMK 杀进程 | 31 个（17:37:13 一次连杀） | 0 | 6 个（17:51:34–38） |
| PSI some avg10 峰值 | （未采到） | 2.78 | 7.17 |
| 设备丢失 | 无 | 无 | 无 |

- 同一会话前后两轮默认档，GPU 内存一致（2.89 / 2.91 GiB）。B 比默认档少约 0.8 GiB，和 vkd3d 统计的 960 MiB 加约 40 MiB 纹理同一量级。
- 关于 LMK：A 先跑并杀掉了一批系统进程，B 紧接着跑，所以 A、B 之间的对比有混杂因素。A2 在 B 之后跑，仍然杀了 6 个进程，PSI 峰值也更高。可以说「低 + 50%」在这台设备上消除了进标题时的 LMK 连杀，但样本只有一轮。
- 画面：标题画面被 MHR 离线时的「连接错误」对话框挡住，阴影和纹理的画面对比还没做，需要进村庄后在同一位置开关对比。

## Alyx（D3D11，DXVK）

B 组（低 + 50%，第 1 版 DXVK）经 DebugBus 无人值守读档进场景。容器里临时加了 `DXVK_LOG_LEVEL=info`。

DXVK 日志：

- `XRGame texture quality low, shadow render scale 50%`
- 纹理质量：到场景时 256 张，估算省 852 MiB。Alyx 有大量 2048 纹理，按上限 512 各丢 2 级。
- 深度缩放（第 1 版规则包含方形图）：`4096x4096x1 -> 2048x2048` 和 `3072x3072x1 -> 1536x1536`，估算共省 75 MiB。
- **告警**：`a depth target scaled by the render scale setting is bound with render targets`。说明「方形 ≥ 2048 的深度图都是阴影图」这个判断在 Alyx 上不成立：这种绑定下渲染目标只会画到一部分。
  - 第 2 版因此只缩放深度数组，方形图不再缩放，vkd3d 和 DXVK 规则一致；告警会带上两边的尺寸。
  - MHR 的 4 张都是数组，不受影响。

场景内（进场景后约 1 分钟）：GPU 217 + 2446 ≈ 2.60 GiB，驻留 3.1 GiB，整机可用约 1.1 GiB。头显截图见证据目录 `scene-low-50.jpg`：画面正常，没有花屏或黑块，近处海报纹理明显变糊，符合「低」档预期。

### Alyx 默认档基线

冲突按用户选择保留了本地存档：之后本地存档与备份逐文件一致。随后 Alyx 按默认档（只加 `DXVK_LOG_LEVEL=info`）读档进场景：

- DXVK 日志只有 `XRGame texture quality high, shadow render scale 100%`，没有处理任何资源。
- 场景内：GPU 237 + 3475 ≈ 3.63 GiB，整机可用约 0.84–0.94 GiB。约 1 分钟后 hlvr 被换出约 1.1 GiB（驻留 2.36 → 1.26 GiB，换出 0.84 → 1.97 GiB），PSI avg10 升到 2.99。

| | 默认 | 低 + 50%（第 1 版） |
|---|---|---|
| 场景内 GPU（mapped + unmapped） | 237 + 3475 ≈ 3.63 GiB | 217 + 2446 ≈ 2.60 GiB |
| 整机可用 | 0.84–0.94 GiB | 1.1–1.2 GiB |

纹理质量「低」在 Alyx 上让 GPU 内存少约 1.0 GiB（-28%），和 DXVK 估算的 852 MiB 加深度 75 MiB 同一量级。两轮不在同一时段，系统状态不同，整机可用内存只作参考。

### 冲突经过

- 退出 B 组时快捷菜单不响应：`quickmenu exit` 的确认键没有被任何项消费，`quickmenu close` 后菜单仍可见，游戏一直处于暂停（SIGSTOP）。只能 `am force-stop` 结束，因此没有走退出时的云同步。这个问题与转换层无关，已单独提出。
- 下一次启动 Alyx 时，云同步报「本地有改动、云端也有新文件」，弹出冲突选择框，等待用户决定：
  - 云端变更 15 是 B 组启动前 app 自己上传的 9 个文件；
  - 但 app 本地记录的变更号仍是 14；
  - B 组读档后 Alyx 又改写了本地 autosave。
- 处理冲突前已备份本地存档：`hlvr/save` 下 12 个文件，以及 Steam `userdata`。位置在证据目录 `alyx/save-backup-1810/`，带 SHA-256 清单。
- 用户在头显上选择保留本地存档。

## 打包后的组件（run100）

按用户要求把第 2 版打成正式组件并出新 APK：

- 远端用仓库里的配方在独立构建根目录中重编：`build-d3d.sh` 复制固定树后打补丁，`build-dxvk-validation.sh` 叠加第 4 个补丁。
- `package-d3d-quality-validation.py` 以已验证的 DXVK xrg4、vkd3d xrg1 包为底，换入新 DLL 和补丁，并核对源码固定、补丁哈希和构建记录。

| 项目 | 值 |
|---|---|
| DXVK `11.0-a676404-arm64ec-xrg5` | 包 `6f1f27c22966444cd519418d3e602990ba8464a4005ee4cb6ce797bf8122c181`，`d3d11.dll` `9a4b59aa…0400` |
| vkd3d-proton `11.0-212991f-arm64ec-xrg2` | 包 `039befb47140184847b0accf306a984239f3acf0ba505517907208d94aaba599`，`d3d12core.dll` `669c8ecd…7544`，`d3d12.dll` `29588988…85f1` |
| APK | `c097efdafe81b8ad3a845efb91ad36675f7065099e38068cbb6246b69fa71ff2`，audit 131 个文件 0 错误；bundle manifest `53413ef9…eae6eb` |
| 单测 | `XrGameQualitySettingsTest` 2/2、`XrGameMemoryBudgetTest` 3/3、`XrGameRuntimeTest` 5/5、`WindowsVrUpscaleTest` 11/11 |

- 覆盖安装后，app 在下次启动时从 APK 装入 xrg5 / xrg2。MHR 前缀 `system32` 里的 `d3d12core.dll`、`d3d11.dll` 与包内一致。
- 设置界面：启动面板主列表的「纹理质量」写 `XRGAME_TEXTURE_QUALITY`，高级里的「阴影渲染缩放」写 `XRGAME_RENDER_SCALE`，默认值不写入容器。这一轮用容器变量驱动，没有在头显里点界面。

复测：

- **MHR，低 + 50%**：
  - vkd3d 日志与开发版一致：同样 4 + 2 张阴影数组，纹理从第 1 张开始计数。
  - 标题稳定后 GPU 1057 + 1054 ≈ 2.06 GiB。
  - 进标题时 LMK 杀进程 0，没有任何「跳过」或告警。
- **Alyx，低 + 50%**：
  - 第 2 版规则下没有缩放任何深度图，也没有绑定告警。
  - 纹理到场景时 256 张，估算省 793 MiB。
  - 进场景约 2 分钟时：GPU 221 + 2511 ≈ 2.67 GiB（默认档 3.63 GiB），驻留 2.46 GiB，换出 0.64 GiB，整机可用 2.04 GiB。
  - 画面正常（`run100-d3dq/alyx-low-50-follow2/scene-low-50-xrg5.jpg`）。
  - 这一轮的 2 秒采样被上一轮脚本收尾的 `pkill` 一并停掉，上面是直接读的单点值。

过程中又出现两次云存档问题，都已单独提出：

- 启动时再次假冲突：app 记录的变更号停在 14。
- 按用户选择「保留本地」后，上传全部返回 `DuplicateRequest`，结果是 `UpdateFail`；随后按「保留本地存档并启动」继续。本地存档与 18:30 的备份（`alyx/save-backup-1830/`）一致。
- 退出 Alyx 时快捷菜单再次卡住，游戏留在暂停状态，没有强制结束，以免再触发假冲突。

## 结论

- vkd3d（xrg2）：「低 + 50%」让 MHR 标题时的 GPU 内存从约 2.9 GiB 降到约 2.1 GiB。进标题时没有 LMK 连杀，也没有设备丢失。开发版和打包版结果一致。
- DXVK（xrg5）：纹理质量「低」让 Alyx 场景内的 GPU 内存从约 3.6 GiB 降到约 2.7 GiB，渲染正常。方形深度图缩放会误伤，第 2 版只缩放深度数组。
- 待做：
  - MHR 进村庄后同一位置开关对比画面；
  - 在头显里通过启动面板切换档位，做端到端验证；
  - 「中」档的实测；
  - D3D9 / D3D8 暂未处理。

## 证据

证据目录（不进仓库）：`swan-20261008/swanxr-ahb/run99-texquality/`

- `a-default/`、`b-low-50/`、`a2-default/`：各轮的 `memmon*.txt`、`logcat*.txt`、`marks.txt`，以及容器备份 `container-*.json`。
- `alyx/b-low-50/`：Alyx 那一轮的记录。
- `vkd3d/`、`dxvk/`、`v2/`：两版 DLL 和对应补丁。
- `alyx/save-backup-1810/`、`alyx/save-backup-1830/`：处理冲突前的 Alyx 本地存档，带 SHA-256 清单。
- `../run100-d3dq/`：
  - APK、audit 报告、组件目录 `catalog.json`、构建日志；
  - `mhr-low-50/`、`alyx-low-50*/`：打包后的两轮复测。
