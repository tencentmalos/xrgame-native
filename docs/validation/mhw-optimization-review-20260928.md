# MHW 性能改进复核（2026-09-28）

结论：先解决异常功耗/调度状态，再针对正常约 24 FPS 场景做呈现异步化和 guest 热点
优化。总 CPU/GPU 利用率不是目标；目标是实际显示帧间隔、长帧和输入响应。
本轮只复核既有数据与源码，没有新的设备性能试验，也没有改默认运行行为。
AYN 当前未连接；仅发现延期设备 Swan，未对其操作。

数据身份沿用 [原始性能记录](mhw-performance-20260927.md) 与
[同步统计](mhw-sync-20260927.md) 中的 APK/catalog/Build ID、AYN boot、PID 和时长。
代码基线是主仓阶段提交 `2ac13287`，不能把后续维护子仓 rebase 当作这些样本的运行时。

## 重新核对后的判断

| 观察 | 可以支持的判断 | 不能据此认定 |
| --- | --- | --- |
| 正常片段 Present 23.86/s，GPU busy 76.9%、约 668 MHz；最热 guest 线程 75.2% 单核，93.9% 样本在 FEXMemJIT | guest 串行工作、GPU 和同步交替限制的候选；总 CPU 平均掩盖线程瓶颈 | JIT 正在重复编译、GPU 算力足够跑 60 FPS |
| 异常片段三个小核各约 99.5%，大核几乎不运行；热线程 runnable 排队 38.2%；Present 3.91/s | 必须先解释核心调度与功耗状态，不能拿此段评价呈现补丁 | 仅是 Wine 锁过多或采样器导致 |
| 原始 sysfs 中 GPU `max_freq`：正常 16/16 次为 680 MHz，异常 120/120 次为 348 MHz | 异常时可见频率上限也改变了，低频不只是空闲降频 | 已证明限频由温控、PServer 或具体用户操作触发 |
| ntsync 每 Present 的 futex 尝试 187→189、region 锁 411→427 | 四倍以上掉速没有相应调用量暴增，单次等待/排队更需计时 | 同步成本为零，或它必然是首要瓶颈 |

