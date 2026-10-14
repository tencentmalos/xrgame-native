# Swan：移除 D3D 渲染缩放，启动面板去掉插帧、加上限帧（2026-10-14）

## 1. 决定

2026-10-14 用户决定移除渲染缩放：它不适合 Steam 游戏，移除可以降低实现复杂度。范围包括最初的阴影缩放，它和渲染缩放共用 `XRGAME_RENDER_SCALE`。纹理质量保留。

实现保留在以下分支，不再维护：

| 仓库 | 分支 | 提交 |
|---|---|---|
| 主仓 | `feature/malos/d3d-render-scale` | `4d5f0f484`（移除前的 `malos/main`） |
| `tencentmalos/vkd3d-proton` | `feature/malos/xrgame-resource-scale` | `0036714d`（渲染缩放），`8465f916`（纹理质量 + 阴影缩放） |
| `tencentmalos/dxvk` | `feature/malos/xrgame-resource-scale` | `2969a39e`（渲染缩放），`f2b86aba`（纹理质量 + 阴影缩放） |

dxil-spirv 补丁只在主仓分支的 `tools/xrgame/patches/` 里，从来没有单独 fork。

同一天用户还要求：启动面板去掉插帧相关选项，改加限帧（沿用原有的配置值和机制）；VR 渲染分辨率选中后显示具体尺寸。

## 2. 转换层：只留纹理质量

- 两个 fork 新开 `feature/malos/xrgame-texture-quality`，各为固定基线上的单个提交：
  - vkd3d-proton `75526125`（基线 `212991fc`，+288/−3）；
  - DXVK `0258c042`（基线 `a6764047`，+200/−7）。
- 做法：从「纹理质量 + 阴影缩放」那一版（`8465f916` / `f2b86aba`）去掉阴影缩放，包括：
  - 深度数组判定；
  - `xrgame_scale_percent` / `m_xrgameScale`；
  - 视口和裁剪缩放、清除矩形缩放、拷贝坐标缩放；
  - DXVK 的 `ValidateRenderTargets` 和初始数据丢弃。
- 视口代码与基线完全一致。核对过：后面的渲染缩放提交没有改动纹理质量的逻辑，只是改名并新增渲染缩放自己的辅助函数。
- 主仓补丁改名为 `vkd3d-xrgame-texture-quality.patch`（`796197a5…`）和 `dxvk-xrgame-texture-quality.patch`（`4a36776e…`），与两个 fork 提交逐字相同；删除 `dxil-spirv-xrgame-render-scale.patch`。
- `build-d3d.sh` 不再给 dxil-spirv 打补丁。`build-dxvk-validation.sh`、`package-components.py`、`package-d3d-quality-validation.py` 改用新补丁和新版本号。
- 组件：DXVK `11.0-a676404-arm64ec-xrg7`、vkd3d-proton `11.0-212991f-arm64ec-xrg4`（`XrGameRuntimeVersions`）。
- App：
  - `XrGameQualitySettings` 只剩纹理质量；
  - 启动面板去掉「渲染缩放」和对应字符串；
  - 旧容器里留下的 `XRGAME_RENDER_SCALE` 不再被读取，不影响运行。

### 构建

在远端构建机的 `xrgame-runtime-build` 容器里，用 `build-d3d.sh` 从全新复制的固定树重编，0 个编译错误。旧的 `project`、`output` 留存为 `*.rs6`。

| 文件 | SHA-256 |
|---|---|
| `d3d11.dll` | `b3b9377d924f984cc36fd49796d5069d57a68511926929d7b9bd9916f5b3c274` |
| `dxgi.dll` | `23042047470c888a69b6e606083138f8ddab0ffbf7a459961e7b23ee398e113a` |
| `d3d10core.dll` | `52b41aefc004dc1db92ca231a9f66763890b94d528850d75bf34e50f660908fa` |
| `d3d9.dll` | `8de156e78ff0a5880ebbfd3ea2cff52d20b32e245bc49c59355c3054f9fe8ebf` |
| `d3d8.dll` | `83fbb82c03e2b39320517847a4bc4cdd97eae2157c813e10370b07343436eafe` |
| `d3d12.dll` | `f9a7c402ef20d29d9fd6c71c868b27b21d2a4c5f93644f1e48d9ede0906e9c17` |
| `d3d12core.dll` | `7326138125fd3db6769f80d0b4d9d10316dd033bf515ea20ed7b51dc2c7803b8` |
| `dxvk-11.0-a676404-arm64ec-xrg7.wcp` | `585baa3d8cb4e3e431ffd1b9d16929d933ac9fa194252fe16b509e8b717ff5f3` |
| `vkd3d-proton-11.0-212991f-arm64ec-xrg4.wcp` | `78ca82e93f1d278e9ba6af93d5da552ed092a479c9738c528dcb13f23ed76acc` |
| bundle `manifest.json`（以 `packages-rs6` 为底换入两包） | `9765fd36d8ccac96827356af29db73683730cc5f75c35619249a96d6b80632cc` |

