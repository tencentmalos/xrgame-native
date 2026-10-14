# Swan：vkd3d 渲染缩放（与 shadPS4 一致，2026-10-13）

> **已移除（2026-10-14）**：渲染缩放不适合 Steam 游戏，按用户要求从主干移除。实现保留在主仓分支 `feature/malos/d3d-render-scale`，见[移除记录](swan-d3d-render-scale-removal-20261014.md)。本记录作为历史保留。

规格见 [D3D 转换层的渲染缩放与纹理质量 v1](../specs/xrgame-d3d-render-scale-texture-quality-v1.md) 的 2.2 节。本记录覆盖 vkd3d-proton（D3D12）在 MHR 上的实现和首轮验证，以及 2026-10-14 补上的 DXVK（D3D11，组件 xrg6）实现；DXVK 的画面还没有在 2D 的 D3D11 游戏上验过。

起因：用户指出之前实现的「只缩放阴影深度数组」与 shadPS4 的 render scale 不一致，要的是整体渲染目标按比例缩小、shader 修正坐标、游戏看到的资源尺寸不变。

## 环境

| 项目 | 值 |
|---|---|
| 设备 | Swan（Pico B3110），`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys` |
| boot id | `6d4335a5-667c-426d-815b-1a82edaa0fbb`（本记录所有运行在同一次开机内） |
| 显示 | 2D 大屏（XR），1280x720 交换链，插帧 2x，90 Hz 请求 |
| 开发期 APK | `2d5e55b6ba6cc3277e48cd66edebee937842d9e3787fdc8c7bd53a75e5d2a833`，DLL 替换进 VKD3D xrg2 组件目录 |
| 打包后 APK | `4b5fa9d83a0e83d08f592eaa11ff1ef06dbf3f723db6a7cca2486d86bc28c956`，`audit-apk` 通过；运行时 bundle manifest `4709964bb2d6c6b13bdb252e195793a90d6fd894bdf668d8baf1636bf3e812b0` |
| VKD3D 组件 | `11.0-212991f-arm64ec-xrg3`，归档 `1987550bb1f733072eb6dd4c5d8dab8537cea65a0c335fc0ebceb9258ce62642` |

### 二进制和补丁

| 版本 | `d3d12core.dll` | 内容 |
|---|---|---|
| 开发 1 | `b5d643b69ab267ec28aef3c15dd8c3ad74e407910a42eb516d8b77f0f2b16dd5` | 资源缩放 + shader 修正，后备缓冲不缩放 |
| 开发 2 | `b41b951e42f89e41095219c5a0e086e1a44d32f5c6fcf88cc8e6a4869840aff2` | 同上，加诊断变量 `XRGAME_RENDER_SCALE_DEBUG` |
| 开发 3 | `749697b900236689efc5c6481d67bfbe4615e5535c6a9c2b3a953ff63ed782b3` | 后备缓冲一起缩放，呈现时拉伸回去 |
| xrg3（`build-d3d.sh`） | `5c3af5e7c6ec631991014c46f38f4def480e496a37494ce99d6353b358cd2049` | 与开发 3 同源；`d3d12.dll` 为 `1ea0241f8e667c4cdbeae500e547c7fe58e8129a52e0489d89bc1622dac6c231` |

开发版在远端开发树里增量编译；xrg3 由 `build-d3d.sh` 在全新复制的固定树上打补丁编译，二进制与开发 3 不同（构建路径不同），所以 xrg3 单独上机复验了一次。

- `vkd3d-xrgame-resource-scale.patch`（基线 `212991f`，含纹理质量）：`4f9760f1e97c56153c600b816e9a79b7ce3c7559109ba5d2aa46d1cf0db0c6cc`
- `dxil-spirv-xrgame-render-scale.patch`（vkd3d 固定的 dxil-spirv `219d770`）：`cb1e470b4ee6b76e3440e8546ac3bbf036faa1377f574d1b6a40fd58d0d2da72`

## 实现要点

- 资源：带 RT 或 DS 标志、2D、单采样、短边 > 64 的纹理按 1/8 档位缩小（8/6/4/3/2）。后备缓冲也缩放。
- 视口和裁剪：按当前绑定的渲染目标缩放；清除矩形、同比例拷贝按比例换算；比例不同的拷贝走 blit。
- 呈现：后备缓冲用线性过滤拉伸到 Vulkan 交换链图像。
- shader（dxil-spirv 补丁）：
  - 纹理描述符写入时，在该槽位的 raw VA 辅助数据里写入标记：低 32 位为应用视图的宽 | 高 << 16，高 32 位为 `0x585247 << 8 | 档位`。不缩放的写 0；单个描述符拷贝靠 `RAW_VA_AUX_BUFFER` 标志带上这个槽位，批量拷贝本来就整段拷贝。
  - dxil-spirv 在 2D 纹理句柄处记下堆索引。`Load` / `Store` 时读出标记，坐标映射为 `floor((c + 0.5) × 实际尺寸 / 应用尺寸)`，偏移先加到坐标上再映射；`GetDimensions` 返回应用尺寸。
  - `SV_Position.xy` 乘 `8 / 当前 pass 档位`。档位放在根签名 push constant 里描述符表偏移之后的一个字，随表偏移一起推送。
