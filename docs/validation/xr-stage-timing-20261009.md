# Swan XR 逐帧阶段时序：首次实测与 GPU 上下文拆分（2026-10-09）

- 分析方法：[gamenative-stage-concurrency-analysis](../../skills/gamenative-stage-concurrency-analysis/SKILL.md)，
  埋点契约 v1（[instrumentation-contract](../../skills/gamenative-stage-concurrency-analysis/references/instrumentation-contract.md)）。
- 状态：契约 v1 **首次在设备上采到完整数据**（Swan + Half-Life: Alyx，每轮一个 30 秒窗口）。另有每轮 20 秒 KGSL
  按上下文的 GPU 时间。run20（app EGL 上下文申请高优先级）**无效**；run22（游戏的 Turnip 上下文降为 KGSL
  优先级 12，加上无新帧时不重绘）让 XR 循环恢复到 72 Hz（§6）。
- 原始 PROF、sidecar、KGSL trace、导出的 counters/slices、bundle 和 Archify 交付都存放在仓库外的
  `xrgame-native-evidence/swan-20261008/swanxr-ahb/run19-timing/`（README 记录了每个文件的 SHA）。
  APK 和日志不提交。

## 1. 身份

| 项 | 值 |
|---|---|
| 源码 | `feature/malos/swan-xr-ahb` @ `51717b60` 加上未提交改动（per-eye 拷贝区域、VMA/语言、契约 v1 埋点、OpenXR 注册表兜底） |
| APK | `app-picoXr-debug.apk`，在 WSL 中从头打包，SHA-256 `11be30f847eaa11fb1b4f7675a745d62fb1378c1d10deff15c1f10b8c671fb58`；`tools/audit-apk`：131 个文件，0 个错误 |
| Build ID | `gamenative_xr_unixbridge.so` `b47bf7e71f9d571c0131f9430795a6f5f919b791`，`libxrimmersive.so` `aecbd755c9e3d33f45d62a9c8af07d1fa6f81f70`，`libxrgame_debugbus.so` `997c19fd828c62c5b336659adca7411b163cf776` |
| 运行时 catalog | `020f0872be940c3596674437285f8a85a87b274c59a60973fb15705d5bf87e76`（Proton 11 xrg5 带 VMA 合并，Turnip `25ef164` xrg6） |
| 设备 | Swan B3110，Android 16，构建 6.2.0（`BQ2A.260122.002-BP2A.250705.008`），boot_id `9cbe019b-9d6f-4a49-8a61-062e11b746e0`；`vm.max_map_count` 为用户临时调大的 1048576 |
| 进程 | app PID 8471，`hlvr.exe` PID 13651（`-vr -condebug`，容器 `TU_DEBUG=nolrz`） |
| 采集 | DebugBus `profiler_capture file 64 30`，capture_id 1，10:33:44 开始，持续 29.98 s，coarse 档；PROF `766b8957…`，sidecar `ed27f7b7…` |
| GPU 状态 | 采集后抽样 kgsl 忙碌率 97–99%，902 MHz；`thermal_pwrlevel=4`，即温控上限 902 MHz（共 18 档，最高 1200 MHz）；显示 72 Hz |

litep MCP（20260921.4）拒绝导入这个 PROF，报错为 SDK wire provenance 未知（`0b467a8`），显式传 `sdk_revision`
也一样。因此改用 litep 包内的参考解码器（`decoder.py` `beec94cf…`），由 skill 的 `export_prof_stage_inputs.py`
导出：共 46,934 个事件，无截断，停止时一个线程上有 2 个 span 未闭合。时钟偏移 mono − trace = 0（ArmCntvct）。

## 2. 整段 30 秒（1103 个游戏帧，fid 连续；1147 个 XR 帧）

