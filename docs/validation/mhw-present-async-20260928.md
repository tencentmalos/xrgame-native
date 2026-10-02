# MHW：将 AHB copy fence 等待移出 X 请求线程

2026-09-28。根据 [场景 profiling](mhw-profiling-20260928.md) 的 GetGeometry 等待链改造；
Mac 独立构建迁移按用户最新要求暂停。本记录仅覆盖 AYN，不能外推为 Swan/XR 验收。

## 实现与边界

- picoXr 的普通 AHB copy 可选异步入队：X 请求线程只校验、保留 AHB、入队；
  原生渲染线程提交 GPU 拷贝，在不持 `frameMutex/renderMutex` 时等待 fence。
  此时也不持 Java renderer/content 锁或 X WINDOW_MANAGER 锁。
- 渲染线程仍串行执行 copy 与 host render，避免复制中的纹理被另一个 renderFrame
  回收。等待后重新加锁释放命令资源；不跨解锁区间保留 texMap/cache 引用。
- 每个任务独立持有 AHB 引用和 JNI 回调；队列最多 8 项（含正在执行的任务）。
  队列满返回 BadAlloc，不阻塞 X 请求线程，也不提前释放 guest 图像。
- 窗口销毁/取消映射/调整大小使旧任务失效；未提交任务发 SKIP/Idle，已提交任务
  必须等实际 GPU 读取完成才能归还。失败时不发 Complete/Idle，保留资源至设备销毁，
  并以原请求 sequence 报错，停止接收新异步任务。
- 初始默认关闭，通过 `present async_copy 0/1` 做同包对照；切换等已接收任务归还。
  内部 debug 包可用容器环境变量 `XRGAME_PRESENT_ASYNC_COPY=1` 持续启用；
  当前设备只对 MHW 设置该选项，其他容器不改变。
  首轮只接受无 wait/idle X fence、无偏移、Present 限帧为 0 的普通 copy；
  直接采样仍默认关闭，其他形态保持原路径。混合请求在未归还时拒绝同步插队。
  不宣称实现了完整 Present MSC/region/wait-fence 语义。

## 验证方法

相同 APK、游戏进程、原角色存档、冰原据点固定视角，按同步 A1 → 异步 B → 同步 A2。
不调整画质/频率/亲和性、不清 shader cache、不替换游戏或运行库。
每段 30 秒，包含 Present trace、进程线程负载、SurfaceFlinger latency，及中段
8 秒 `simpleperf cpu-clock:u -f 99 --trace-offcpu --call-graph fp`。
SurfaceFlinger 排除首份历史数据；trace 的 fence 等待是 host elapsed，不是 GPU pass 时间。

## 同包 A/B/A 结果

| 指标 | 同步 A1 | 异步 B | 切回同步 A2 |
| --- | ---: | ---: | ---: |
| X11 Present 请求速率 /s | 19.35 | 26.66 | 19.07 |
| SurfaceFlinger 实际呈现速率 /s | 19.39 | 26.65 | 19.09 |
| 呈现间隔 p95 /ms | 100.27 | 50.14 | 100.29 |
| KGSL 区间 busy 均值 | 74.56% | 99.35% | 74.38% |
| GPU current 平均频率 /MHz | 643.17 | 680.00 | 638.50 |
| 系统总 CPU busy | 46.83% | 59.54% | 47.14% |
| 最热 guest TID 8094 on-CPU | 44.59% | 66.89% | 44.80% |
| 约 8 秒采样：该线程 surface capabilities 查询 off-CPU | 2.817 s | 0.142 s | 2.581 s |

三段 GPU max_freq 都为 680 MHz；没有锁频，B 的自动调频也更持续处于上限。
因此这是正常 governor 下整条路径的实测收益，不是固定频率的纯代码成本对比。

相对两侧同步样本的均值，host 实际呈现速率提高 **38.5%**，查询 off-CPU 下降约 **94.7%**。
表中查询等待累加所有原始样本（不是只取前 15 个栈）；最初快看 B 的头部栈得到
0.125 秒，完整聚合为 0.142 秒。数据见 [机器可读摘要](mhw-present-async-20260928.json)。

异步 B 的 842 个完整 trace 均满足 GPU fence 完成后才发 Complete/Idle，0 顺序错误；
另有 3 个采集边界处不完整帧，保留并剔除。请求接收/入队返回在 X TID 7910，
copy dequeue/done/Idle 移到渲染 TID 7907；同步两侧全部在 TID 7910。
从 receive 到入队返回均值 **0.254 ms**、p95 **0.674 ms**；同帧原生 copy fence 等待
均值仍为 **33.89 ms**，但已不占用 X 请求线程。等待变长不等于拷贝成本增加：现在
GPU 更持续繁忙，队列中还包含前序 guest/host 工作，没有 GPU timestamps 不能拆分。

三次 Home → 返回应用 → 点击恢复，均回到同一游戏场景，PID 保持不变；结束时
累计 4438 次异步完成、0 skipped、0 failed，队列有 1 项正常在途。没有发现黑屏、花屏
或明显画面回退。尚未覆盖其他游戏、混合 fence/限帧路径、设备丢失或长时间游玩；
单元测试的取消/失败覆盖不能当成设备故障注入结果。