- 缓存：渲染缩放档位计入 shader 接口键，切换档位后不会复用旧 SPIR-V。

## 结果

### 标题加载插画出现 2×2 块状，原因与修正

开发 1 在 50% 下，MHR 加载阶段的标题插画出现明显的 2×2 最近邻块状，100% 时清晰。逐项排除：

| 运行 | 设置 | 结果 |
|---|---|---|
| `mhr50-a`（pid 16559） | 低纹理 + 50% | 块状 |
| `mhr50-tqhigh`（pid 17293） | 高纹理 + 50% | 块状，排除纹理质量 |
| `mhr100-base` | 100% | 清晰 |
| `mhr50-dbg2` | 50%，`XRGAME_RENDER_SCALE_DEBUG=2`（不改 `SV_Position`） | 块状，位置正确 |
| `mhr50-dbg1` | 50%，`XRGAME_RENDER_SCALE_DEBUG=1`（完全不改 shader） | 块状，位置正确 |

块状与 shader 修正无关。MHR 最后用点采样把 960x540 的场景拷进 1280x720 的后备缓冲，后备缓冲不缩放时就成了最近邻放大。shadPS4 的做法是最终输出图也缩放，呈现时统一放大。开发 3 改成后备缓冲一起缩放（MHR 为 `1280x720 -> 640x360`），呈现时线性拉伸。之后标题插画变成 50% 应有的柔和，块状消失（`mhr50-bb`）。

### 场景画面

用 DebugBus 注入按键依次关掉离线连接错误（MR-111-61）和自动保存提示，进入 3D 标题场景，再到存档选择界面。没有读档，存档没有改动。

- 3D 标题场景和存档选择界面：布局与 100% 完全一致，景深、阴影、火光、角色都在正确位置，没有错位或重影。
- 画面整体变软，边缘有锯齿，界面文字因按 640x360 渲染而发糊。这是渲染缩放的代价，与 shadPS4 相同。
- 篝火粒子在三档之间不同，是动画帧不同，100% 也有同样的橙色光晕。
- 75% 下文字清晰可读。
- 保存下来的 8 份 logcat 里没有 device lost、GPU page fault，也没有 vkd3d 的 XRGame FIXME（拷贝跳过、渲染目标比例混用、push constant 不足）。其中 4 份只覆盖采样的 30 秒。

### 存档选择界面的内存与负载

同一界面，高纹理，每档单独启动，每 5 秒采样一次，持续 30 秒。

| 档位 | 运行 | KGSL mapped + unmapped | 帧率 | GPU 占用 |
|---|---|---|---|---|
| 100% | `mhr100-saveselect` | 1151 + 2512 = 3663 MiB | 47.7 | 87% |
| 75% | `mhr75-saveselect` | 1159 + 1917 = 3076 MiB | 60.0 | 79%（2 个读数） |
| 50% | `mhr50-bb`（开发 3，pid 22454） | 1186 + 1515 = 2701 MiB | 60.0 | 82% |
| 50% | `run104-rs-apk/mhr50`（xrg3，pid 27899） | 1150 + 1515 = 2665 MiB | 60.0 | 81% |

- 50% 比 100% 少约 0.98 GiB GPU 内存，75% 少约 0.57 GiB。到标题时 vkd3d 日志计 128 张目标被缩放，自报节省 2060 MiB（按应用尺寸估算，含随后释放的资源，不等于常驻差值）。
- 这个界面在 75% 和 50% 下都到了 60 fps，看起来是游戏的上限，所以帧率只说明不再受 GPU 限制，不能用来比较 75% 和 50%。
- 各档分开启动，系统可用内存受当时状态影响，不作比较。
- 100% 和 75% 两次运行没有记下进程 PID。

### 启动面板

xrg3 APK 的 MHR 启动面板：「渲染缩放」在主列表里紧跟「纹理质量」，选项为 100/75/50/37.5/25%，非默认档显示说明文字。VR 游戏不显示这一项（它们用「VR 渲染分辨率」）；原来高级选项里的「阴影渲染缩放」已去掉。截图 `run104-rs-apk/launch-panel.png`。`XrGameQualitySettingsTest` 2/2 通过。