| 指标 | p50 | p90 | p99 |
|---|---:|---:|---:|
| 游戏帧间隔（bridge submit 结束 → 下一帧） | 27.13 | 28.59 | 32.25 ms |
| shipper 等待游戏 GPU 完成（fence wait） | 26.61 | 27.59 | 28.66 ms |
| 游戏渲染 CPU（右眼 swapchain 等待结束 → drain 开始） | 20.07 | 21.85 | 22.53 ms |
| FRAME_SYNC 往返（游戏侧） | 0.64 | 6.27 | 17.62 ms |
| 控制服务器 FRAME_SYNC（等待新 XR serial） | 0.29 | 5.93 | 17.19 ms |
| DXVK drain（flush + 锁） | 0.14 | 0.62 | 1.74 ms |
| bridge submit | 0.02 | 0.10 | 0.29 ms |
| swapchain 等待 L / R | 0.09 / 0.05 | 0.19 / 0.10 | 0.98 / 0.59 ms |
| FRAME 发送到 app 收到 | 0.13 | 0.37 | 1.32 ms |
| XR 循环周期 / xrWaitFrame | 26.08 / 25.57 | 34.05 / 33.52 | 41.73 / 41.24 ms |
| app 绘制 / input+locate / xrEndFrame | 0.20 / 0.22 / 0.02 | 0.38 / 0.35 / 0.05 | 2.64 / 0.52 / 0.13 ms |
| submit 结束 → 该 fid 首次被 XR 帧绘制 | 38.78 | 50.94 | 54.73 ms |
| FRAME_SYNC 开始 → app 收到该帧 | 52.54 | 54.84 | 56.12 ms |

- 91 个游戏帧（8.3%）从未被绘制，134 个被绘制了两次。
- 1147 个 XR 帧中，129 个没有新的游戏帧，10 个左右眼显示的 fid 不同。
- 六帧时序图取采集中点的 fid 4486–4492（按确定规则选取，不挑最慢的帧）。严格分析通过，fid token 契约通过。

## 3. KGSL 按上下文的 GPU 时间（10:45 开始，20.07 s GPU tick，无丢事件）

采集方法：root 下建私有 tracefs instance `xrg_kgsl`（mono 时钟），启用 `adreno_cmdbatch_queued/submitted/retired`、
`adreno_drawctxt_switch`、`adreno_preempt_*`，采完删除 instance，不改全局 tracing。用 queued 事件的线程查到
Tgid，再把上下文对应到进程：ctx 8 / 13 是 `com.pico.xr.openxr_runtime` 的 compositor（优先级 0）；ctx 49 是
SteamPSP 的 XR 线程（优先级 8）；ctx 53 是 `hlvr.exe`（优先级 8），其中 `dxvk-submit` 线程提交游戏渲染，渲染线程
提交 bridge 的 AHB 拷贝。

| 上下文 | 独占 GPU 份额 | 单个 batch GPU 时间 p50 | queued → retired p50 / p90 |
|---|---:|---:|---:|
| 游戏 DXVK 渲染 | 89.9% | 0.08 ms（p90 14.1，p99 23.0） | 23.7 / 39.5 ms |
| Pico compositor（ctx 13 + 8） | 6.5% | 0.37 + 0.08 ms | 0.75 / 1.0 ms |
| SteamPSP XR 合成 | 1.7% | 0.33 ms | **23.2 / 32.6 ms** |
| bridge AHB 拷贝（每眼区域） | 1.6% | 0.34 ms | 22.9 / 24.1 ms |
| 空闲 | 0.3% | | |

app 合成的 batch 在 GPU 上只执行 0.33 ms，但在 ring 上平均要等 22.6 ms（p50），排在同优先级的游戏 batch
之后。只有 compositor 在抢占：rb0↔rb2 之间共 2861 次。这与 XR 循环只有约 38 Hz 一致：运行时按 app 每帧
GPU 的实际完成时间拉长了 xrWaitFrame。

## 4. 校准结论

1. 设备处于温控上限下的 GPU 瓶颈，游戏自身占约 90%。要提帧率，只能减少游戏的 GPU 工作量，可选手段有 LRZ、
   分辨率/画质档位、注视点渲染。零拷贝（去掉 bridge 拷贝加 app 合成）最多能省约 3.3% 的 GPU。
2. 跨进程开销实测都很小：FRAME 0.13 ms，ACQUIRE 约 0.1 ms，drain 0.14 ms，submit 0.02 ms。去掉它们提不了帧率。
   FRAME_SYNC 的长尾来自游戏和 XR 循环的节奏耦合，不是 socket 本身的开销。
