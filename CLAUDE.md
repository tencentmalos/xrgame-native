@AGENTS.md

`AGENTS.md` holds the shared facts and constraints; this file is only the Claude entry point.

- 2026-10-02 普通 Android SBS 已在 AYN 显示 Alyx 双眼主菜单与虚拟手；OpenComposite 新 ABI 与 GBE 离线本地通信已修复。双手固定 pose，按键/摇杆从 Android 注入到 Windows OpenXR action 的验证通过。完整游玩、物理手柄与 Swan 尚未验收。见 [SBS 规划](docs/specs/xrgame-native-vr-sbs-v1.md)及[证据](docs/validation/vr-sbs-20261002.md)。
- 2026-10-10 Swan GPU 测量必须在游戏场景里（读档并恢复后）进行，主菜单和暂停状态的数据不算数；对比要在同一局里切换。OpenComposite 原来每只眼都整张拷贝 Alyx 的 3670x1699 双眼纹理，现在只拷显示区域（`displayed-bounds-copy.patch`，DLL `8a8b65ea`）。同局对比：36.0 → 38.3 fps，游戏每帧 GPU 时间 21.9 → 20.3 ms。触发文件 `C:\gamenative-xr\opencomposite-full-copy` 可恢复整张拷贝用于对比，测完删掉。见[记录](docs/validation/swan-oc-bounded-copy-20261010.md)。
- 2026-10-10 Swan Steam 云存档反复冲突已定位并修复（picoXr）：
  - 删除只在批次里声明、从未真正执行，导致 Alyx 的关卡过渡文件被反复下载和删除，每个批次都推高云端变更号。
  - 云端已有相同内容时，上传返回 0 块、提交报告未提交，被我们的严格判定当成了 UpdateFail。
  - 修复：`XrGameCloudRpc` 记录 EResult，真正执行删除（带护栏），0 块按已同步处理。UpdateFail 时可以「保留本地存档并启动」。
  - 进 VR 时自动收起游戏库主面板，退出后恢复。
  - 已挑合上游 3 个 Steam 提交，暂不整体合并上游。
  - 见[记录](docs/validation/swan-steam-cloud-sync-20261010.md)。
- 2026-10-04 图形 API 录制回放：picoXr debug 包用 GFXReconstruct 录制，D3D12 在 API 层录，其他 API 录 DXVK 转换后的 Vulkan；通过 DebugBus `api_capture` 触发，在 Windows 上脱离游戏回放。AYN 上录的 MHR D3D12 帧在 Windows 原生 D3D12 上回放正确。Wine/FEX 下 D3D12 必须用 unassisted 内存跟踪，用 page_guard 会黑屏。回放过程的 RenderDoc capture 能在 renderdoc MCP 中打开；先选定事件、ID 用十进制字符串传入后，图像导出和 draw 检查可用。见 [规格](docs/specs/xrgame-native-api-replay-v1.md)及[证据](docs/validation/api-replay-20261004.md)；Vulkan 层模式、MHW 和其余 MCP D3D12 工具尚未验证。
- 2026-10-09 Swan：
  - Vulkan 合成 + SGSR 超分 + 眼动 FDM（ETFR）在 Alyx 上生效；
  - Turnip xrg12（mesa `malos/main` `04e1d665`，A8XX 默认关闭 LRZ fast clear）在完整 LRZ 下单次约 19 分钟无硬件卡死，尚未做长时间验收；
  - 合成器 GPU page fault 已于 2026-10-10 修复：Turnip 在 A840 上不初始化 bin foveation 寄存器，继承了其他上下文留下的值。复位提交为 mesa `malos/main` `adb7e30a`，出包 Turnip xrg13，游戏进程和 app 进程都已加载；fault 为 0，全设备 300 s 为 0。全 LRZ 长时间游玩尚未在 xrg13 上测试。见[根因记录](docs/validation/swan-xr-composite-faults-20261010.md)；
  - Swan 手柄射线默认修正 pitch +24°，尚待确认。
  - 游戏设置在 XRGAME 构建中默认显示精简页，完整配置在「Legacy 设置…」。
  - 见[记录](docs/validation/swan-xr-composite-20261009.md)。GPU hang 按最高优先级处理，要修复根因，不靠开关绕开。
