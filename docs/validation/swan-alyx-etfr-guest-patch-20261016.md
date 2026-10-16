# Swan：Alyx 眼动注视点（ETFR）的 guest patch（2026-10-16）

背景：2026-10-15 晚用户把 Alyx 自带 FDM/ETFR 改走 guest patch（原生路径下佩戴时画面不对）。本记录是在 guest patch v1 框架上的第一轮实现，静态分析用 reverse_study MCP，每轮用 capture SDK 在无人佩戴时取游戏的双眼图。**真人佩戴下的效果没有验证。**

## 环境与证据

| 项 | 值 |
|---|---|
| 设备 | Swan（Pico B3110），`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `bf55d1cb-dcba-478b-b837-623dc3ad7d5b`，SELinux Enforcing |
| APK（gdbg6，picoXrDebug） | `a96ed239aed310cf2a3f39bae58f14d5784396ea9f0ac80259c2daa669cabe88` |
| 运行时 | Turnip `turnip-adb7e30-xrg16`，DXVK xrg7，VKD3D xrg4，Proton xrg7 |
| 游戏 | Half-Life: Alyx（546560），`-condebug -vulkan +vr_fdm 1`，`launch 546560 load=s0/autosave` 读档进 `a1_intro_world` 后测量；每轮都是重新启动 |
| 目标模块 | `renderingpipelines.dll` `f8d3f03beea8037748f762a3aa35ae447fafeb37056d313565f4a9d23be9e690`；`rendersystemvulkan.dll` `7be3494e0874bc989864461b9c60fbbcb9d6e25eefffd8d823cf55268916fa0c`（设备上的文件与分析用副本逐字节一致） |
| 补丁包 | `etfr_fdm_split` `0d71fac8…69c3`；`etfr_offset_eye` `b41728e0…3214`；`etfr_fdm_ab` `9468ad0b…687c`（构建用 NDK 27.3 clang，recipe 在 `tools/xrgame/guest-patch/games/546560/`） |
| 证据目录（私有） | `etfr-guest-patch-20261016/run1…run12`：每轮容器前后、双眼 PNG、guest patch 状态、`console.log`、状态层读数 |

容器在测试前备份，测试后按字节还原（`cmp` 相同）：`-condebug -vulkan`，`XRGAME_XR_FOVEATION=off`，没有选 guest patch。

## 静态分析（reverse_study）

Alyx 的 FDM 只经过这几处：

| 位置 | 作用 |
|---|---|
| `renderingpipelines+0x16C70` | 取（或创建）某个 RT 尺寸的 FDM 纹理 `foveated_fdm_texture_%d_%d_%d`，边长 = ceil(RT / 32)，按尺寸缓存。唯一调用点 `+0x1786C` 在 VR 管线构建函数 `+0x171C0` 里 |
| `renderingpipelines+0x5D60` | 生成图案：读 `fove_square`、`fove_square_use_legacy`，两者都非零走 `+0x5AF0`（旧方形），否则走 `+0x5450` |
| `renderingpipelines+0x5450` | 方形或径向图案：边长取宽高的较小者，**居中在整张图里**，每个纹素两字节 |
| `rendersystemvulkan+0x34320` | EndRendering：向 VR 接口要每只眼的注视中心（NDC），用 `lroundf(中心 × 0.5 × 渲染区域 / 粒度) × 粒度` 换成像素偏移，经 `VK_EXT_fragment_density_map_offset` 提交 |
| `rendersystemvulkan+0x347C0` | BeginRendering：渲染描述（第 2 个 dword 为视图数）拷到上下文 `+0x1310`，渲染区域拷到 `+0x154C` |

默认立体模式（multiview instancing）下两只眼共用一层 2592×1200，FDM 图 81×38 也只有一张。结果：

- 图案的高密度区在整张图中央，也就是两眼接缝上。
- 偏移按整张图的宽度（两眼之和）换算，眼动偏移量是应有的两倍。

## 补丁

- `etfr_fdm_split`（`renderingpipelines.dll`）：
  - hook `+0x16C70` 记下层数，hook `+0x5450`：单层且宽大于高时，把游戏自己的图案分别按左、右半宽各生成一次再拼起来，每只眼一块高密度区。
  - 其他情况（多层、旧方形、缓冲不够）调回原函数。
  - 图案缓冲是 64 KiB 的 `.bss`。
- `etfr_offset_eye`（`rendersystemvulkan.dll`）：在 `+0x34474`（换算循环开头，8 字节指令）放一个 `before` site，单视图且渲染区域宽大于高时把两个中心的 x 减半。
- `etfr_fdm_ab`（诊断，只用于 A/B）：把 `+0x1786C` 的调用 `replace` 为“没有 FDM 纹理”，用 `guest_patch enable|disable` 在同一局里切换 FDM 开关。

三个包都通过了安装检查，状态里 `installed=1`、`skipped=[]`。

## 位置验证（Turnip `fdmviz`，无人佩戴，注视由 `debug.xrgame.xr.gaze` 覆盖）

fdmviz 把每个片段按面积染色（红 ≥16 像素，橙、黄、绿依次更密）。表中是游戏渲染的单眼图（1296×1200）里“没有染色”的全密度区中心。

| 条件 | 眼 0 中心 x / y | 眼 1 中心 x / y |
|---|---|---|
| 已打补丁，注视 0,0 | 576 / 640 | 576 / 640 |
| 右 20° | 784 / 640 | 784 / 640 |
| 左 20° | 344 / 640 | 344 / 640 |
| 上 10° | 576 / 528 | 576 / 528 |
| 同一局，右 20°，偏移补丁关 | 944 | 944 |
| 同一局，右 20°，偏移补丁开 | 784 | 784 |
| 没打补丁，右 20° | 眼 0 里没有全密度区，眼 1 的中心在 320 | |

- 预期偏移：右 20° 时 NDC x = tan20°/tan47.5° = 0.3335，按单眼宽度换算为 0.3335 × 0.5 × 1296 = 216 像素；上 10° 为 0.2012 × 0.5 × 1200 = 120。实测 +208、−232（左）、−112（上），都在一个粒度（32 像素）之内。
- 注视 0,0 时中心在 576 而不是 648：Alyx 只调用 `GetEyeTrackedFoveationCenterForProjection` 一次，两眼共用一个中心，Swan 两眼外倾 ±5°，所以有 ~0.08 NDC 的固定偏差，已经记录（`alyx-vulkan-etfr` 备忘）。
- 偏移补丁关闭时，位移是开启时的约 1.8 倍（944−576 与 784−576），方向与“偏移按整张图宽度换算”一致，比理论的 2 倍小一点，因为全密度区被渲染区域边缘截断。
- 计数器：偏移补丁在 29 844 次 EndRendering 里全部走了每眼换算（`offset_kept` 为 0），图案补丁拆分了 1 张图（`fdm_kept` 为 0）。

## 画面：周边发黑与景深预通道

- 打补丁、`+vr_fdm 1`、默认参数（run3）：双眼图的全密度块外基本是黑的，周边平均亮度 1.8（FDM 关时是 37.1），94% 的周边像素亮度 < 4。
- 没打补丁（run2，带染色）：周边平均亮度 7.2，79% 的像素 < 4。**所以这不是补丁造成的**，这就是用户 10-15 晚上佩戴时看到的“全密度块亮、周边暗”。
- 关掉 `+vr_subsampled`（`+vr_render_subsampled 0`，run5）没有改变。
- 加 `+vr_depth_prepass 0`（run6）：周边平均亮度 37.5（FDM 关为 37.1），没有 < 4 的像素，全密度块 40.9 对 40.3。周边按预期变粗，没有发黑。
- 解释（推断，没有进 Turnip 验证）：Alyx 先做景深预通道，再在前向通道里载入这份深度；预通道和前向通道各自提交 FDM 偏移，Turnip 按分箱缩放加载深度时两者对不上，周边的深度测试全部失败。

## 性能：这是结论里最重要的一条

状态层读数，72 Hz，游戏帧率，单位 fps。同一局里用 `etfr_fdm_ab` 切换 FDM；GPU 占比：FDM 开的阶段 99%，关的阶段 85%–99%（run7 的 51 fps 阶段约 89%）。

| 轮 | 配置 | FDM 关 | FDM 开 |
|---|---|---|---|
| run7 | `+vr_fdm 1 +vr_depth_prepass 0`，4 个 30 s 阶段（关-开-关-开） | 51.3、51.5 | 27.6、27.6 |
| run8 | 同上加 `TU_DEBUG=gmem` | 38.1、37.9 | 25.3、25.4 |
| run12 | 同 run7，4 个 45 s 阶段 | 32.4、40.3 | 24.6、26.3 |
| run11 | 同 run7，只测关（跨局重复） | 37.9 | |

- 每一对都是开 < 关，开比关慢 24%–46%。
- “关”的绝对值在不同局之间差得很大（run7 的 51 fps 对 run11/run12 的 38 fps，同配置），所以只有**同一局内的比较有意义**，跨局的数字不要直接比。
- 景深预通道本身：没有 FDM 时，预通道开 35.8、关 37.8（run9、run10，跨局，差距在噪声以内）。
- 周边变粗的画面细节：FDM 开时 |梯度| 均值 1.299，关时 1.759。

也就是说，在这套 Turnip（xrg16，A840）上，Alyx 的 FDM 不省 GPU，反而更贵。这和位置是否正确无关。我没有测原因；候选：
- A840 的 `has_hw_bin_scaling=False`，缩放走 `load_3d_blit` 的软件路径和逐分箱 patchpoint（`tu_clear_blit.cc` 的 `fdm_apply_load_coords`）；
- `TU_DEBUG=gmem`（run8）下同一局里依然是开更慢（25.3 对 38.1），所以不是只有 sysmem 路径才慢。

## Turnip 的 FDM 为什么慢（调查，进行中）

读 `references/mesa-turnip`（`4815dc71`，xrg16 为其加补丁）：

- `tu_cmd_buffer.cc`：`bin_scale_en = CHIP < A8XX && has_hw_bin_scaling && …`。A840 是 A8XX，硬件分箱缩放从不启用；只要某个分箱被缩放，`disable_lrz = true`，该通道没有 LRZ。
- 缩放分箱靠 CPU 侧 patchpoint 实现：视口、裁剪、片段着色器参数三类状态各自登记，每个分箱都要重放（`CP_MEM_WRITE`，再 `CP_WAIT_MEM_WRITES`；A8XX 的着色器常量走 preamble，每个分箱还要 `CACHE_INVALIDATE` + WFI，最后 `CP_WAIT_FOR_ME`）。
- 有 patchpoint 就关并发分箱（`tu7_cb_disable_reason("FDM patchpoints")`）。
- autotune 把带 FDM 的通道强制成 GMEM（`tu_autotune.cc` “Uses a fragment density map”），没有 FDM 的通道可以走 sysmem。

测量（run13，同一局，`etfr_fdm_ab` 切换，`+fove_radial_inner 1 +fove_radial_outer 1` 让游戏的 FDM 保持开启但全图全密度，游戏自己的 dump 里所有纹素都是 255）：

| 阶段 | 游戏 fps |
|---|---|
| FDM 关 | 49.8、50.3 |
| FDM 开，全图全密度 | 11.7、11.7 |

- **没有任何着色节省时 FDM 反而更慢（11.7，而有周边降密度的图是 27.6）。** 所以开销不在着色，是每个分箱的固定成本：全密度时分箱不合并，分箱数最多；有降密度时周边分箱合并，分箱数少，所以更快一些。
- 这与上面的代码阅读一致（per-tile patchpoint + 缓存失效 + WFI），但还没有逐分箱的 GPU 时间戳证实；Turnip 的 u_trace（`MESA_GPU_TRACES=print`，`draw_ib_gmem` 起止）可以量，没做。
- 要让 Turnip 的 FDM 有收益，需要的是 A8XX 的硬件分箱缩放（`GRAS_BIN_FOVEAT`/`RB_BIN_FOVEAT` 的正确编程），或者去掉逐分箱 patchpoint；这是 Turnip 改动，需要用户批准。

## 高通（系统）驱动能不能跑 Alyx

- `cmd gpu vkjson`：Adreno 840，Vulkan 1.4.39，184 个设备扩展，包括 `VK_EXT_fragment_density_map`、`VK_EXT_fragment_density_map2`、`VK_EXT_fragment_density_map_offset`、`VK_QCOM_fragment_density_map_offset`、`VK_EXT_host_image_copy`、`VK_KHR_dynamic_rendering`、`VK_KHR_multiview`、`VK_QCOM_multiview_per_view_render_areas`。Alyx 要的 FDM 偏移扩展有；不再需要我们给 Wine 加的 `VK_EXT_host_image_copy` 补丁。
- 缺的是呈现通路，不是驱动能力：
  - 系统驱动是 Android HAL（`/vendor/lib64/hw/vulkan.adreno.so`，由 `/system/lib64/libvulkan.so` 加载），只提供 Android surface；Khronos 加载器不能直接把它当 ICD 用。
  - Turnip 靠 Mesa 的 X11 WSI + `turnip-x11-ahb.patch`（分配 AHB，经 `xcb_dri3_pixmap_from_buffers` 交给 App 内的 X server）；高通驱动没有这一层。
  - 上游 GameNative 的“System 驱动”走 `libvulkan_wrapper`，v1 规格明确不用它（无源码），picoXr 里也没有。
- Wine 侧：win32u 自己管理 swapchain（`win32u_vkCreateSwapchainKHR`、`QueuePresent`、`AcquireNextImage`），只有 surface 由用户驱动（`winex11.drv`）通过宿主的 `vkCreateXlibSurfaceKHR` 创建，系统驱动没有这个扩展。因此可行的做法是在我们的 Proton 里做一个“AHB 软件 swapchain”：宿主加载器换成 Android 的 `libvulkan.so`，swapchain 用 AHB 图像，呈现仍走 DRI3 + 现有的 X server 交换环。这是新组件，不是配置项。
- 没有试：没有在高通驱动上跑过 Alyx 的任何东西。

## 结论与待办

- ETFR 的两块 guest patch（每眼图案、每眼偏移）功能上做对了：全密度区跟着注视，每只眼一块，数值与预期一致。
- 要看到正确画面，必须同时加 `+vr_depth_prepass 0`（容器启动参数，不是补丁）。
- **在 Turnip 的 FDM 路径变快之前，ETFR 没有性能收益，不应该默认打开。** 下一步应该是查 Turnip 里 FDM 变慢的原因（先用 gpu-snapshot / 计数器看是加载、存储还是片段着色）；改 Turnip 需要用户批准。
- 未测：真人佩戴；`fove_square_use_legacy 1`（旧方形走 `+0x5AF0`，补丁里没有 hook，包会保持游戏原图案）；径向图案；`-multiview` 路径（单眼层，本来就不需要拆分，补丁自动跳过）；长时间稳定性。
- `etfr_fdm_ab` 只在 `+vr_fdm 1` 时有意义，仅作为测量工具。
