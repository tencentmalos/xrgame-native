# MHW 当前场景性能调查（2026-09-27）

状态：取得冰原据点同一视角的 CPU、Present、GPU busy 和频率样本。
正常运行片段显示 guest 执行/调度与呈现同步共同占用帧预算；另捕获一次主要线程
全部集中在三个小核的严重降速。尚未证明完整关键路径或确定调度变化的触发源。
没有调整游戏画质、进程亲和性、设备频率、功耗或温控策略。

2026-09-28 的 [优化复核](mhw-optimization-review-20260928.md) 重新配对 Present，
并确认异常片段 GPU `max_freq` 也由 680 MHz 降至 348 MHz；来源尚未确定。
后续按调度/功耗、异步呈现、guest 热点和同步计时排序验证。

2026-09-28 的[新场景 profiling](mhw-profiling-20260928.md) 在正常核心调度下复现
约 18.5 FPS，并定位到 surface capabilities/GetGeometry 等待被同步 AHB Present
阻塞。该轮有两份原生 off-CPU 栈对照，不应与本文历史进程合并统计。

## 身份与采集

- AYN Thor / Android 13 / API 33 / 4 KiB；targetSdk 36。
- Build：`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`。
- Boot：`8bb14501-5800-4b9b-a9ab-7173603812f7`。
- Android PID `18530`；MHW PID `27409`；Wine server PID `20249`。
  MHW start-time ticks `128744959`，热门线程 `27430` 为 `128745058`。
- APK SHA-256：`c3c220cd9d4999b031324055a54c63dbdd71fef2801a941be3a83bfae3250055`。
- Catalog SHA-256：`f4f34d00c5e207b65b583f4be1bedcd85a4ffffac6bd7e6c36a9443339c2159f`。
- Build IDs：`libevshim.so` = `5d357cd7a9756016f8b6eb3d1bfc9cc23edb262d`；
  `libwinlator.so` = `e120a35c35b69960ed4fe4c8993045bddffe4c1b`；
  `libvulkan_renderer.so` = `36421bccc5e74ef19b0ff30b47b21ea071507eab`；
  `libxrgame_debugbus.so` = `7182d65276c0f44d49b55fa761b4ca5ceef1b19a`。
- 游戏配置：DX12、1280×720、游戏帧率上限 60、V-Sync Off、
  Variable(Prioritize Resolution)、CAS On；App Present 限帧为 0。

原始文件均在仓库外 `xrgame-native-evidence/mhw/20260927/performance/`。
`scene1` 包含 30 秒 simpleperf、32 秒 Perfetto、1000 次 Present trace 和
54.52 秒墙钟采集；逐线程首尾采样间隔约 50.08 秒。其 shell 逐文件读取较重，
不能把这一轮当作无观察开销的性能基线。`capture-result.json` 的 `start` 字段
实际上在完成时写入；绝对时间应使用 logcat 和原始 trace 时钟，不使用该字段倒推。

## 正常速度片段

| 测量 | 结果 | 边界 |
|---|---|---|
| X11 Present 请求间隔 | 平均 41.90 ms（23.86 次/秒），p90 59.96 ms | 1000 个请求；不是物理屏幕扫描完成时间 |
| 最热游戏线程 `27430` | 约 75.2% 单核 CPU、5.2% runnable 排队 | `/proc/TID/schedstat` 首尾差；不是整个进程 CPU |
| 6 个 Job Thread | 各约 22.6–23.1% 单核 CPU、24.3–25.0% 排队 | 线程相互重叠，不相加为串行帧时间 |
| game + app user CPU 样本 | 79.77% 位于 `[anon:FEXMemJIT]` | 是翻译后的 guest 执行，不等于正在 JIT 编译 |
| 线程 `27430` user CPU 样本 | 93.93% 位于 `[anon:FEXMemJIT]` | 尚未解析为具体 guest 函数 |
| GPU busy | 16 个读数均值 76.9% | KGSL 区间计数；不是按 MHW 分离的 GPU pass 时间 |
| GPU 频率 | 平均 667.8 MHz，多数读数为当前最高档 680 MHz | 这段不能解释为 GPU 一直低频 |
| AHB copy 路径 fence 等待 | 平均 6.82 ms，p50 0.78、p90 28.44、p99 36.23 ms | 含前序队列执行与 host 唤醒，不能称为纯拷贝 GPU 时间 |
| AHB copy 整个调用 | 平均 8.04 ms；锁等待均值 0.055 ms | 999 对原生时间戳；不是 CPU 活跃耗时 |