### 其他

- 开发 3 的第一次启动在创建 D3D12 设备前就退出了，logcat 里没有任何 vkd3d 日志。这与之前曲面屏测试时遇到的偶发退出相同，重试正常。
- 测试结束后恢复了容器：MHR 回到测试前的「低纹理 + 50%」。Alyx 去掉了 `XRGAME_RENDER_SCALE`，因为 VR 游戏的面板不显示这一项，留着会改不到。VKD3D xrg2 目录里替换过的 DLL 也已还原。

## 未验证 / 未完成

- 狩猎场景、读档后的村庄、长时间游玩。
- 37.5% 和 25% 两档。
- 图像原子操作没有做坐标映射。
- 缩放图拷到缓冲（CPU 读回）目前跳过并记一次日志，没有先放大。
- 光线追踪和 work graph 的 shader 不做修正。
- 根签名的 push constant 已满时，`SV_Position` 不修正，并记一次日志。
- 呈现时只是线性拉伸，还没有接 FSR / SGSR。
- DXVK（D3D11）渲染缩放的画面（规格 P4）。Swan 上现有的 D3D11 游戏只有 Alyx，而 Alyx 是 VR 游戏，验证需要另找 2D 的 D3D11 游戏。

证据目录（仓库外）：`swan-20261008/swanxr-ahb/run103-renderscale2/`、`run104-rs-apk/`。

## DXVK（D3D11）：组件 xrg6（2026-10-14）

与 vkd3d 同一套档位和判定（规格 2.4）。shader 走 DXVK 的新 IR（dxbc-spirv），在 `DxvkIrLowerBindingModelPass` 里改写：

- 缩放标记放在 push data 的资源槽位里（每个 SAMPLED/STORAGE_IMAGE 槽 4 字节，`DxvkDescriptorFlag::PushData`）；当前 pass 的缩放放在 `DxvkBuiltInPushData` 的 16–19 位。
- 改写的指令与 vkd3d 相同：`SV_Position` 按 pass 档位换算，`Load`/`Store`/原子操作映射整数坐标，尺寸查询返回应用的尺寸。图像记下应用请求的尺寸（`DxvkImageCreateInfo::xrgameRequestedExtent`）。
- 只在缩放低于 100% 时改写 shader；DXVK 的 shader 缓存版本串加上 `+xrgame-rs1-<debug 位>`，与不缩放的缓存分开。`XRGAME_RENDER_SCALE_DEBUG` 的位与 vkd3d 相同。
- D3D11 一侧：后备缓冲也缩放；绑定 framebuffer 时合并各附件的档位；视口、裁剪、清除矩形、拷贝范围按档位换算，呈现时线性拉伸（`XrGameBlitImage`）。

Alyx 闪退（开发版，Alyx 容器临时设 `XRGAME_RENDER_SCALE=50`）：`vkCreateGraphicsPipelines` 访问违例。导出的 76 个 shader 中有 1 个通不过 `spirv-val`：push data 块只有一个成员时 dxbc-spirv 把它当标量，读标记时却按结构体加了成员下标（`OpInBoundsAccessChain … %uint_5 %uint_0`）。修复后读标记前先看 push data 的类型是不是结构体；同一流程导出的 7725 个 shader 全部通过 `spirv-val`，Alyx 进场景并正常退出（画面未评估）。证据 `run105-dxvk-rs/`。

| 项目 | 值 |
|---|---|
| 补丁 `dxvk-xrgame-resource-scale.patch` | `57a6ac935338cf6d6c2dbcd4486f61ec69165c3eedf655344ca2f89aec9621eb`（叠在其余 3 个 DXVK 补丁之上） |
| `d3d11.dll`（`build-d3d.sh`） | `8e16d99aceaff09334c132fc122b4464c082de6aab6e30eaf1c99bd655900d1a` |
| 组件 `dxvk-11.0-a676404-arm64ec-xrg6.wcp` | `f2be1761f4799d0983955036e6a351113503a9fafae8882bc5b8f361c9a6a629` |
| APK | `d0c11ff8435e997ec19a2b1cd9b2e8cde35ddbc3b02a4078ea99afff5bfd732b`（bundle manifest `8b3e4fc7…`），`audit-apk` 通过 |

APK `d0c11ff8` 在 Swan 上跑了一轮无人值守 Alyx（不设渲染缩放）：容器自动换到 `11.0-a676404-arm64ec-xrg6`，进场景稳定，菜单退出正常（`run106-cloud-dxvk6/`）。
