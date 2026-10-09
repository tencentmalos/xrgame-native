# GPU 优先级分层：合成高于游戏（v1，2026-10-09）

- 证据：[xr-stage-timing-20261009](../validation/xr-stage-timing-20261009.md)（run19–run23）。
- 状态：Swan 已实现（app 经 Pico 接口进入 ring1，游戏保持默认 ring2）。其他设备和平台为**方案**，尚未实现或测量。

## 1. 问题与目标

- 游戏（Wine/DXVK/Turnip）和我们的合成（app 的 GL/Vulkan 上下文）共用一块 GPU。
- 游戏的单个 command batch 长达 14–25 ms。如果合成与游戏同优先级，合成只能排在游戏的 batch 后面：Swan 上实测要等约 23 ms。头显运行时会按 app 每帧 GPU 完成的时间来调节 xrWaitFrame，结果整个 XR 循环被拖到约 38 Hz（run19）。
- 目标是三层顺序：**系统合成器 > 我们的合成 > 游戏**。在 Adreno/KGSL 上，只有不同 ringbuffer 之间才会抢占；同一个 ring 内只能等前一个 batch 结束。

KGSL 优先级为 0–15，按 `prio / 4` 映射到 4 个 ringbuffer。Pico 的分配（spatial_runtime `SpatialRuntimeSession.cpp` 注释）：

| 优先级 | ring | Pico 用途 | 我们的用途 |
|---:|---:|---|---|
| 0 | 0 | xr runtime、SurfaceFlinger | — |
| 4 | 1 | xrshell | 我们的 XR 合成线程（Swan） |
| 8 | 2 | 焦点 app、vrshell（驱动默认值） | 游戏（Turnip 默认） |
| 12 | 3 | 非焦点 app | 回退方案中的游戏 |

## 2. 通用策略（按顺序尝试）

1. **提升我们的合成**（首选，只涉及自己的进程）：
   1. 平台私有接口。Pico 是 `libsysperftracker.so` 的 `GPUOptimization::setPriority(pid, tid, 4)`（KGSL ioctl `0xC00C09B0`）。符号缺失或返回失败时静默回退。
   2. 标准接口：GL 用 `EGL_IMG_context_priority` HIGH，Vulkan 用 `VK_EXT/KHR_global_priority` HIGH。必须验证真实生效：Swan 上 EGL 返回 HIGH，但 KGSL 里实际仍是 8（run20）。
2. **降低游戏**（仅当第 1 步失败；降级在 KGSL/DRM 上都不需要特权）：
   1. 标准做法：DXVK / vkd3d-proton 给自己的队列加 `VkDeviceQueueGlobalPriorityCreateInfoKHR(LOW)`，由各自 fork 里的配置项开启，避免依赖 Turnip 私有环境变量。驱动侧需要真正实现全局优先级：
      - Turnip 的 msm 后端已经支持；
      - Turnip 的 KGSL 后端目前忽略队列优先级（`submitqueue_priority_count = 1`）。需要在共享的 `malos/main` 上补全为 4 级（0/4/8/12）。实验版补丁 `turnip-kgsl-context-priority`（`TU_KGSL_PRIORITY`，run22）证明降到 12 有效。该补丁未合入，保存在云端分支 `feature/malos/kgsl-context-priority` 和证据目录里。
   2. 驱动不支持时：保持同优先级，转用第 3 步的缓解手段。
3. **无法分层时的缓解**（与优先级无关，各平台都受益）：
   - 没有新帧时不提交 GPU 工作，重提已释放的图像（run21/run22 已实现）。
   - 零拷贝：让游戏直接渲染进头显 swapchain 的 AHB。合成那一步就消失了，XR 循环不再依赖 app 的 GPU 完成时间。这也是替代优先级的长期方案。
   - 缩短游戏单个 batch（DXVK 更频繁地提交），降低同 ring 内的排队上限。需要实测对游戏帧率的影响。

启动时的选择：app 侧先探测第 1 步能否成功，再决定是否给游戏下发第 2 步的降级配置。探测结果和最终布局都要写进日志和诊断记录。

## 3. 平台矩阵

| 平台 | GPU / 内核 | 合成路径 | 计划 | 状态 |
|---|---|---|---|---|
| Swan（Pico，Android 16） | Adreno 840，KGSL | OpenXR GLES 投影层 | app 经 Pico 接口设为 4；游戏保持默认 8 | 已实现；run24 实测 rb0/rb1/rb2 三层分布，合成 p90 1.24 ms（录屏期间） |
| 其他 Pico 头显 / ROM | Adreno，KGSL | 同上 | 同上，但要先探测库和符号 | 未测 |
| AYN Thor、Odin3 等 Android 掌机 | Adreno，KGSL | app 自己的 Vulkan 呈现（平面 / SBS），经 SurfaceFlinger | 先试 Vulkan global priority HIGH 并验证；不生效时游戏降为 LOW（需要 Turnip KGSL 补全加 DXVK 配置） | 未测 |
| 非 Adreno 的 Android（Mali 等） | 厂商驱动 | 同上 | 有 global priority 就用；没有则走第 3 步 | 不在当前 v1 路径上 |
| Linux 主机 / 骁龙笔记本 | msm DRM 或桌面 Mesa | 不适用或另行设计 | Turnip msm 后端已支持全局优先级，只需 DXVK 侧请求 LOW | 方案 |

## 4. 验证（每个平台都要做）

- **有 root 时**：KGSL tracefs（`adreno_cmdbatch_queued/retired` 的 `ctx_prio`、`rb_id`、抢占记录）；Pico 上还可以读 `/proc/gpu_procs/<pid>/<tid>/status` 的 `ctxt_prio`。分析脚本见验证记录。
- **没有 root 时**：用埋点契约 v1 的 XR 循环周期、上屏延迟、未绘制帧数做判断。计划再加一个指标：合成的 GPU 完成延迟（EGL/Vulkan fence 从提交到 signal），以便不依赖 root 判断合成是否被游戏挡住。
- **验收**：在游戏 GPU 饱和的场景中，合成从提交到完成的 p90 < 2 ms，并且 XR 循环保持在显示刷新率。

## 5. 风险

- Pico 接口是未公开的 C++ 符号加厂商 ioctl，换 ROM 后可能失效。必须保留静默回退和诊断记录。
- 我们占用了 Pico 留给 xrshell 的优先级 4。系统 UI、xrshell 以及录屏时运行时的 capture presentation 与我们同处 ring1，互相不能抢占。run24 录屏期间合成 p90 仍为 1.24 ms；打开系统菜单的场景还没测。
- 降低游戏优先级后，游戏会被所有默认优先级的 GPU 客户端抢占。实测（run22）游戏每帧约多 0.4 ms。