DXVK 构建记录里的 `qualityPatchSha256` 是新补丁的 SHA。两包以已验证的 DXVK xrg4、vkd3d xrg1 私有存档为底打包，属于私有验证组件，不是发布产物。

## 3. 启动面板

- **插帧**：去掉「插帧」和高级里的「插帧光流分辨率」两行及其字符串。功能本身仍可以通过容器的 `XRGAME_XR_FRAMEGEN` 或 DebugBus `vr_upscale fg=` 打开。
- **限帧**：主列表新增「限帧」，所有显示模式都显示。
  - 读写的是快捷菜单 FPS 限制器的容器 extra `fpsLimiterEnabled` / `fpsLimiterTarget`，默认开启、60。`XServerScreen` 的键名和读取函数由 `private` 改为 `internal`，生效机制不变：游戏启动时由 `XServerScreen` 读取。
  - 选项为「无限制 / 30 / 36 / 45 / 60 / 72 / 90 FPS」。已保存的目标值不在列表里时（例如快捷菜单里调出的 40），作为额外一项保留。
  - 按「启动」时值有变化才写回。
- **VR 渲染分辨率**：
  - 选项下方显示单眼尺寸。计算用 `XrResolutionRecommendation.eyeSize`，基于上次 XR 会话记录的眼尺寸，与 Windows VR 运行时交给游戏的尺寸一致。
  - 用户指出 100% 只是推荐尺寸，到不了 Swan 的上限，于是增加了「高」和「极高」两档。
    - 内部存为 150 和 200：100 以上时，每个轴从推荐尺寸线性走向运行时允许的最大眼尺寸，与「XR 输出分辨率」的推荐/高/极高一致。
    - `getViews` 在会话中读取 `nativeXrDisplayInfo` 的最大尺寸，读不到时退回 `PrefManager.xrDisplayInfo`；两者都没有时停在推荐尺寸。
    - Windows 运行时声明的 `maxSwapchainImageWidth/Height` 由 4096 改为 16384，与 `xrCreateSwapchain` 实际接受的上限一致。OpenComposite 只用推荐尺寸，不看这个值。
  - Swan 当前推荐 2592×2400，最大 4160×3552。各档单眼尺寸：

    | 档位 | 单眼 |
    |---|---|
    | 推荐 | 1296×1200（即 50%） |
    | 50% | 1296×1200 |
    | 75% | 1944×1800 |
    | 100% | 2592×2400 |
    | 高 | 3376×2976 |
    | 极高 | 4160×3552 |

  - 这些数值由单测覆盖。
  - 高和极高两档会显著增加 GPU 和显存开销，Swan 上的帧率还没有实测。
- **纹理质量说明**：用户指出原说明不对。
  - 原说明是「去掉大于 1024（中）或 512（低）像素的纹理层级」：两档写在同一句里，读起来像是删掉大纹理，也没写影响范围。
  - 现在按所选档位显示一句：贴图最高只加载到长边 1024 或 512 像素，更大的 mipmap 层级不加载；只影响 D3D11/D3D12 游戏，渲染目标和没有 mipmap 的贴图不受影响。「高」不显示说明。
  - 上限值来自 `XrGameQualitySettings.textureCap`，与转换层的阈值一致，有单测覆盖。

## 4. Swan 验证

