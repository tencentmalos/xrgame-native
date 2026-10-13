# Swan：MHR 显存上限 4 GiB 试验与「显存预算」设置（2026-10-13）

## 背景

- Adreno 只有一个统一内存堆。容器的 `maxDeviceMemory=0` 时不设上限，DXVK 的 dxgi 会把这个堆的大小（十几 GiB）当作显存报给游戏。
- 试验目的：把 `dxgi.maxDeviceMemory` 限到 4096，看 MHR 会不会按这个值收缩纹理和缓存，从而压低进标题时的内存峰值。

## 试验

1. 停掉 app，把 MHR 容器 `dxwrapperConfig` 里的 `maxDeviceMemory` 从 0 改为 4096。容器其他设置不变，包括插帧 `XRGAME_XR_FRAMEGEN=2x`。
2. 启动后确认生效：
   - 游戏进程环境变量里有 `DXVK_CONFIG=dxgi.maxDeviceMemory = 4096;dxgi.maxSharedMemory = 4096;`。
   - 前缀里的 `dxgi.dll` 是 DXVK 的，含 `dxgi.maxDeviceMemory` 选项；D3D12（vkd3d-proton）同样经过它。

每 2 秒采样一次，KGSL 内存为 mapped + unmapped：

| 阶段 | MHR GPU 内存 | MHR 驻留内存 | 整机可用 |
|---|---|---|---|
| loading（15:28:25–15:29:12） | 669 + 1320–1344 MiB ≈ 1.99 GiB | 2.58–2.61 GiB | 2.54–2.66 GiB |
| 进标题后约 3 分钟（15:32） | 1195 + 约 2797 MiB ≈ 3.9 GiB | 1.7 GiB，另有 3.4 GiB 已换出 | 1.1–1.4 GiB |

- 对照（不限，上午 12:27–12:28 的采样）：
  - loading 时 MHR GPU 内存约 1.99 GiB，和这次一样；
  - 标题刚出来时约 3.2 GiB，这次停留几分钟后约 3.9 GiB。
- 进标题时，15:29:21 前后 LMK 一次连杀 16 个进程，原因是「critical pressure」和「min watermark is breached even after kill」。HyperHold 换出 SteamPSP uid 的 GPU 内存 2269 页（约 9 MiB）。这次没有出现设备丢失，MHR 留在标题。
- dmabuf 页池在 loading 开始时从 880 MiB 降到 264 MiB，进标题时降到 0。页池先被分配消耗掉了。

**结论**：MHR（RE Engine）不按 DXGI 报告的显存收缩自己的资源，4 GiB 上限对它的内存峰值没有作用。降内存需要游戏内调低画质，或者在转换层降低纹理分辨率（shadPS4 的材质质量做法）。

单位换算：采样脚本用 32 位整数，`gpumem_unmapped` 超过 2 GiB 时会回绕成负数，表里已按 +4096 MiB 换算回来。

## 设置

- 启动面板「高级」里新增 **显存预算**，可选 2G / 3G / 4G / 6G / 8G，默认 4G。
- 存在容器 `dxwrapperConfig` 的 `maxDeviceMemory`（MiB）中。
- XRGame 构建写 `DXVK_CONFIG` 时，统一按 2–8 GiB 夹取；未设置或为 0 时按 4 GiB。这意味着 XRGame 不再有「不限」这一档。
- 实现：`XrGameMemoryBudget`，`DXVKHelper.setEnvVars`。
- 单测：`XrGameMemoryBudgetTest` 3/3，`WindowsVrUpscaleTest` 11/11。
- APK：`71d610f2b914287e8716c06b8476a36ff52b459939996478f000d31869367625`（run98）。

## 证据

证据目录（不进仓库）：`swan-20261008/swanxr-ahb/mem-cap4g/`，包括 `memmon.txt`、`logcat.txt`、`marks.txt`。