`schedstat` 的运行时间、就绪排队时间分别统计；不能把排队称为正在执行系统调用。
字段含义见 [Linux scheduler 文档](https://docs.kernel.org/scheduler/sched-stats.html)。
旧日志还出现 thermal zone 55 的约 90,000 原始温度读数，但缺少该 zone 的类型与
异常转换对齐证据，本轮不据此指定温控根因或修改温控。

## Present 配对复算

重新配对 scene1 同一 recording generation 的 `receive/copy_done/complete/idle`，
确认无重复事件；要求本次四个事件和下一次 receive 均存在，得到 998 对。
它们是 X11 请求边界，不是物理显示完成帧。

| 指标 | 复算结果 |
| --- | ---: |
| 本次 receive → 下次 receive，均值 | 41.91 ms |
| 本次 receive → complete，均值 | 8.37 ms |
| 其中 host fence 等待，均值 / p90 | 6.82 / 28.44 ms |
| 本次 complete → 下次 receive，均值 | 33.54 ms |
| fence >20 ms 的请求 | 207/998 |
| 上述 207 对 complete → 下次 receive，均值 | 1.79 ms |
| 本次 fence 等待与本次到下次请求间隔的 Pearson r | -0.403 |

这不证明 GPU 等待有益。它表明：等待长时，后续请求经常已接近可处理；可能有 guest
工作重叠或请求积压。因此不能从 41.91 ms 直接减去 6.82 ms 来预测提速，单纯缩短
host 调用也可能只是把等待移到别处。源码中的阻塞值得改造，但收益必须重新测量。

原始 log SHA-256：`4218cda7578b456b5a02d48456214e295742e2d67c166e71c4dd72cd9603dd73`。
派生记录 `scene1/present-pairs-review-20260928.json` SHA-256：
`e2c9fdcbb28181cc71353b056422171e8ebdfd64b55500e67b48ab6136e82937`。
二者仍位于仓库外的原 performance evidence 目录。

## 优先改进顺序

### 1. 先使功耗/调度行为可解释

- 同窗记录实际 GPU max/current、各 CPU policy、每个关键 TID 的有效 affinity/cpuset、
  runnable 排队、温控/core_ctl 状态，以及前后台和 PServer 操作世代；配置文件不能
  替代运行状态。常规低频采样，状态改变时短时加密采样并测量观察开销。
- 源码发现 `PowerManager.resume()` 无条件调用 `driver.start()`，而
  `PServerDriver.start()` 会进入 baseline 恢复/采集与 babysitter 路径。
  `applyCurrentProfile()` 虽然检查 enable 标志，却不能阻止前述动作。
  应补齐关闭状态的生命周期门控，并检查异步 stop/start 的恢复顺序。
  这是明确的代码审查点，尚未证明它造成历史小核异常。
- 先保持正常系统策略做对照。只有逐线程证据支持时才测试关键线程的大核倾向；不把
  所有 Wine/FEX/audio 线程挤到同一大核，也不以关闭温控或无限自旋作为修复。

### 2. 首选结构实验：保留 GPU copy，异步处理 Present

当前调用链是：`PresentExtension.presentPixmap` 在 WindowManager/content 锁内调用
`VulkanRenderer.copyHardwarePixmap`，再持 Java renderer 锁进入 native；native 取得
`frameMutex` 独占锁和 `renderMutex`，提交 copy 并等待 fence，之后才返回 Complete/Idle。
`renderFrame()` 也依赖 `frameMutex`，因此等待期间会阻挡 host 后续帧处理。
Vulkan 的 [vkWaitForFences](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForFences.html)
本身是 host 阻塞等待，不是 GPU copy 执行计时。

建议实验设计：

1. X11 请求只做校验、获取 AHB 引用、记录 window/pixmap/serial/世代并放入有界队列。
2. renderer 统一管理 queue/command pool，复用 command buffer/fence，先比较两个在途
   slot；需要时才对照三个。每个目标纹理与其 GPU 使用世代一起回收。
3. fence 完成处理在全局锁外进行；保留现有 Complete 的完成条件，Idle 仅在 host
   不再读取该 guest buffer 后发出，绝不在“仅入队”时提前释放。
4. 队列满必须有明确背压；resize、隐藏、销毁和失败要正确清理各代资源，避免以
   无限排队换吞吐、引入输入延迟或重现黑屏。

先补 copy/composite 的 GPU timestamp、queue submit、fence signal/host 返回，以及
实际 SurfaceFlinger present 的关联。查询支持检查 `timestampValidBits/period`，异步读取；
GPU 与 CPU 时钟未经校准不能直接相减。见
[Vulkan timestamp 文档](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html)。

直接采样留作同一实验的第二步。现有 `submitAhbBarriers()` 仍同步等待 fence；
删除 `vkCmdCopyImage` 并没有自动删除 CPU 等待。此前 Hades II 对照也未证明帧率提升。

### 3. 解析 guest 热点，并做低成本 DX11/DX12 对照

- 在稳定场景把最热线程的 FEX 执行 PC 映射到带代码世代的 guest 模块/偏移和函数，
  分清有效游戏计算、忙等、锁和音频；块入口 RIP 不冒充精确采样指令。
  Wine ARM64EC 前端仍需自己的探针身份/传输，不能直接宣称已有 shadPS4 的能力。
- 先用少量计时根与调用点验证热点，再决定是否适合 host thunk、批处理或 FEX 代码生成
  优化。未知 ABI 不做函数替换，默认不开探针并保留采样开销对照。
- 同场景、分辨率与等效画质比较 DXVK/DX11 和 VKD3D/DX12；MHW 已有受控切换重启
  记录，试验成本较低，但当前数据未证明哪条更快。

### 4. ntsync 优化先计时，再改协议

当前 ntsync 已编译为 `aarch64-linux-android` 原生库，并非整段都在 FEX 中执行。
建议增加独立、默认关闭的采样计时：region 锁等待/持有、futex 返回原因、signal 到
waiter 实际运行延迟，并与同一 TID/Present 对齐。不要用会启用 watchdog/timedlock
的 `NTSYNC_DEBUG` 替代低干扰测量。

若证实 region 锁在关键路径，再评估减少重复注册/扫描、合并合法的信号与等待操作、
减少争用；涉及多对象 wait/all、alert、timeout、abandoned mutex 必须保留语义。
不能直接设置 `NTSYNC_NO_SIGUSR1_BLOCK=1`：Wine 挂起持锁线程会使整个共享区域停住。
实现依据见 [固定版本 ntsync 源码](https://github.com/tencentmalos/ntsync-android/blob/7ce6435e5979b1cb5341aa4b299f31e8937fe121/src/core.rs)。

## 试验通过标准

每次仅改一个因素，固定存档位置/视角、暖缓存、分辨率和有效系统策略，做 A/B/A
短轮交错并追加持续运行。记录实际显示帧间隔的 median/p95/p99、输入响应、GPU
耗时/频率、逐线程运行/排队、功耗与温度；Present 次数只作辅助指标。
CPU/GPU 占用升高本身不算收益，固定帧率下省电也可能是合理结果。

第一个实现单元应是“功耗生命周期门控 + 状态记录 + GPU 时间分离”；其后再评估
默认关闭的异步 copy。现阶段没有足够证据给出预期提升百分比或承诺达到 60 FPS。