- 产品显示名已于 2026-10-07 更名为 **SteamPSP**（PICO Space Pro）；图标与维护说明见 [品牌资源](docs/branding/README.md)。包名、签名、存储目录和代码标识保持稳定，历史记录不改名。
- Default to Chinese when talking with the user.
- **主干**（2026-10-09 用户确认，统一记录）：主仓 `tencentmalos/xrgame-native` 为 `malos/main`；Foundation `tencentmalos/foundation` 为 `main`。Foundation 远端默认分支就是 `main`，没有 `master`。
- 修改 Foundation 前必须从当前固定提交切出独立功能分支，不能直接在共享 `main` 上修改，完成后以 `main` 为合并目标（用户补充，2026-09-27）。
- 用户于 2026-09-27 授权将私有 `tencentmalos/foundation` 作为 `foundation/` 子仓接入 DebugBus，替代原先的禁止引入规则。操作与边界见 `docs/debugging/debugbus.md`，不公开发布 Foundation 源码。2026-10-09 用户放开范围：本项目同为学习研究性质，Foundation 的任何模块（upscale、foveation、fsr1、xr 眼动、lite-engine 等）都可编进内部 picoXr 包；仍以子仓链接，不把其源码拷进本公开仓库，APK 只在内部分发。
- **主仓主干为 `malos/main`**：`tencentmalos/xrgame-native` 的功能分支默认从最新 `malos/main` 创建，并以它作为合并 / PR 目标。子仓分支遵循各自约定；commit / push 仍需用户明确授权。
- MHW 的 Steam/loader/根目录配置路径启动阻塞已修复，AYN 到达 3D 标题画面、开场剧情和角色创建；已观察用户进入冰原场景；受控 DX11 → DX12 → DX11 重启、原 3 个角色识别和正常退出已通过，长时间稳定性仍待验。重启链路见 `docs/validation/mhw-restart-20260927.md`。目录别名仅为 MHW profile 启用，不修改游戏文件，不放宽零长度 D3D buffer。见 `docs/validation/mhw-startup-20260927.md`。
- picoXr 必须内置完整运行时（含源码 Steam 客户端、DXVK 与 VKD3D），缺组件禁止出包。x64 Steam 游戏默认配置客户端，旧容器启动时校验并修复运行库；不修改游戏目录或存档。验证范围见 `docs/validation/bundled-runtime-20260927.md`。
- **Turnip 子仓主干也为 `malos/main`**：已从 shadPS4 的 `codex/turnip-xr-fdm2`（`d15b7c01`）建立并推送；迭代分支 `feature/malos/xrgame-wine-icd` 从该基线切出，不能从 Mesa 上游 `main` 起步。2026-09-28 阶段版本已先推子仓再更新 gitlink；已验证运行时仍采用配方的固定基线与补丁，不能把维护分支 rebase 当成新二进制验收。
- Start from [docs/specs/xrgame-native-personal-v2.md](docs/specs/xrgame-native-personal-v2.md). The user revised the plan on 2026-09-26: personal learning, AYN validation now, Swan deferred; the old WP3 engineering/publication remainder is the final optional P5 and does not block runtime work.
- 2026-09-28 新增 [Mac 独立构建规格](docs/specs/xrgame-native-macos-build-v1.md)：用户同日再次明确暂停 Mac 构建迁移（ROI 低）；保留评估，当前优先将 Present fence 等待移出 X 请求线程及全局锁，并做 MHW 对照验证。当前只完成评估和小型编译探针，未迁移完整运行时或授权公开上传二进制。 异步 Present 改造与 MHW 同包对照见 [验证记录](docs/validation/mhw-present-async-20260928.md)，当前按容器显式启用，不视为全游戏/完整生命周期验收。 图形设置新增“异步呈现（实验性）”，MHR 同包对照与 UI/暂停验证见 [MHR 记录](docs/validation/mhr-present-async-20260929.md)。
- Retain the original v1 acceptance gaps; see [the original-spec/observability review](docs/validation/spec-observability-review-20260926.md). D0–D4 expands guest/host debugging and analysis alongside P1–P3. Do not treat shared FEXCore code as an already integrated Wine debugger, or AYN validation as Swan/XR acceptance.
  - Start with P1 (former WP4 on AYN), using the existing embedded runtime and Steam-installed games; complete the remaining Steam usability checks in P2.
  - Do not split work packages into micro-specs.
  - Measure AYN behavior before adding a shim. Swan's Android 16 matrix and XR capability probe wait for P4; do not treat Android 13 plus targetSdk 36 as Android 16 evidence. Measure before replacing the display path in P3.
- Before changing anything under `references/`, read that checkout's own instructions (for example FEX's AI-contribution ban).

- 新引入的直接依赖仓库先 fork 到 `tencentmalos` 再维护；迁移 URL 不升级 gitlink。GBE、Proton Wine、JavaSteam、GFXReconstruct 等已补齐 fork。

- 调试流程：`tools/xrgame/README.md` 的 Device debugging。native app 与 Wine x64 guest 的基础暂停/读取/恢复/清理已验证，见 `docs/validation/wp3-debuggers-20260926.md`；Hades II 已通过 Turnip AHardwareBuffer/X11 路径进入实际场景并响应移动（GPU 拷贝，无 CPU 读回，保留 present-wait）；完整游戏与 WP3 仍未验收。

- 当前 AYN 的下载/续传/修复/目录识别和独立公共 depot 对照见 `docs/validation/personal-p2-downloads-20260927.md`；guest 软件断点可命中但恢复仍失败，默认关闭，不能把基础 attach/inspect 通过扩大为完整 D2。
