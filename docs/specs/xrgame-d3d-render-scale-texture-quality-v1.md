# D3D 转换层的渲染缩放与纹理质量 v1

状态（2026-10-13）：

- vkd3d-proton（D3D12）和 DXVK（D3D11）同步实现：用户当天要求 DXVK 一并处理。
- P1、P2、P4 已在 Swan 上完成首轮验证，组件为 vkd3d xrg2 和 DXVK xrg5，见[验证记录](../validation/swan-d3d-texture-quality-20261013.md)。
- 画面对比和「中」档实测还没做。
分支：主仓 `feature/malos/d3d-render-scale-texture-quality`。转换层改动以补丁形式维护在 `tools/xrgame/patches/`：

- `vkd3d-xrgame-resource-scale.patch`，基线 `212991f`；
- `dxvk-xrgame-resource-scale.patch`，基线 `a6764047`，叠在现有 3 个 DXVK 补丁之上。

两个补丁同时提交到 `tencentmalos/vkd3d-proton`（`8465f916`）、`tencentmalos/dxvk`（`f2b86aba`）的 `feature/malos/xrgame-resource-scale` 分支。

## 1. 背景与目标

Swan 只有约 15 GB 内存，Pico 系统常驻就占掉约 7 GB（见 [dmabuf 页池记录](../validation/swan-dmabuf-pool-20261013.md)）。MHR 进标题时的内存峰值会触发：

- LMK 连续杀进程；
- HyperHold 把我们的 GPU 内存换出；
- 一次合成设备丢失；
- 一次整机重启（在叠加了 GFXR 截取的情况下）。

限制报告给游戏的显存（`dxgi.maxDeviceMemory`）对 MHR 无效，见 [显存上限记录](../validation/swan-mhr-vram-cap-20261013.md)。

用 GFXR 抓取 MHR 标题画面（游戏第 4150 帧）的 D3D12 状态后，按资源描述估算存活资源，合计约 2.07 GiB：

| 类别 | 大小 |
|---|---|
| 两张阴影深度数组（2048×2048×32，R32） | 1024 MiB |
| 采样纹理（BC7 / BC1 / BC4 / 未压缩） | 340 MiB |
| 上传缓冲 | 233 MiB |
| 渲染目标（内部 1080p） | 191 MiB |
| 其他缓冲 | 210 MiB |

参照 shadPS4 的「渲染缩放」和「纹理质量（高/中/低）」，在转换层里缩小资源的 Vulkan 后备图像，游戏看到的 D3D12 资源描述保持不变。

## 2. 方案

### 2.1 纹理质量：只丢 mip，不重新编码

**适用范围：**

- DEFAULT 堆里的 2D 纹理，committed 或 placed；
- 不带 RT、DS、UAV、SIMULTANEOUS_ACCESS 标志；
- 单采样，`MipLevels ≥ 2`；
- 短边达到阈值。

reserved（tiled）、shared、borrowed 资源和采样反馈图都不处理。

**档位：** 按 mip 尺寸上限判定，长边超过上限的层级不进 Vulkan 图像。

| 档位 | 上限（长边） | 例子 |
|---|---|---|
| 高 | 不限 | 原样 |
| 中 | 1024 | 2048 纹理丢 1 级，4096 丢 2 级 |
| 低 | 512 | 1024 纹理丢 1 级，2048 丢 2 级 |

- 不按「每张纹理丢几级」判定，是为了让不同纹理里同尺寸的层级要么都在、要么都不在。纹理流送常在不同大小的纹理之间按层级拷贝，这样两边总能对上。
- 至少保留 1 级：mip 不够的纹理只丢到剩 1 级。它保留的那一级在别的纹理里可能已被丢掉；从那种纹理往这一级拷贝时缺源数据，跳过并记录一次日志。
- shadPS4 的「中」是 0.75 倍重采样并转码 ASTC，「低」是丢 1 级 mip。这里 BC 对 BC、不转码，所以只能按 mip 粒度缩小。
- BC 格式保持原样：Turnip 原生支持 BCn，丢 mip 也不需要转码。

