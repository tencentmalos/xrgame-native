# Swan：dmabuf 系统堆页池占 1.44 GiB，疑似回收不及时（2026-10-13）

Swan 空闲时，dmabuf 系统堆的空闲页池囤了 **1.44 GiB**。这些页已经释放，不属于任何缓冲，只是留在池里等复用。

- 内核把它算作「可回收」（KReclaimable），所以 MemAvailable 已经把它算进可用内存。
- `drop_caches=2` 能瞬间把它清空，可见 shrinker 回调本身能用。
- 清空后，页池在 **3 分钟内自己涨回 492 MiB**。这段时间设备空闲，正在使用的 dmabuf 总量没变。内核里有一个 `[qcom,system-pool-refill-thread]` 线程，应该是它在主动预填充。
- 同一天 MHR 进标题时出现内存峰值：LMK 连续杀进程、系统 thrashing，HyperHold 把前台 XR 应用的 GPU 内存换到 zram，最终导致 XR 合成器 `DEVICE_LOST`。峰值那一刻页池有多大，没有采到。

所以「压力下 shrink 不工作 / 不及时」目前是**疑似**，还没有证实。下面列出已经证实的部分、疑点、强制回收方法和验证计划。

## 环境

| 项目 | 值 |
|---|---|
| 设备 | Swan，B3110，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys` |
| 内核 | `6.12.58-android16-6-maybe-dirty-4k` |
| boot id | `9cbe019b-9d6f-4a49-8a61-062e11b746e0` |
| 物理内存 | MemTotal 15,770,200 KB |
| swap | zram，20 GB |
| vm 参数 | `swappiness=100`，`vfs_cache_pressure=100`，`watermark_scale_factor=150`，`watermark_boost_factor=0`，`min_free_kbytes=16384`，`page-cluster=0` |
| 相关内核线程 | `[qcom,system-pool-refill-thread]`（PID 147），`[dmabuf-deferred-free-worker]`（PID 133），`[kswapd0]` |
| 模块参数 | `qcom_dma_heaps` 在 `/sys/module` 下没有导出 parameters，页池上限和预填充目标无法在运行时调整 |

## 1. 空闲时页池占 1.44 GiB

采样时间 14:39:45。MHR 和 SteamPSP 都已退出，设备停在 Pico 桌面。

`/sys/kernel/dma_heap/heap_pools_kb`：

```text
system:      order9-0 order4-902720 order0-602848
qcom,system: order9-0 order4-902720 order0-602848
```

- `system` 和 `qcom,system` 的数字完全相同，说明两者共用一个池。
  - `total_pools_kb` 显示 3,011,136 KB，是把这个池算了两遍。
  - 实际是 902,720 + 602,848 = **1,505,568 KB ≈ 1.44 GiB**：order4 约 882 MiB，order0 约 589 MiB。
- `/proc/meminfo` 的数字与之吻合：
  - KReclaimable 1,778,580 KB，其中 slab 部分（SReclaimable）只有 263,332 KB。
  - 非 slab 部分 1,515,248 KB，和页池大小基本一致。
  - 页池计入 KReclaimable，所以 MemAvailable 已经把它当作可用。
- 正在使用的 dmabuf（`DMABUF:`）是 2,309,040 KB（2.20 GiB），这是另一块，与页池不重叠。

## 2. 强制回收：echo 2 和 echo 3

用 root 在 adb shell 里依次执行，单位 KB：

```sh
sync
echo 2 > /proc/sys/vm/drop_caches   # 调用所有 shrinker：slab 加 dmabuf 页池
echo 3 > /proc/sys/vm/drop_caches   # 再加上文件页缓存
```

| 时刻 | 动作 | 页池 order4+order0 | MemFree | MemAvailable | Cached | KReclaimable | SReclaimable | DMABUF |
|---|---|---|---|---|---|---|---|---|
| 14:39:45 | 清理前 | 902,720 + 602,848 | 5,018,516 | 7,234,132 | 2,569,608 | 1,778,580 | 263,332 | 2,309,040 |
| 14:39:45 | `echo 2` 刚执行完 | **0 + 0** | 6,665,608 | 7,474,924 | 2,568,852 | 239,696 | 239,484 | 2,309,040 |
| 14:39:48 | `echo 2` 后 3 s | 0 + 0 | 6,661,620 | 7,473,044 | 2,571,080 | 239,260 | 239,244 | 2,309,040 |
| 14:39:48 | `echo 3` 刚执行完 | 0 + 0 | 7,662,376 | 7,515,840 | 1,637,172 | 194,736 | 194,736 | 2,309,040 |
| 14:39:51 | `echo 3` 后 3 s | 0 + 0 | 7,645,476 | 7,506,128 | 1,644,060 | 194,500 | 194,484 | 2,309,040 |

- **`echo 2`**：
  - 页池立刻清零，回收 1,505,568 KB。
  - MemFree 增加 1,647,092 KB（+1.57 GiB）。
  - KReclaimable 从 1,778,580 降到 239,696。
  - MemAvailable 只增加 240,792 KB（+0.23 GiB），因为页池本来就被算作可用。
- **`echo 3`**：
  - 文件页缓存再降 933,908 KB，MemFree 再增加约 0.95 GiB，页池保持 0。
  - 代价是之后文件要重新从存储读回来。
- 两步合计，MemFree 从 4.79 GiB 升到 7.29 GiB，正在使用的 dmabuf 不受影响。
- 结论：dmabuf 页池的 shrinker 回调在被显式调用时工作正常，回收是同步、立即的。

## 3. 清空后 3 分钟涨回 492 MiB

`echo 2` / `echo 3` 之后没有任何操作，设备停在 Pico 桌面空闲：

| 时刻 | 页池（order9 / order4 / order0，KB） | 合计 | DMABUF（KB） | 备注 |
|---|---|---|---|---|
| 14:39:51 | 0 / 0 / 0 | 0 | 2,309,040 | drop_caches 刚结束 |
| 14:42:51 | 14,336 / 487,168 / 1,848 | **503,352 KB ≈ 492 MiB** | — | **3 分钟内涨了 492 MiB** |
| 14:43:03–14:44:04 | 14,336 / 487,168 / 1,848 | 不变 | 2,309,040 | 每 5 s 采一次，共 13 次 |
| 14:45:03 | 14,336 / 487,168 / 1,848 | 不变 | 2,309,040 | PSI 内存压力 some/full 均为 0 |

- 正在使用的 dmabuf 总量前后都是 2,309,040 KB。也就是说，这 492 MiB 不是哪个缓冲释放后进的池，至少采样点上看不到总量变化。
- 内核有 `[qcom,system-pool-refill-thread]`（PID 147）。结合「空闲时涨到一个固定值后停住」的形态，判断是 qcom 系统堆的预填充线程主动从伙伴系统分配页来填池，目标大约 476 MiB 的 order4 页。
  - 这个判断没有看到内核源码确认。
  - 3 分钟内短暂分配又释放的情况，现有采样也不能完全排除。
- 15:00 前后又出现一次：
  - 15:00:46 时，页池降到 66.8 MiB，同时正在使用的 dmabuf 涨到 3,177,200 KB，多了约 848 MiB。也就是有组件一次性分配了约 850 MiB，其中约 425 MiB 取自页池。
  - 15:01:48 时，正在使用的 dmabuf 回到 2,327,584 KB，页池随之涨到 917,992 KB（896 MiB）。这批缓冲释放后全部进了页池，没有还给系统。
  - 15:03:48 时，页池 901,516 KB（880 MiB），PSI 接近 0。
  - 是哪个组件分配的没有查到。当时前台是 `com.picoxr.launcher`。
  - 从 `drop_caches` 到这里只过了 24 分钟，空闲时页池就回到了 880 MiB。
- 第 1 节看到的 1.44 GiB 比预填充水位高得多。多出来的部分，推测来自之前几局 MHR 和我们 app 退出时释放的缓冲，经 `[dmabuf-deferred-free-worker]` 进了池。KGSL 自开机以来的分配峰值是 `page_alloc_max` = 10,400,284,672（约 9.7 GiB）。

## 4. 内存峰值时的事故（14:11，MHR loading → 标题）

| 时间 | 事件 |
|---|---|
| 14:11:24 | lmkd 以「critical pressure and device is low on memory」连续杀进程：`com.picovr.settings`（adj 905）、`com.pvr.seethrough.setting`（900）、`spatial_avatar_service`（800）、`com.picoxr.launcher`（600）、`com.pico.assistant`、`com.bytedance.pico.inputmethod`、`com.picovr.updatesystem`、`com.picovr.firmwareupdate`（200）、`com.pvr.picocast:sink`（149）等。这些进程在 1–3 s 内被系统重新拉起 |
| 14:11:31.99 | lmkd `[HYPERHOLD]GPU write zram` 作用于 memcg 10155（SteamPSP 及其 Wine/MHR 子进程，当时 XR Activity 在前台），回收 16,052 页（约 63 MiB），耗时 32 ms |
| 14:11:32.95 | lmkd 再杀 `spatial_avatar_service`，原因「device is in direct reclaim and thrashing (31%)」 |
| 14:11:32–37 | SteamPSP 的 XR 线程提交被阻塞约 5 s，画面冻结，Pico 帧统计中断 |
| 14:11:37.008 | 内核报 `kgsl:restore gmem timeout`；`vkQueueSubmit` 返回 `VK_ERROR_DEVICE_LOST`，XR 会话结束 |

- 整段日志里没有 GPU page fault。
- 峰值那一刻页池的大小没有采到，所以不能说当时页池没被回收。

## 5. 已证实与疑点

**已证实：**

1. 空闲时页池能占到 1.44 GiB，计入 KReclaimable 和 MemAvailable，但不是空闲内存。
2. shrinker 被显式调用（`drop_caches=2`）时能把页池瞬间清零。
3. 页池清零后，空闲状态下约 3 分钟就涨回 492 MiB，正在使用的 dmabuf 不变。内核里存在 qcom 系统堆预填充线程。
4. 内存峰值时出现了 direct reclaim、thrashing 31%、LMK 连续杀进程，以及对前台应用 uid 的 GPU 内存换出。

**疑点（待验证）：**

1. **压力下是否及时调用 shrinker。** 页池是现成的可回收内存，却出现了 thrashing、连续杀进程和 GPU 内存换出。可能的原因：
   - 在 `swappiness=100` 和 20 GB zram 下，kswapd 和 direct reclaim 优先换出匿名页；
   - 页池 shrinker 每轮回收的量太少，跟不上 MHR 一次性申请约 1.3 GB GPU 内存的速度。
2. **预填充线程是否在压力下仍在补池。** 如果它的停止条件只看水位、对压力反应慢，就会和回收互相抵消。
3. **MemAvailable 把页池算作可用，可能高估了「能马上拿到」的内存。** 依据 MemAvailable 做判断的组件（应用、启发式调度）会因此偏乐观。
4. **HyperHold 对前台 XR 应用 uid 的 GPU 内存做换出。** 恢复时内核 `restore gmem` 超时（约 5 s），直接导致 Vulkan 设备丢失。这一点本身就该反馈给 Pico：前台 XR 应用的 GPU 内存不应该被换出，或者恢复失败不应该让设备丢失。

## 6. 现在可用的强制回收方式

| 方式 | 效果 | 代价和限制 |
|---|---|---|
| `echo 2 > /proc/sys/vm/drop_caches`（root） | 页池加 slab 立即回收，本次 +1.57 GiB 空闲 | 效果是临时的：空闲约 3 分钟就涨回约 0.5 GiB，游戏退出时还会再进池 |
| `echo 3 > /proc/sys/vm/drop_caches`（root） | 在上一行基础上再清文件缓存，本次再 +0.95 GiB | 文件缓存要重新读，冷启动阶段会多一些 I/O |
| 运行时调页池上限或预填充水位 | — | 没有导出的模块参数，需要内核改动 |

我们 app 自己没有 root，不能在正式流程里写 `drop_caches`。在 root 的开发机上，可以先在启动重负载游戏前手动执行一次。

## 7. 下一步验证

1. 在 MHR loading → 标题的切换期间，每 0.5 s 记录一次：页池（`heap_pools_kb`）、MemFree、MemAvailable、`/proc/pressure/memory`、`/proc/vmstat` 里的 kswapd/direct reclaim 和 slab 回收计数、lmkd 和 HyperHold 日志，以及 `qcom,system-pool-refill-thread` 的 CPU 时间。要回答三个问题：
   - 峰值时页池有没有被回收、回收了多少；
   - 内存吃紧时预填充线程是否还在补池；
   - 页池回收和 LMK 杀进程的先后顺序。
2. 同样的流程做两组：启动前 `drop_caches=2` 和不做。对比进标题时是否还会杀进程、是否还会出现 HyperHold GPU 换出和 `restore gmem timeout`。
3. 如果能确认压力下页池没有被优先回收、或者预填充线程在压力下仍在补池，就带着第 1 步的时间线向 Pico 报告。

## 证据

证据目录（不进仓库）：`swan-20261008/swanxr-ahb/mem-audit-1415/`

| 文件 | 内容 |
|---|---|
| `meminfo-full.txt` | 完整 meminfo |
| `drop-caches.txt` | 第 2 节的原始数据 |
| `pool-refill-idle.txt` | 第 3 节的原始数据 |
| `dmabuf.txt`、`dmabuf-full.txt` | `dmabuf_dump` 输出 |
| `dumpsys-meminfo.txt`、`meminfo-spatial-runtime.txt` | dumpsys 输出 |
| `oom.txt` | 各进程的 OOM 等级 |
| `odin3-meminfo.txt` | 对照机 Odin3（24 GB）的 meminfo |
| `reclaimable-1500.txt`、`reclaimable-final.txt`、`dmabuf-gap-1502.txt`、`dmabuf-1500.txt`、`dmabuf-b-1500.txt` | 15:00 前后的观察 |

第 4 节的日志在 `swan-20261008/swanxr-ahb/run97-framegen-starve/user-stuck/logcat-dump.txt`。