显示路径中 `copyWindowContentAHB` 提交拷贝后同步 `vkWaitForFences`，再归还
图像/发 Complete。因此存在可测量的同步等待和流水线间隙；尚不能把整个等待都
归因于该拷贝本身，也不能据此宣称去掉拷贝会获得等量加速。

## 严重降速与复核

新增只读 `tools/xrgame/sample-process-load.c`，用一个进程批量读取线程 stat、
schedstat、wchan 和系统计数，避免每个文件启动一次 shell 子进程。
用 NDK 27.3 / Android API 26 编译，`-Wall -Wextra -Werror` 通过；设备运行 60 秒
正常退出，CPU 时间 2.663 秒（约 4.44% 单核，仍有观察开销）。不注入输入或控制进程。

`scene2` 60 秒采样期间观察到：

- CPU 0、1、2 各约 99.5% busy，CPU 3–7 几乎没有运行时间；所以总 CPU 只有
  37.3%，并不意味着当时可调度到的核心有余量。
- 最热线程只在 0–2 上运行，CPU 时间占比 57.1%，runnable 排队 38.2%。
  `AK::EventManage` 线程运行 37.2%、排队 62.1%；尚未定位该音频相关线程的具体函数。
- GPU busy 平均约 31%，频率固定为 348 MHz；Present 平均约 3.91 次/秒。
- simpleperf 只在中间约 20 秒开启；其前、中、后三个窗口均约 4.2–4.5 FPS，
  不能将低速仅归因于 CPU 采样。该轮 3264 samples、0 lost。
- 停采后的截图曾恢复约 21.7 FPS；但随后独立 6 秒复核在采样开始前就再次处于
  只在小核运行/GPU 348 MHz 的状态。**撤回“采样器导致小核限制”的初步推断。**
  这段只能作为异常调度现场，不能与正常片段直接作优化 A/B。
- 检查时游戏 affinity 为 0–7，cpuset 为 top-app，CPU online 为 0–7；没有证据
  支持“容器明确只允许三个核心”。需要在异常转换时同步记录 effective mask、
  governor、thermal/core_ctl 和系统性能服务行为。
- 18:24:15/18:24:25 的日志记录功耗控制关闭及驱动重新初始化；本调查没有执行
  这些设置操作。随后保存的 profile 中功耗控制、自动调频、自动限帧、绑核均关闭。
  不能把当前保存配置反推为全部历史窗口的有效状态。

## 证据完整性与下一步

- 两段 LiteP 原始 PROF SHA-256：
  `616436660f4893a7d60b9dfb2b7612bac203c660f7660824e3721b5654cf2510`（30 秒），
  `1274d30a469fc056ed9dc0199d92673e2308b83d36c9975faf106902409e6b75`（60 秒）。
  使用匹配的 vendored SDK reader 解码，无 skipped chunk / truncated / diagnostics。
  第一段 708 个 FrameMark，第二段按实测窗口截取；FrameMark 来源是 X11 Present 请求。
- Perfetto 有 FrameTimeline，但 sched/ftrace/KGSL batch 均为 0，且 importer 报告
  13 个 negative timestamp 被丢弃。不能借此建立 GPU critical path 或假装获得调度切片。
- 采集和 Present trace 均已结束，profiler recording 恢复 0；没有 live debugger。

优先固定并记录系统功耗/调度状态，解释小核异常；再定位 `27430` 的 guest 热点
及其等待者，分离真实游戏工作、音频线程和同步成本。显示侧需将 GPU 执行完成、
拷贝、host 唤醒分别计时，再比较异步归还/直接采样；不能仅凭总 CPU/GPU 百分比
决定优化方向，也不在本轮改动默认显示路径或接入 Foundation 音频模块。
