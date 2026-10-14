# D3D 转换层的渲染缩放与纹理质量 v1

状态（2026-10-13）：

- vkd3d-proton（D3D12）和 DXVK（D3D11）同步实现：用户当天要求 DXVK 一并处理。
- 纹理质量（P1）：vkd3d 和 DXVK 都已在 Swan 上完成首轮验证，见[纹理质量记录](../validation/swan-d3d-texture-quality-20261013.md)。
- 渲染缩放：vkd3d 按 shadPS4 的做法重做（P3），组件 vkd3d xrg3，MHR 首轮验证见[渲染缩放记录](../validation/swan-d3d-render-scale-20261013.md)。DXVK 的渲染缩放（P4）已实现并打包为 DXVK xrg6（2026-10-14），Alyx 上修掉了 push data 引起的管线创建崩溃；画面还没有在 2D 的 D3D11 游戏上验证，见[渲染缩放记录](../validation/swan-d3d-render-scale-20261013.md)的 DXVK 一节。
- 画面对比和「中」档实测还没做。

分支：主仓 `feature/malos/d3d-render-scale-texture-quality`。转换层改动以补丁形式维护在 `tools/xrgame/patches/`：

- `vkd3d-xrgame-resource-scale.patch`，基线 `212991f`；
- `dxil-spirv-xrgame-render-scale.patch`，vkd3d 固定的 dxil-spirv 子项目 `219d770`。dxil-spirv 是 vkd3d 的嵌套依赖，不另开 fork；
- `dxvk-xrgame-resource-scale.patch`，基线 `a6764047`，叠在现有 3 个 DXVK 补丁之上。

纹理质量和阴影缩放那一版已提交到 `tencentmalos/vkd3d-proton`（`8465f916`）、`tencentmalos/dxvk`（`f2b86aba`）的 `feature/malos/xrgame-resource-scale` 分支。渲染缩放的改动目前只在补丁和 vkd3d 工作区里，未提交。

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

### 2.2 渲染缩放（与 shadPS4 一致）

2026-10-13 用户指出：最初实现的「只缩放阴影深度数组」不是要的东西。要的是 shadPS4 的 render scale：游戏的渲染目标整体按比例缩小，shader 里修正坐标，游戏看到的资源尺寸不变。下面按 shadPS4 的做法对应到 D3D。

**shadPS4 的做法（`emulations/shadps4`，`internal_scale.h`、`image.cpp`、`vk_rasterizer.cpp`、SPIR-V 后端）：**

- 档位以 1/8 为单位：8/6/4/3/2，即 100% / 75% / 50% / 37.5% / 25%。整局固定，改了要重启游戏。
- 缩放对象：渲染目标、深度目标和最终输出图，短边 > 64。只采样的纹理归纹理质量处理。
- 以下情况图回到原尺寸：被当作存储图像写入（计算着色器的普通整图写入除外）、原子操作、多重采样，或者与不缩放的附件混在一个 pass。
- 绘制：
  - 视口 x/y/宽/高乘缩放系数；
  - 裁剪左上取下整、右下取上整；
  - 渲染区域取附件实际尺寸；
  - depth bias 和线宽不补偿。
- shader：
  - `FragCoord.xy` 乘 `1/s`，让游戏看到原坐标；
  - 整数坐标的 `texelFetch` 和存储图像读写映射为 `floor((c + 0.5) × 实际尺寸 / 原尺寸)`；
  - 尺寸查询返回原尺寸；
  - 归一化坐标的采样不需要改。
  - 每个绑定是否缩放由 push constant 里的位表给出。
- 读回时先放大到原尺寸再给游戏（有插值误差）。最终输出图在呈现时用 FSR / SGSR 放大到交换链。

**D3D 侧的对应：**

| 项目 | 做法 |
|---|---|
| 档位 | `XRGAME_RENDER_SCALE=100\|75\|50\|37.5\|25`，默认 100，进程启动时读一次 |
| 缩放对象 | 带 RT 或 DS 标志、2D、单采样、短边 > 64 的纹理，交换链后备缓冲也在内。不缩放：多重采样、CPU 可见堆、共享、reserved 资源。带 UAV 标志的渲染目标也缩放，计算着色器写入走坐标映射 |
| 决定时机 | 创建时按资源描述决定，之后不变。D3D12 的描述符堆按索引访问，资源无法在创建后换掉后备图像，所以没有 shadPS4 那样的「升回原尺寸」 |
| 视口 / 裁剪 | 当前绑定的 RTV 或 DSV 是缩放图时，按比例缩放。缩放图与原尺寸图混绑时记录一次日志，按较小尺寸绘制 |
| 清除 | RTV、DSV、UAV 的清除矩形按比例缩放 |
| 拷贝 | 两边同比例：盒子按比例缩放后直接拷。比例不同：彩色图用 blit 缩放；深度图格式不同无法 blit 时记录并跳过 |
| CPU 读回 | 缩放图与缓冲之间的拷贝目前跳过并记一次日志。先 blit 到原尺寸临时图再拷的做法还没做 |
| 呈现 | 后备缓冲也缩放，呈现时用线性过滤拉伸到 Vulkan 交换链图像（blit 或全屏绘制两条路径都按实际尺寸）。不缩放后备缓冲时，MHR 最后用点采样把缩放后的场景拷进原尺寸后备缓冲，出现 2×2 块状，见验证记录 |
| shader（vkd3d：dxil-spirv 补丁） | 纹理 SRV / UAV 描述符写入时，在该槽位的 raw VA 辅助数据里放标记：低 32 位为应用视图顶层 mip 的宽 \| 高 << 16，高 32 位为 `0x585247 << 8 \| 档位`；不缩放的写 0。单个描述符拷贝靠 `RAW_VA_AUX_BUFFER` 标志带上这个槽位，批量拷贝本来就整段拷。dxil-spirv 在 2D 纹理句柄处记下堆索引，`Load` / `Store` 时读标记映射坐标（偏移先加到坐标上），`GetDimensions` 返回应用尺寸。`SV_Position.xy` 乘 `8 / 当前 pass 档位`，档位放在根签名 push constant 里描述符表偏移之后的一个字，随表偏移推送；push constant 已满的根签名不加这个字，记一次日志。档位计入 shader 接口键 |
| shader（DXVK：IR 下降 pass） | 每个阶段的 push data 里放一个 SRV/UAV 槽位的缩放位表和当前 pass 缩放，在 `DxvkIrLowerBindingModelPass` 里改写同样几类指令 |

