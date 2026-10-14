---
name: swan-memory-reclaim
description: 查看 Swan（Pico 头显）当前内存，并立即回收内核可丢弃的内存（dmabuf 空闲池、可回收 slab）。用户说"看一下内存""清理内存""立即回收""drop caches"，或大游戏（如 MHR）启动前、被 lmkd 杀后需要腾内存时使用。
---

# Swan 内存查看与立即回收

脚本：`tools/xrgame/swan-memory-reclaim.py`（Python 3，需要主机上有 `adb`）。

```sh
python3 tools/xrgame/swan-memory-reclaim.py --serial <设备序列号> --status   # 只看
python3 tools/xrgame/swan-memory-reclaim.py --serial <设备序列号>            # 回收并对比
```

- 序列号取自 `--serial`、`ANDROID_SERIAL`，或唯一连接的设备。本仓库公开，不要把序列号写进仓库文件。
- 需要 root：脚本会先执行 `adb root`。Swan 重启后 adbd 会丢掉 root。
- 回收命令是 `sync; echo 2 > /proc/sys/vm/drop_caches`。

## 输出怎么看

| 行 | 含义 |
|---|---|
| MemAvailable | 内核估计还能给新进程用的内存 |
| MemFree | 完全空闲的内存 |
| Cached | 文件页缓存；`echo 2` 不清它 |
| KReclaimable | 可回收的内核内存，包含 dmabuf 空闲池 |
| dmabuf free pool | dmabuf system 堆的空闲池。`system` 和 `qcom,system` 是同一个池，只算一次，所以 `total_pools_kb` 是它的两倍 |
| swap used | 已换出到 zram 的量；回收不改变它 |

Swan 空闲时，dmabuf 空闲池约 1–1.5 GiB，回收后清零，MemFree 相应上升。MemAvailable 涨得少，因为这部分本来就大多算在可用里。

## 什么时候用

- 启动 MHR 这类内存峰值高的游戏**之前**：
  - 空闲时，`[qcom,system-pool-refill-thread]` 约 3 分钟就会把池子补回 0.5 GiB 左右，所以要紧挨着启动做。
  - 背景：MHR 加载和进标题时，Pico 的 lmkd 会连杀进程，`[xrprocessmanager] POLICY_SWAP_THEN_KILL` 甚至会杀掉前台的 SteamPSP。见 `docs/validation/swan-d3d-render-scale-removal-20261014.md`。
- 排查 HyperHold 把 GPU 内存换到 zram 后出现的设备丢失（`kgsl:restore gmem timeout`）时，先看当前内存。
- 原始测量见 `docs/validation/swan-dmabuf-pool-20261013.md`。

## 注意

- 用户可能正戴着头显在玩。脚本发现 Wine 进程时会先列出来。
  - 回收只丢内核缓存，不动游戏进程的内存，但会带来短暂的 I/O。
  - 不确定时先只运行 `--status`，再问用户。
- 不要循环反复执行。补池线程会把空闲池再补回去。
- 这不会改变设备设置，也不会持久化，重启后恢复原状。