3. XR 循环降频的根因是 app 的 GL 上下文与游戏同优先级，见 §6。
4. 没有新帧时重提上一张已释放的图像，不再重绘（run21 的改动，随 run22 一起实测）。

## 6. 优先级修复：run20 无效，run22 有效

**run20**：APK `9a709c2298b0e11783a739f4c12374cac343faca8aa9385a08593c74d8b152f3`。app 的 EGL 上下文申请
`EGL_IMG_context_priority` HIGH，`eglQueryContext` 返回 HIGH（`0x3101`），但 KGSL 里 XR 线程提交的上下文仍是
`ctx_prio=8`。app 合成 queued → retired p50 26.1 ms，XR 循环 p50 25.5 ms，都没有改善。可见驱动或内核没有把
普通 app 的 HIGH 请求落到 GPU 上下文上。这一轮之前，用户把 Alyx 改成了低画质：游戏帧间隔 p50 从 27.13 降到
26.16 ms，KGSL 中游戏的份额从 89.9% 变为 90.1%。

**run22**：APK `bd5a63feac45cc1830a4ed5c41cace7272d657cb5c29128fb4056932966d7309`，catalog `a315b492…`。
- Turnip `turnip-25ef164-xrg9`（`c89384a2…`，Build ID `8f7d5cd834da7e6e0c5dde4cc655383e3e418da9`）是在 xrg6
  源码上加 `tools/xrgame/patches/turnip-kgsl-context-priority.patch`（`c3f4fd4f…`）：设置 `TU_KGSL_PRIORITY`
  时按该优先级创建 KGSL 上下文，不设置时行为不变。
- Windows VR 启动时，若容器没有设置，就给游戏设 12。游戏进程环境中确认为 `TU_KGSL_PRIORITY=12`；app PID 16420，
  `hlvr.exe` PID 26598，画质设置与 run20 相同。

| 指标 | run19 p50 / p99 | run22 p50 / p99 |
|---|---|---|
| KGSL app 合成 queued → retired | 23.2 / 32.6（p90）ms | **0.81 / 1.87 ms** |
| XR 循环周期 | 26.08 / 41.73 ms | **13.89 / 14.30 ms** |
| 游戏帧间隔 | 27.13 / 32.25 ms | 26.58 / 28.04 ms |
| FRAME_SYNC 往返 | 0.64 / 17.62 ms | 0.49 / 0.91 ms |
| submit 结束 → 首次绘制 | 38.78 / 54.73 ms | 32.35 / 40.61 ms |
| app 收到 → 首次绘制 | 12.20 / 27.38 ms | 6.36 / 13.73 ms |
| 从未绘制的游戏帧 | 91 / 1103 | 0 / 1133 |
| 每个游戏帧被绘制的 XR 帧数 | {0: 91, 1: 878, 2: 134} | {1: 109, 2: 1020, 3: 4} |

- KGSL：游戏 ctx 57 的优先级为 12，在 rb3 上；app 的 ctx 53 在 rb2 上抢占它 764 次，compositor 抢占 2703 次。
  独占份额：游戏 90.7%，compositor 6.5%，bridge 拷贝 1.3%，app 合成 1.3%，空闲 0.1%。
- 1014 个 XR 帧走了“重提已释放图像”的路径（无新帧）；app 绘制 p50 降到 0.14 ms。
- 与画质相同的 run20 相比，游戏帧间隔 p50 从 26.16 增加到 26.58 ms（被抢占约 0.4 ms/帧），p99 从 29.72 降到
  28.04 ms。游戏帧间隔一栏与 run19 不是干净对比（画质不同）。
- 剩余问题：游戏约 37.6 fps，比 XR 的一半（36 fps）略快，相位缓慢漂移。109 个游戏帧只显示了一次，六帧窗口内
  延迟从 27.6 漂到 30.2 ms。

### run23：app 经 Pico 接口进入 ring1

- APK `58a118d6e2dce67098c7ecf7c76e9b86a6338a7f3a7239f94e7099cd5bcbad8d`（libxrimmersive `f0e83dd9…`），其余同 run22。
- 接口来源：spatial_runtime（`D:/workspace/spatial_runtime` @ `579e146fe4`）的 `RenderThread::setGpuPriorityImpl` 和
  `SpatialRuntimeSession::setGpuPriority`，二者都调用 `/system/lib64/libsysperftracker.so`（列在 `public.libraries.txt`
  中）。反汇编可见，`GPUOptimization::setPriority(pid, tid, prio)` 会打开 `/dev/kgsl-3d0`，发 `ioctl(0xC00C09B0, {pid, tid,
  prio})`，并拒绝 prio ≥ 16。