**已知误差：**

- 坐标映射在像素中心上有半像素级偏差，与 shadPS4 一致。
- TAA 邻域、SSR 步长、Hi-Z 链、按像素偏移的模糊等精确到像素的效果，画质会下降。
- 计算着色器的派发数不缩小：每个实际像素会被约 1/s² 个线程写入，结果正确但没有省下计算量。
- slope-scaled depth bias 在低分辨率下偏大，线宽不补偿。
- 像素着色器向不缩放的 UAV 写入，会出现空洞。
- 界面也按缩小后的后备缓冲渲染，文字发糊，与 shadPS4 相同。
- 呈现只做线性拉伸，还没接 FSR / SGSR。
- 图像原子操作、光线追踪和 work graph 的 shader 没有做坐标映射。
- 诊断用环境变量 `XRGAME_RENDER_SCALE_DEBUG`（位掩码）：1 不改 shader，2 不改 `SV_Position`，4 不映射纹素坐标，8 不改尺寸查询。也计入 shader 接口键。

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
| `XRGAME_RENDER_SCALE` | `100` / `75` / `50` / `37.5` / `25` | `100` |

- vkd3d 和 DXVK 在进程启动时读取这两个变量，重启游戏后生效（与 shadPS4 一致）。默认值不写入容器。
- 启动面板：「渲染缩放」和「纹理质量」都放在主列表，与 shadPS4 一致。实现见 `XrGameQualitySettings`。VR 游戏不显示「渲染缩放」，它们的眼睛分辨率由「VR 渲染分辨率」决定。
- 统计日志：
  - 启动时各打一行当前档位。
  - 被缩放的渲染目标在第 1 张和之后每 64 张时打一行累计数和累计节省量。
  - 纹理在第 1 张和之后每 64 张时打一行累计数和累计节省量。
  - vkd3d 用自己的 INFO 级日志，能进 logcat。
  - DXVK 在容器里默认 `DXVK_LOG_LEVEL=none`，验证时要临时改成 `info`。

## 4. 阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 规格、设置管线、vkd3d 选项解析和统计日志 | 日志能看到选项和计数 |
| P1 | vkd3d 纹理质量 | MHR 标题和村庄：画面无明显错误；KGSL 内存下降与统计一致；无 GPU 错误 |
| P2（已撤回） | vkd3d 只缩放阴影深度数组 | 已验证，但不符合需求，被 P3 取代 |
| P3 | vkd3d 渲染缩放：缩放全部渲染目标，视口、清除、拷贝和读回处理，dxil-spirv 补丁修正 shader | MHR 50% / 75%：画面无明显错位，无 GPU 错误；KGSL 内存下降与统计一致。首轮通过（标题、存档选择界面），狩猎场景未验 |
| P4 | DXVK 纹理质量（已完成）和渲染缩放（xrg6 已实现，Alyx 可进场景） | 需要一款 2D 的 D3D11 游戏验证画面；Alyx 是 VR 游戏，面板不显示渲染缩放 |

## 5. 测量方法

- 内存：每 2 秒采样 MHR 的 KGSL mapped/unmapped、RSS 和 swap，以及整机可用内存、dmabuf 页池、PSI。脚本为 `mhr-memmon.sh`，在开关两种状态下各跑一遍，同样跑到标题画面。
- 资源构成：用 vkd3d 自己的统计日志。在内存紧张的场景，不再用 GFXR 截取，因为截取的快照会让 Swan 重启。
- 画面：头显截图，同一位置开关对比。

## 6. 构建与部署

- 补丁接入构建：
  - `build-d3d.sh` 把 vkd3d 固定树复制一份，打 vkd3d 补丁，再给 `subprojects/dxil-spirv` 打 dxil-spirv 补丁，编 arm64ec。构建目录名取两个补丁合起来的摘要；
  - `build-dxvk-validation.sh` 把 DXVK 补丁作为第 4 个补丁，也计入输入哈希；
  - 组件版本号递增。
- 开发期替换设备上已安装组件里的 `d3d12.dll` / `d3d12core.dll`，并同步更新组件记录里的 SHA，与之前 FEX 补丁的做法相同。