**做法：** VkImage 的尺寸为 `extent >> N`，mip 数为 `mipLevels - N`，D3D12 的 mip 编号对应到 Vulkan 的 `mip - N`。因为 `(W>>N)>>k == W>>(N+k)`，保留下来的 mip 尺寸与原资源完全相同。

**改动点（vkd3d-proton，`libs/vkd3d`）：**

1. `vkd3d_get_image_create_info`：同一份判断同时决定实际图像和 `GetResourceAllocationInfo`。它必须只依赖资源描述和配置，因为 placed 资源创建时会检查图像大小与堆是否匹配。
2. `struct d3d12_resource` 新增丢弃的 mip 数，供后续路径使用。
3. 子资源编号映射（`vk_image_subresource_from_d3d12` 一系）。
4. 上传与拷贝：
   - buffer → 纹理：落到被丢 mip 上的拷贝直接跳过，其余减去 N；
   - 纹理 → buffer、纹理 → 纹理、`CopyResource`：只拷两边都存在的 mip。读到被丢的 mip 时用 Vulkan mip 0 补，这种情况很少，记录一次日志。
5. Barrier：传统 barrier 和增强 barrier 都要重映射，跳过被丢的 mip。
6. SRV：
   - `base = max(0, MostDetailedMip - N)`；
   - 层数减去被丢的部分；
   - `ResourceMinLODClamp` 减 N；
   - `VK_REMAINING_MIP_LEVELS` 按 Vulkan 的 mip 数换算。

**已知误差（v1 接受）：**

- shader 用 `GetDimensions` 查询，或用 `Load` / `SampleLevel` 访问被丢的 mip 时，拿到的是缩小后的数据。普通材质纹理很少这样用。
- 要修正需要给每个 SRV 传元数据，vkd3d 目前没有这个通道；描述符缓冲模式下连辅助缓冲都没有。放到 P3。

### 2.2 渲染缩放 v1：阴影类深度图

**适用范围：**

- 带 DS 标志、单 mip 的 2D 纹理，数组层数 > 1，且短边 ≥ 1024。
- 不处理：
  - 单张方形深度图：最初把边长 ≥ 2048 的也算进来，但 Alyx 的 4096² 和 3072² 深度图会和渲染目标绑在一起，见[验证记录](../validation/swan-d3d-texture-quality-20261013.md)；
  - 与交换链同尺寸的主深度；
  - 带 RT 标志的资源；
  - 多重采样。

**档位：** 100% / 75% / 50%，默认 100%。

**做法：** 图像尺寸按比例缩放。绑定这种 DSV 时，当前绘制的视口、裁剪矩形和清除矩形按同样比例缩放：

- framebuffer 尺寸用缩放后的大小；
- 在 `update_dynamic_state` 里缩放 x、y、宽、高，渲染目标变化时把视口标记为需要重新提交。

**已知误差：**

- 着色器用 `Load` 加上游戏自己算的整数坐标读阴影图时会错位。用 `GetDimensions` 算出的坐标、归一化坐标的 `SampleCmp` / `Gather` 不受影响。
- slope-scaled depth bias 在半分辨率下会偏大。
- 如果这种深度图和未缩放的 RTV 一起绑定，说明判断失误：记录一次日志（含两边尺寸），按缩放后的尺寸绘制。DXVK 的 `ValidateRenderTargets` 用应用创建的尺寸比较，不会因此拒绝绑定。

### 2.3 不在 v1 范围

- **彩色渲染目标的通用缩放（P5）。** 后处理大量使用 `SV_Position` 和 `Load`，需要 dxil-spirv 修正 FragCoord，并给每个纹理传缩放信息，相当于 shadPS4 的完整做法。
  - 对 D3D 游戏，主渲染分辨率已经可以通过「游戏分辨率」加超分来降低；
  - 但 MHR 自己的内部分辨率是 1080p，要在游戏里调。
- D3D9 / D3D8（DXVK 的 `D3D9CommonTexture`）暂不处理。

### 2.4 DXVK（D3D11）

