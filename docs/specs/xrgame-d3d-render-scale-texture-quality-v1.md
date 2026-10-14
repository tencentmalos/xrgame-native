# D3D 转换层的纹理质量 v1（渲染缩放已移除）

状态（2026-10-14）：

- 只保留纹理质量。vkd3d-proton（D3D12）和 DXVK（D3D11）都已在 Swan 上完成首轮验证，见[纹理质量记录](../validation/swan-d3d-texture-quality-20261013.md)。
- 渲染缩放（包括最初的阴影缩放）已于 2026-10-14 按用户要求移除，见第 2.2 节和[移除记录](../validation/swan-d3d-render-scale-removal-20261014.md)。组件为 vkd3d xrg4、DXVK xrg7。
- 画面对比和「中」档实测还没做。

转换层改动以补丁形式维护在 `tools/xrgame/patches/`：

- `vkd3d-xrgame-texture-quality.patch`，基线 `212991f`；
- `dxvk-xrgame-texture-quality.patch`，基线 `a6764047`，叠在现有 3 个 DXVK 补丁之上。

补丁与 `tencentmalos/vkd3d-proton`（`75526125`）、`tencentmalos/dxvk`（`0258c042`）的 `feature/malos/xrgame-texture-quality` 分支一致，各为基线之上的单个提交。

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

参照 shadPS4 的「纹理质量（高/中/低）」，在转换层里缩小资源的 Vulkan 后备图像，游戏看到的 D3D12 资源描述保持不变。

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
- 要修正需要给每个 SRV 传元数据，vkd3d 目前没有这个通道；描述符缓冲模式下连辅助缓冲都没有。暂不处理。

### 2.2 渲染缩放（已移除）

2026-10-14 用户决定移除：渲染缩放不适合 Steam 游戏，移除可以降低实现复杂度。

- 实现保留在主仓分支 `feature/malos/d3d-render-scale`（`4d5f0f484`）。该分支上的本文件第 2.2 节有完整设计和已知误差，验证见[渲染缩放记录](../validation/swan-d3d-render-scale-20261013.md)。
- 转换层的代码保留在 `tencentmalos/vkd3d-proton`（`0036714d`）和 `tencentmalos/dxvk`（`2969a39e`）的 `feature/malos/xrgame-resource-scale` 分支，dxil-spirv 补丁在上述主仓分支的 `tools/xrgame/patches/` 里。
- 这些分支不再维护。

### 2.3 不在 v1 范围

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
| `InitDeviceLocalTexture` | 初始数据跳过被丢的层级 |
| `CopyImage` | `CopySubresourceRegion`、`CopyResource` 都汇到这里。图像一侧的 mip 编号映射，staging 缓冲一侧保持 D3D11 编号 |
| `UpdateImage` | 跳过被丢的层级 |
| SRV | mip 范围同 vkd3d |
| `ResolveSubresource` | 任一方被缩小时跳过并记录一次日志 |

D3D11 没有资源级 min LOD clamp，不需要映射。

## 3. 设置

| 容器环境变量 | 取值 | 默认 |
|---|---|---|
| `XRGAME_TEXTURE_QUALITY` | `high` / `medium` / `low` | `high` |

- vkd3d 和 DXVK 在进程启动时读取这个变量，重启游戏后生效（与 shadPS4 一致）。默认值不写入容器。
- 启动面板：「纹理质量」放在主列表。实现见 `XrGameQualitySettings`。
- 旧容器里留下的 `XRGAME_RENDER_SCALE` 不再被读取，不影响运行。
- 统计日志：
  - 启动时打一行当前档位。
  - 纹理在第 1 张和之后每 64 张时打一行累计数和累计节省量。
  - vkd3d 用自己的 INFO 级日志，能进 logcat。
  - DXVK 在容器里默认 `DXVK_LOG_LEVEL=none`，验证时要临时改成 `info`。

## 4. 阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 规格、设置管线、vkd3d 选项解析和统计日志 | 日志能看到选项和计数 |
| P1 | vkd3d 纹理质量 | MHR 标题和村庄：画面无明显错误；KGSL 内存下降与统计一致；无 GPU 错误 |
| P2（已撤回） | vkd3d 只缩放阴影深度数组 | 已验证，但不符合需求，被 P3 取代 |
| P3（已移除） | vkd3d 渲染缩放 | 2026-10-14 移除，见第 2.2 节 |
| P4 | DXVK 纹理质量（已完成）；渲染缩放已移除 | — |

## 5. 测量方法

- 内存：每 2 秒采样 MHR 的 KGSL mapped/unmapped、RSS 和 swap，以及整机可用内存、dmabuf 页池、PSI。脚本为 `mhr-memmon.sh`，在开关两种状态下各跑一遍，同样跑到标题画面。
- 资源构成：用 vkd3d 自己的统计日志。在内存紧张的场景，不再用 GFXR 截取，因为截取的快照会让 Swan 重启。
- 画面：头显截图，同一位置开关对比。

## 6. 构建与部署

- 补丁接入构建：
  - `build-d3d.sh` 把 vkd3d 固定树复制一份，打 vkd3d 补丁，编 arm64ec。构建目录名取补丁的摘要；
  - `build-dxvk-validation.sh` 把 DXVK 补丁作为第 4 个补丁，也计入输入哈希；
  - 组件版本号递增。
- 开发期替换设备上已安装组件里的 `d3d12.dll` / `d3d12core.dll`，并同步更新组件记录里的 SHA，与之前 FEX 补丁的做法相同。
