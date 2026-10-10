# Swan：OpenComposite 只拷贝显示区域（2026-10-10）

设备 Swan（Pico B3110，Android 16），游戏 Half-Life: Alyx（546560），OpenXR 模式，Vulkan 合成 + SGSR + 眼动 FDM，输出 100%，Turnip xrg13。

## 问题

- Alyx 通过 OpenVR 提交一张 3670x1699 的纹理，两只眼共用（DXGI 格式 27，R8G8B8A8_TYPELESS，非 MSAA）。
- 按当前分辨率，左眼在 `(0,0)`、右眼在 `(1296,0)` 起，各 1296x1200。纹理右侧和下方都没有用到。
- OpenComposite 默认 `invertUsingShaders=false`。这时 `Compositor::Invoke` 把纹理区域置空，D3D11 路径给每只眼各建一个 3670x1699 的 swapchain，并整张 `CopySubresourceRegion`。
- 结果：每帧拷贝 2×6.24 Mpx，而实际显示的只有 2×1.56 Mpx。这些拷贝在游戏进程的 GPU 上下文里执行，而游戏本身已经是 GPU 瓶颈。
- 显示区域（`imageRect`）原本就按纹理区域计算。我们的桥接也只把眼睛区域加 1 像素边框拷到 AHB，所以其余像素从来没有被用到。

## 修改

`references/opencomposite/patches/displayed-bounds-copy.patch`，由 `build.ps1` 在 mailbox 补丁之后应用，并检查产物里带有标记字符串。

- `Compositor::Invoke` 在调用 `CopyToSwapchain` 期间把要显示的纹理区域交给后端。
- D3D11 后端只拷贝这个区域，向外多拷 2 像素，并放在 swapchain 里的相同位置。swapchain 尺寸和 `imageRect` 都不变，Alyx 调整分辨率时也不会重建 swapchain。
- 以下情况保持原样：
  - shader 翻转路径；
  - 纹理区域非法或为空时，仍整张拷贝；
  - MSAA 纹理仍整张 resolve，之后只拷区域；
  - D3D10、D3D12、Vulkan 和 GL 后端没有改。
- 日志会记录前 8 次拷贝的区域。
- **对比开关**：容器里存在 `C:\gamenative-xr\opencomposite-full-copy` 文件时，恢复整张拷贝。每秒最多检查一次，切换时写一条日志。只用于在同一局游戏里做对比，测试结束后已删除。

| 项目 | 值 |
|---|---|
| OpenComposite DLL | `8a8b65ead757bb0b4d1b2365cb344e70e97e917d9efc911de973443a7e1e636d`（导入 DLL 集合与 `2c3f368b` 相同） |
| 补丁 LF SHA-256 | `1c297d747cad4f476148d08970ef4c779e8e4cbc4a2639dc13825e4504d7b68e` |
| 编译器 | MSVC 19.51（Windows SDK 10.0.26100.0） |
| APK | `b918f442efa632c6ac34068c940d05accc6a67ab718d599063e33795ca441f50` |
| 组件清单 | `49a55d7850a6b72757fdca1a3cb4fe6b53d30e5d4aced17ddea287e323f58145`（bundle-xrg13） |
| `libxrimmersive.so` Build ID | `021caf0babeb92da10b636bf9e64b814d46abe39` |
| 设备 | B3110，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `9cbe019b-9d6f-4a49-8a61-062e11b746e0` |
| 进程 | app PID 27229，`hlvr.exe` PID 30075 |

设备上的 OpenComposite 日志显示，左眼拷贝 `1298x1202 at 0,0 of 3670x1699`，右眼拷贝 `1300x1202 at 1294,0 of 3670x1699`。

## 测量方法

- 用 KGSL tracefs（`adreno_cmdbatch_queued/retired`）记录每次提交的 GPU 开始和结束时间。
- 各上下文的独占时间按优先级计算，依次是 Pico 合成器、我们的合成、游戏。
- 游戏帧率按游戏进程里桥接提交（`hlvr.exe` 线程）的次数计算。
- GPU 上限为 902 MHz（`thermal_pwrlevel=4`）。
- **必须在游戏场景里测**：读档进入 `a1_intro_world` 并恢复游戏后，头显静置。
  - 当天早些时候在主菜单或暂停状态测的数据不能代表实际负载，已作废，包括 `run57` 的预算和 `run58` 的数据。
  - 分别启动新旧两个包做对比也不可靠：即使在同一场景，视角和游戏状态不同，旧包一次 22–29 fps 且 GPU 有 22% 空闲，新包一次 39.5 fps。
- 所以在同一局里交替切换：先测只拷显示区域（B），再测整张拷贝（A），共 3 轮，每种状态采 2 段，每段 8 秒。切换后先等 5 秒。

## 结果（run60，同一局，18:04–18:07）

| 状态 | 游戏帧率 | 游戏每帧独占 GPU 时间 |
|---|---|---|
| 只拷显示区域（B，6 段） | 平均 38.3 fps（36.9–39.1） | 平均 20.31 ms |
| 整张拷贝（A，6 段） | 平均 36.0 fps（35.3–36.6） | 平均 21.90 ms |

- 不计第一段，B 平均 38.5 fps、20.19 ms。
- 每帧节省游戏上下文 GPU 时间约 1.6 ms，帧率提高约 6.6%。每轮 B 都高于前后相邻的 A。
- 我们的合成占 GPU 的比例从 11.7% 升到 12.6%。这是因为新帧变多了，每个新帧的耗时不变，仍约 3.2 ms。

## 场景内的 GPU 分布（run60，只拷显示区域，实时切换 `vr_upscale`）

| 设置 | 游戏帧率 | 游戏 | 我们的合成 | Pico 合成器 | 每个新帧的合成耗时 |
|---|---|---|---|---|---|
| SGSR + 眼动 FDM，100% | 38.4–39.0 | 77.7% | 12.7% | 9.5% | 3.22 ms |
| SGSR，关闭 FDM | 38.0–38.3 | 76.9% | 13.5% | 9.6% | 3.48 ms |
| SGSR + 眼动 FDM，75% | 40.2–40.8 | 82.4% | 9.3% | 8.1% | 2.26 ms |
| 不超分 | 45.1–45.2 | 91.2% | 1.5% | 7.1% | 0.33 ms |

- 整个过程中，游戏每帧 GPU 时间稳定在 19.9–20.6 ms，GPU 空闲不超过 0.2%。
- 在场景里，眼动 FDM 让每个新帧的合成省约 0.26 ms。
- 超分的总代价约 14% 帧率：45.2 降到 38.6 fps。
- 最后已恢复 SGSR、眼动 FDM、100%。

## 尚未覆盖

- 只验证了 Alyx 的 D3D11 路径。没有测其他 OpenVR 游戏、MSAA 纹理、shader 翻转路径和其他图形后端。
- 没有做长时间游玩，也没有在 Alyx 动态分辨率变化时观察画面，比如边缘有没有残留旧像素。
- Android 侧的两处多余拷贝还在：合成结果拷进 swapchain，以及准备 pass。桥接拷贝也还在。
- 「对比开关」是否保留到正式补丁里，等用户决定。