与 vkd3d 同一套判定和档位，只处理应用无法映射、共享或查询 tiling 的纹理：

- map mode 为 NONE；
- `CPUAccessFlags == 0`；
- 不是 tiled、shared、GDI、11on12、交换链或外部导入的图像；
- 没有 `GENERATE_MIPS`；
- 非多平面格式。

改动点：

| 位置 | 做法 |
|---|---|
| `D3D11CommonTexture` 构造 | 缩小图像尺寸和 mip 数 |
| `InitDeviceLocalTexture` | 初始数据跳过被丢的层级；缩放深度图丢弃初始数据并清零 |
| `CopyImage` | `CopySubresourceRegion`、`CopyResource` 都汇到这里。图像一侧的 mip、偏移、尺寸映射，staging 缓冲一侧保持 D3D11 编号 |
| `UpdateImage` | 跳过被丢的层级；缩放深度图不接受 CPU 上传 |
| SRV | mip 范围同 vkd3d |
| DSV、`BindFramebuffer`、`ApplyViewportState` | 绑定缩放深度图时，视口和裁剪矩形按比例缩放 |
| `ValidateRenderTargets` | 用应用创建的深度尺寸比较，避免误拒绝渲染目标绑定 |
| `ResolveSubresource` | 任一方被缩小时跳过并记录一次日志 |

D3D11 没有资源级 min LOD clamp，不需要映射。

## 3. 设置

| 容器环境变量 | 取值 | 默认 |
|---|---|---|
| `XRGAME_TEXTURE_QUALITY` | `high` / `medium` / `low` | `high` |
| `XRGAME_RENDER_SCALE` | `100` / `75` / `50` | `100` |

- vkd3d 和 DXVK 在进程启动时读取这两个变量，重启游戏后生效（与 shadPS4 一致）。默认值不写入容器。
- 启动面板：「纹理质量」放在主列表，「阴影渲染缩放」放在高级选项。实现见 `XrGameQualitySettings`。
- 统计日志：
  - 启动时各打一行当前档位。
  - 每张被缩放的深度图打一行，含节省量。
  - 纹理在第 1 张和之后每 64 张时打一行累计数和累计节省量。
  - vkd3d 用自己的 INFO 级日志，能进 logcat。
  - DXVK 在容器里默认 `DXVK_LOG_LEVEL=none`，验证时要临时改成 `info`。

## 4. 阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 规格、设置管线、vkd3d 选项解析和统计日志 | 日志能看到选项和计数 |
| P1 | vkd3d 纹理质量 | MHR 标题和村庄：画面无明显错误；KGSL 内存下降与统计一致；无 GPU 错误 |
| P2 | vkd3d 阴影深度图缩放 | MHR 阴影正常；标题峰值内存下降约 0.75 GiB（50% 时）；进标题不再连杀进程 |
| P3 | dxil-spirv 修正 `GetDimensions` / `Load` | 仅在 P1 出现可见错误时做 |
| P4 | DXVK 同等实现（与 P1、P2 同步） | 用 Alyx（D3D11）验证 |
| P5 | 彩色渲染目标通用缩放 | 另立规格 |

## 5. 测量方法

- 内存：每 2 秒采样 MHR 的 KGSL mapped/unmapped、RSS 和 swap，以及整机可用内存、dmabuf 页池、PSI。脚本为 `mhr-memmon.sh`，在开关两种状态下各跑一遍，同样跑到标题画面。
- 资源构成：用 vkd3d 自己的统计日志。在内存紧张的场景，不再用 GFXR 截取，因为截取的快照会让 Swan 重启。
- 画面：头显截图，同一位置开关对比。

## 6. 构建与部署

- 补丁接入构建：
  - `build-d3d.sh` 把 vkd3d 固定树复制一份再打补丁编 arm64ec；
  - `build-dxvk-validation.sh` 把 DXVK 补丁作为第 4 个补丁，也计入输入哈希；
  - 组件版本号递增。
- 开发期替换设备上已安装组件里的 `d3d12.dll` / `d3d12core.dll`，并同步更新组件记录里的 SHA，与之前 FEX 补丁的做法相同。