优化后 GPU busy 接近 100%，说明原本的 CPU/GPU 交替空等明显减少。下一阶段需要
GPU pass/timestamp 或 render capture 才能区分 guest 渲染、host copy 和合成成本；
不再把本场景的低帧率简单归因于 X GetGeometry，也不声称全局 CPU 必须跑满。

## 构建与检查

- NDK 原生构建、`assemblePicoXrDebug` 通过；APK 从清空输出后打包，审计 123 项、0 错误。
- 8 个既有 AHB lease/Sync fence 单测通过；C++ 队列测试验证 fence-held admission、
  容量含在途任务、引用生命周期、窗口 generation 复用、关闭与失败保护。
- 6 个 Present trace 分析器测试通过，覆盖异步 dequeue 早于 JNI 返回，以及禁止早发 Idle。
- ASan/UBSan 联合运行未进入测试 main：本机 ASan runtime 初始化发生递归分配自旋，
  已终止并保存 sample 栈；不能记为 sanitizer 通过。普通 C++ 测试通过。

## 构建身份

- A/B/A APK SHA-256：`685b8ae6bf7cf1513c385ef9a2f0a872d09e53a789a243d41280b60fcb47434c`。
- 组件 catalog SHA-256：`e8619cda78b93fbde4e3f509157476f08db3ecb04749215e1fa4eb885984f353`。
- A/B/A `.so` Build IDs：renderer `9c3da038db47a42197cfe5e8ebf6d3233a5cc316`；
  winlator `e120a35c35b69960ed4fe4c8993045bddffe4c1b`；
  evshim `5d357cd7a9756016f8b6eb3d1bfc9cc23edb262d`；
  debugbus `30a9c69fe7e2e2c13006718a8591f3d8619cdc23`。
- 原始私有证据（含未完成/失败尝试、基线 APK、进程 maps、原始采样、截图）保留在
  仓库外 `xrgame-native-evidence/mhw/20260928/async-present/`，不进入公开仓库。

- A/B/A 设备：AYN Thor，Android 13，
  `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`；
  boot id `8bb14501-5800-4b9b-a9ab-7173603812f7`。应用 PID 6913、MHW PID 8070、
  wineserver PID 7948；各段负载窗口 30 秒，perf 分别 8.033/8.044/8.031 秒；
  前后 stat/start-time 与 maps 均保存，整个 A/B/A 未重启游戏。

## 持续启用版本

同包对照之后补上容器启用参数读取，以及关闭队列时持 dirtyMutex 修改退出谓词，
避免消费者准备休眠时漏掉退出通知。最终版本：

- APK SHA-256：`6f485c301e23b0f2d21f13c38f2409ddbb1c54b37e33e84e539ea9f516c8d383`。
- renderer Build ID：`6a799a63e3b40275cf373d803df361cecb183482`；其余上述库和 catalog 不变。
- 再次清空 APK 输出构建、123 项审计通过；已保留数据安装。
- MHW 重启时 DebugBus 已自动报告 asyncCopyRequested/Active=true，无需重新发送开关。
- 最终包同场景 30 秒独立复核：Present **26.76/s**，SurfaceFlinger **26.77/s**，
  p95 **50.14 ms**，GPU busy **99.59%**；8.038 秒 perf 内最热 TID 18182 的
  surface capabilities 查询 off-CPU **0.140 秒**。这组数属于最终包，不与旧包拼接成 A/B。
- 最终包应用 PID 17019、MHW PID 18163、wineserver PID 18049；同一设备与 boot id。
  844 个完整 trace、0 顺序错误，2 个边界不完整帧保留；simpleperf 144904 samples、0 lost。
  查询时累计 24797 个异步完成、0 skipped、0 failed；包含加载/菜单，不能全部当作 3D 场景帧。
- 设备安装 APK 的 SHA-256 与最终审计匹配；profiler capture_active=0、状态 ready，
  Present trace 余量 0，采集已停止。游戏仍使用异步路径。

## 复现

```sh
clang++ -std=c++17 -pthread -Wall -Wextra -Werror \
  tools/xrgame/tests/present_copy_queue_test.cpp -o /tmp/xrg-present-copy-queue-test
/tmp/xrg-present-copy-queue-test
python3 -m unittest discover -s tools/xrgame/tests -p test_present_trace.py -v
python3 tools/xrgame/debugbus.py --serial <device> present async_copy 1
python3 tools/xrgame/debugbus.py --serial <device> present trace 120
```

每次测试都要重新核对 APK、PID/start-time 与窗口场景；不要复用文中的 PID。
上述版本的持续开关设置在容器环境变量，仅内部 debug 包读取。回退时设为 `0`/删除变量并重启，
或使用 `present async_copy 0` 仅切回当前会话。

2026-09-29 后续：已增加 picoXr 的“编辑容器 → 图形 → 异步呈现（实验性）”开关，
沿用相同容器变量，启动读取不再受 DEBUG 限定；仍默认关闭。
MHR 的独立同包 A/B/A 也观察到相同等待链，见 [MHR 与配置入口验证](mhr-present-async-20260929.md)。
后续记录不扩大本页历史版本的验收范围。