- 结果：XR 线程（30916）调用后返回 0；root 读回 `/proc/gpu_procs` 的 `ctxt_prio` 为 4（app 进程自身无权读取该节点）。
  KGSL 中 app 的合成上下文 ctx 53 为优先级 4，跑在 rb1；rb3→rb1 抢占 877 次，rb1→rb0（compositor）30 次。
  合成从 queued 到 retired p50 0.84 / p90 1.02 ms。XR 循环 p50 13.89 / p99 14.34 ms，没有未绘制的游戏帧。
- 重新进入游戏后场景变了（游戏帧间隔 p50 22.04 ms，渲染 CPU 11.2 ms），游戏侧数据不能与 run22 对比。

### run24：Swan 最终布局（运行时 ring0、app ring1、游戏 ring2）

- 去掉游戏降级，Turnip 回到共享的 xrg6，app 保留 Pico 接口。APK
  `a18a6bce8412599f617a450ef2dcb9dc221fd8379b3a070b86188fbd1c67d0ab`，catalog `020f0872…`，libxrimmersive
  `eabc9c36…`。通用方案见 [GPU 优先级分层](../specs/xrgame-native-gpu-priority-v1.md)。
- 实测（13:35，app PID 26333，游戏 PID 637）：XR 线程 `ctxt_prio` 4，游戏 8。KGSL 中 compositor 在 rb0，
  app 合成在 rb1，游戏在 rb2。抢占记录：rb2→rb1 955 次，rb2→rb0 2027 次，rb1→rb0 103 次。
  app 合成从 queued 到 retired p50 0.90 / p90 1.24 ms；XR 循环 p50 13.91 / p99 14.32 ms，没有未绘制的游戏帧。
- 这一轮用户在录屏：运行时的 capture presentation（优先级 4，rb1）和硬件编码器（优先级 8，rb2）也占用 GPU，
  而且场景和帧结构与之前不同（游戏 p50 39.4 ms），所以游戏侧数据不作对比。

### run26/run27：优先级被系统改回，以及节拍与延迟开关的 A/B

- run26（APK `cbea7492…`）和 run25 在游戏中都退回到 run19 的状态：PxrMetric 显示 XR 28–45/72，FrmGpu
  17–44 ms。XR 线程申请优先级 4、返回 0，但游戏中用 root 读回，app 的所有上下文都是 `ctxt_prio:8`，KGSL 中
  app 合成从 queued 到 retired p50 23.5 ms。Pico 系统会把前台 app 的上下文改回默认的 8，run24 只是采集时恰好
  还是 4。同一时段还有其他任务留下的 `deqp-vk` 占用约 22% 的 GPU，它在 15:49 前后自行退出。
- run27（APK `47224d15…`，libxrimmersive `bbf67144…`）：XR 线程每秒重新申请一次。游戏中读回为 4；GPU
  99–100% 时 PxrMetric 72/72，FrmGpu 约 0.5 ms，MTP 约 20.5 ms。
- 同一场景、同一次游戏会话，用 DebugBus `vr_tuning` 在线切换，每个变体先稳定 10 秒，再采 30 秒 Litep 和
  20 秒 KGSL+sched（无丢事件）。证据在 `run27-prio/`。

| 变体 | 30 秒游戏帧数 | 帧间隔 p50 | 取姿态 → GPU 完成 p50 | 每帧显示次数 {1,2,3} | GPU 空闲 | app 合成 q→r p50 | 预测提前量 |
|---|---:|---:|---:|---|---:|---:|---:|
| 全关 | 1114 | 26.99 ms | 53.15 ms | 76 / 1030 / 8 | 0.12% | 0.82 ms | 4.78 周期 |
| `pacing=half` | 1080 | 27.70 ms | 40.59 ms | 3 / 1075 / 2 | 3.08% | 0.86 ms | 3.00 |
| `start=3000` | 1062 | 27.83 ms | 34.59 ms（推迟 p50 6.56） | 15 / 997 / 50 | 2.93% | 0.83 ms | 3.75 |
| `predict=1` | 1095 | 27.45 ms | 54.06 ms | 46 / 1034 / 15 | 0.08% | 0.82 ms | 4.88 |
| 三项全开 | 1061 | 27.98 ms | 37.47 ms（推迟 p50 3.11） | 6 / 1012 / 43 | 3.65% | 0.90 ms | 3.05 |