设备：Swan（`B3110`），`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `6d4335a5-667c-426d-815b-1a82edaa0fbb`。两个容器都设了 `XRGAME_TEXTURE_QUALITY=low`，都没有 `XRGAME_RENDER_SCALE`。证据在仓库外 `xrgame-native-evidence/swan-20261014-texq/`。

| APK（SHA-256） | 内容 |
|---|---|
| `c53fb5331080d2257c985bbe78397bb31686ecdd0ee6c23eaf20aacdbfa103e6` | 移除渲染缩放、新组件；MHR 两轮 |
| `b7ef38f1527ac2bee07e0e37f45f9eb19a0407f629ba9afe83bdf24320decf77` | 加上启动面板改动（去掉插帧、加上限帧）；Alyx 一轮、面板截图 |
| `2f5add435330f718f4d4124c2b1b33c49f65a4fdaa7b2d602efeedf7b1d3a354` | 加上 VR 渲染分辨率的尺寸说明；面板截图 |
| `24e0224629811538fade6e369551f85d08c6bba715612f30817ffa0d6e72e508` | 加上 VR 渲染分辨率的「高」「极高」，运行时交换链上限改为 16384；只做了单测，未上机 |
| `ce0b641111d2c8c9953d13eae20a0c66cfebcfb55949fa0793729f4e053b897d` | 加上新的纹理质量说明；单测通过，上机截了面板（见下） |

这些 APK 用的都是同一个 bundle（`9765fd36`）。

### MHR（vkd3d xrg4，2D 大屏）

**第 1 轮**（app PID 29771，16:48:57–16:51:29）：
- vkd3d 打印 `XRGame texture quality low.`，不再有渲染缩放的字样。之后打印 `XRGame texture quality: 64 textures reduced, 39.5 MiB saved`。
- 按键按早了：当时还在加载插画（"Compiling shaders"）。大屏模式下 PowerMetrics 报的是 XR 的 72 Hz，`mhr-to-title.sh` 把它当成了标题画面的帧率。所以只到了第二个联机错误对话框（MR-111-81），没有进 3D 场景。
- 对话框画面上：KGSL mapped 1052 MiB + unmapped 1842 MiB，合计约 2894 MiB，57.2 fps。
- 快捷菜单退出正常，云同步 `11 -> 11`，日志里没有 GPU 故障或设备丢失。

**第 2 轮**（紧接着启动，16:52:17）：
- 加载中途，Pico lmkd 先连续杀掉十余个系统和后台进程，16:53:57 记录 `[xrprocessmanager] POLICY_SWAP_THEN_KILL pid:29771 uid:10155 adj:0`，16:53:59 app 进程以 signal 9 退出。
- 这与 10-13 的记录一致：MHR 的显存峰值主要来自两张 2048×2048×32 的阴影深度数组（约 1 GiB）。当时「低纹理 + 50% 阴影缩放」把标题时的 GPU 内存从 2.9 GiB 降到 2.1 GiB，进标题不再出现 LMK 连杀。
- 移除缩放后，纹理质量在 MHR 这里只省几十 MiB，标题前后又回到约 2.9 GiB。在系统刚从上一轮恢复、可用内存偏少时会被杀。
- 只观察到 1 次，没有重复测。

### Alyx（DXVK xrg7，VR）

app PID 9458，hlvr PID 11759，17:00:25–17:02:32，无人值守读档进场景。

- 场景内 hlvr 的 KGSL 合计 2762–2774 MiB（6 次采样）。10-13 用 xrg5 测的是「低」档约 2.7 GiB、「高」档约 3.6 GiB，可见 DXVK 的纹理质量仍然生效。
- prefix 的 `system32/d3d11.dll` 是 `b3b9377d…`，`dxgi.dll` 是 `23042047…`，都是 xrg7 的构建产物。
- 快捷菜单退出正常。

### 启动面板

截图用 capture SDK 截 SteamPSP 的 2D 窗口。

- **MHR（普通游戏，XR 大屏）**：
  - 主列表有「限帧 60 FPS」和说明文字。该容器存的是 `fpsLimiterEnabled=true`、`fpsLimiterTarget=60`。
  - 没有「插帧」，也没有「渲染缩放」。
- **Alyx（VR）**：
  - 「限帧」显示 60 FPS。该容器没有存过这两个 extra，按默认值显示。
  - 「VR 渲染分辨率：推荐」下方显示「单眼 1296x1200」，即运行时推荐的 2592×2400 的 50%。
- **`ce0b6411`，Alyx**：
  - 纹理质量「低」下方显示新说明（长边 512 像素等）。
  - VR 渲染分辨率的下拉框为 推荐 / 50% / 75% / 100% / 高 / 极高。「推荐」显示单眼 1296x1200，在草稿里选「极高」显示单眼 4160x3552。
  - 随后取消面板，容器仍是 `xrRenderScaleMode=recommended`。
  - 「高」「极高」交给游戏后的实际渲染尺寸和帧率没有上机测。
- 测试期间头显里有人在操作 SteamPSP：出现过一个不是脚本打开的 Alyx 面板，上面选着 36 FPS。所以没有在设备上做「面板选 30 → 启动 → 读回容器 extra → 改回」的往返测试，以免继续干扰。

## 5. 未覆盖

- 限帧在设备上的写入往返（见上）。读取经过验证；写入沿用 `XServerScreen.persistFpsLimiterState` 同样的 `putExtra` + `saveData`。
- MHR 进 3D 场景后的画面和显存（本轮没到）；纹理「中」档；2D 的 D3D11 游戏。
- 移除缩放后 MHR 的 LMK 风险：只观察到 1 次。是否要为内存保留一个只缩深度数组的措施，留给用户决定。