- 锁 36 Hz：99.5% 的游戏帧正好显示两次，取姿态到 GPU 完成缩短 12.6 ms，代价是游戏帧率从约 37.0 降到
  36.1 fps，GPU 让出约 3%。
- 推迟 FRAME_SYNC：延迟缩短最多（18.6 ms），游戏帧率约 −3%；因为帧略慢于 36 Hz，显示 3 次的帧增加到 50 个。
  与锁 36 Hz 叠加时两者都在推迟取姿态，显示 3 次的帧为 43 个，比单独锁 36 Hz 差。
- 拉长预测不改变时序（提前量约 4.8 个 XR 周期，约 67 ms），效果只能靠佩戴体感判断。
- 逐线程 CPU（全关，sched_switch 23.5 秒）：调用运行时的游戏线程在 Submit → xrEndFrame（占 77% 时间，
  即伴随窗口的 DXGI Present 等待）中 99% 时间不在 CPU 上；主线程 on-CPU 40.7%，8 个工作线程各约 13.5%。
  没有线程跑满，这个场景是 GPU 瓶颈（run22 型）。run24 型场景这一轮没有复现。Wine 线程没有名字。

### run28/run29：LRZ 卡死的原因与修复

- 10-08 的卡死快照（完整 LRZ）：BR 停在 `CP_REG_RMW GRAS_SC_BIN_CNTL |= FORCE_LRZ_DIS` 等 ME，BV 停在
  `CP_THREAD_CONTROL` 线程同步等 BR。跳过 RMW 的等待（run8）后，BR 改停在下一个 `CP_SET_DRAW_STATE`，BV 不变。
  所以 RMW 不是根因，而是 BR 在等需要 BV 推进的东西，BV 又在等 BR。
- Turnip 在 LRZ 不能快速清除时会关掉该 render pass 的并发分箱（concurrent binning）。因此 `nolrz`（LRZ 关、
  并发分箱开）和 `nolrzfc`（LRZ 开、并发分箱关）都稳定；run28 中 `nolrzfc` 相对 `nolrz` 没有可测收益。
  卡死需要 LRZ 与并发分箱同时开启。
- 修复 `tools/xrgame/patches/turnip-lrz-rmw-no-cb.patch`（Turnip `turnip-25ef164-xrg10`）：render pass 在
  suspend/resume 或 secondary 边界发出 LRZ RMW 时记一个标志（随 render pass 状态合并），该 render pass
  关闭并发分箱，其余 render pass 保持 LRZ 与并发分箱。
- run29（APK `cde50d35…`，`TU_DEBUG=noconform` 即完整 LRZ）：从启动到采集约 5 分钟没有 GPU 卡死。游戏帧间隔
  p50 26.68 ms，游戏每帧 GPU（fence 等待）25.85 ms，对比 `nolrz` 的 26.99 / 26.67 ms，约快 1–3%，接近噪声。
  换驱动后首次进游戏约 60 秒只有 5–6 fps，GPU 只有 40–50% 忙，之后恢复到 37–42 fps，推测是驱动二进制变化导致
  着色器缓存重建。长时间稳定性还没有测。

## 7. 未覆盖

- 只有一个场景、一个 30 秒窗口和一个 20 秒 KGSL 窗口。没有长时间稳定性和热稳态数据。
- PE 侧 drain 用 QPC 计时再换算到 CLOCK_MONOTONIC，误差没有单独验证。
- FRAME_SYNC 走 winsock 回退路径的帧没有时间戳；本次采集中没有出现这种帧。
- 没有 GPU 时间戳 query，CPU/GPU 对齐标为 unknown；KGSL 数据与 PROF 不是同一时间窗口。
