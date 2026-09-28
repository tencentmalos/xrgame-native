# Monster Hunter: World 启动退出（2026-09-27）

状态：**Steam 客户端、loader 注入和零尺寸 `CreateBuffer` 启动错误已修复；AYN 已通过首次设置、3D 标题画面和开场剧情，并到达角色创建；完整可操作场景与受控重启验收仍未完成。**
Steam App ID 为 `582010`，与 Monster Hunter Rise 不同。此记录不代表游戏运行验收。

## 现象与根因

用户启动后很快返回。10:47 的原始日志已保留；Android 主进程未崩溃。
10:55 使用原 APK、原游戏文件，临时增加 `WINEDEBUG=+seh,+loaddll,+process` 后复现：

1. Wine 成功创建 `A:\MonsterHunterWorld.exe`，加载游戏的原始 PE。
2. `SteamAPI_Init()` 报告无法加载
   `C:\Program Files (x86)\Steam\steamclient64.dll`。
3. 游戏尝试运行 `Steam.exe -- steam://run/582010//`，随后调用
   `NtTerminateProcess(..., exit_code 53)`。Wine 此处按十进制输出。
4. 游戏模块加载日志为 10:55:08.058，退出请求为 10:55:08.986，间隔 928 ms。
   这只是两条日志的时间差，不是帧率或完整进程存活时长。

此时还未进入游戏渲染；随后 X11 连接写入失败是进程退出后的清理现象，不能据此归因
于 Turnip、VKD3D 或 FEX。根因是 picoXr 的源码版 Steam 客户端仅为 Hades II 配置，
MHW 仍直接启动，前端已登录 Steam 并不意味着 Wine 内已有 Steam 客户端 DLL。

## 修复

`XrGameSteamLaunch.clientProfile()` 为 `582010` 的默认/明确选择的
`MonsterHunterWorld.exe` 接入已有 catalog 固定的源码版 GBE 客户端，工作目录为游戏根目录。
客户端与配置写入该游戏的 Wine prefix；不替换游戏 EXE、Steam API DLL 或存档，
不启用 Steamless，也不自动解锁 DLC。用户选择其他工具 EXE 时仍按其选择直接启动。
共享客户端的缺文件提示改为通用 Steam 游戏提示。

## 构建与身份

- 故障 APK SHA-256：`5840b81de3358741bf6d93b83979fff966b5ab9253102da1de778a24df9d468e`。
- 修复候选 APK SHA-256：`be00316211769d5103739ebdf9a6ae3d0c30956853414f8900ff9e5f96c82e89`。
- component catalog SHA-256：`90eaac4a0bb18ecaa1d517f0ea2a3a7544e486efc2cd9cc8114b08a63de056e2`。
- Runtime 仍为 Proton 11 / FEX xrg5 / DXVK xrg2 / VKD3D xrg1 / Turnip xrg5。
  FEX DLL、unixlib SHA 与 Build ID 见 [前一轮运行时记录](personal-p3-debuggers-20260927.md)。
- 相关 APK ELF Build IDs：`libwinlator.so` = `e120a35c35b69960ed4fe4c8993045bddffe4c1b`；
  `libvulkan_renderer.so` = `36421bccc5e74ef19b0ff30b47b21ea071507eab`；
  `libgndownload.so` = `674e470eca24f48e67e09f5329fc7e7a58b98469`。
  完整列表保存在私有 `apk-artifacts.json`。
- 原游戏 EXE SHA-256：`c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`。
- 故障设备：AYN Thor，Android 13，build
  `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`，
  boot id `8bb14501-5800-4b9b-a9ab-7173603812f7`。
- 详细复现 Android PID `20259`，游戏 Unix PID `21384`、Wine PID `0xec`、TID `0xf0`。
  短进程的 start-time ticks 未捕获，不作为调试 attach 身份证明。

`assemblePicoXrDebug` 成功；`XrGameSteamLaunchTest` 6 项与 `XrGameSteamClientTest`
3 项全部通过；APK 审计 122 项、0 错误。构建前删除旧输出 APK 后重新包装。

## 后续实测与第二、第三个阻塞点

AYN 随后重新连接。上述 `be003...` 是早期候选，不能作为最终成功身份：

1. 启用 MHW 的额外客户端 hook 后，旧 loader 将父进程 `LoadLibraryW` 的 ARM64EC 导入
   thunk 地址 `0x7ffe532600` 传给子进程。该地址不属于子进程，导致 `c0000005`，loader
   报错 1067。只关闭额外 hook 则报 `Application load error 3:0000065432`。
2. 新增 `gbe-loader-remote-export.patch`：从子进程映射的 PE 解析实际导出；处理 Wine
   CREATE_SUSPENDED 时 PEB loader list 尚未建立的情况。通过有界映射扫描找到 ntdll，
   使用实际 `RtlExitUserThread` 入口创建引导线程完成 DLL 初始化，然后再次解析
   `LoadLibraryW`。保持现有 loader 生命周期补丁和加载结果检查，不使用父进程导入地址。
3. 中间 validation5（仅模块快照）与 validation6（仅 PEB 引导）仍失败，日志和 APK 保留。
   validation7 的本地复测记录到 `Injected!`（loader 时间戳 921 ms），游戏继续建立 DXVK
   2.7.1/Vulkan device 与 1280×720 swapchain，随后显示：
   `E_INVALIDARG: IDx11Device->CreateBuffer(&desc,pinitvalues?&data:NULL,&pbuffer)`。
   该轮日志没有该次 buffer 的描述符；随后 Guest 调试取得了零长度描述符，见下文。
   **游戏仍未修好，零长度来源当时尚未确定；现已定位到配置路径，见末节。**

validation7 身份：

- APK SHA-256：`42fa2f689fa4f49a772abfbab644e82e315eb4faac389cd97c6e470ae03719f8`。
- catalog SHA-256：`69cf5cfa8ff065a550ab31f1b630cb84ac27418acabb033a76414a9feef5c361`。
- Steam archive SHA-256：`729e1b3bc73447edd2b5ce91492f031743e797c4d9b2da122057597ce8ecda10`。
- Loader SHA-256：`2c743aaa1be40f39a7ff096e62cecafa44fb522b12a42b196254f83b0b365997`。
- Android PID `20534`，MHW Unix PID `21657`。诊断脚本采样窗口为 6 次、每次间隔 5 秒，
  不作为完整进程运行时长；device build/boot id 和其余运行库 Build IDs 同上。

设备上的临时 `WINEDEBUG`、DXVK 日志和 `localSavesOnly` 字段现已按修改前 `.container`
恢复。没有修改账号、游戏 EXE 或存档。原始现场和失败记录位于工作区外
`xrgame-native-evidence/mhw/20260927/`；后续候选、loader 构建来源与诊断记录位于
`xrgame-native-evidence/bundled-runtime/20260927/`，包括 `mhw-dxvk-log/`。
原始日志含云存储 URL，不进入公开仓库。

内置依赖、旧容器修复与 D3D10/10.1 实测见
[运行时验收](bundled-runtime-20260927.md)。用户随后要求先接通 Foundation DebugBus，
[host 诊断入口](../debugging/debugbus.md)已建立；DXVK 失败描述符属于独立 guest 进程，
不能从 host 模块查询冒充取得。随后通过 Wine Guest Debugger 的有界内存读取取得了描述符。

## CreateBuffer 描述符与显示链路（同日补充）

以下使用 DebugBus APK `0fa585cd98aa2cbbeed8b1978ed41ecb4de96f849c2e384ea8aa686bbdf52bc9`，
catalog 仍为 `69cf5cfa8ff065a550ab31f1b630cb84ac27418acabb033a76414a9feef5c361`。
设备 build、boot id 和三项核心 ELF Build IDs 与上文相同；新增 DebugBus 库 Build ID 为
`7d997790eb741cb7e6b72d1f9f2ecff5636ebf54`。未更改游戏 EXE 或游戏 DLL。

在错误对话框停留时，Guest Debugger 读取实际调用栈附近的数据：

| 字段 | 实测值 |
| --- | --- |
| `ByteWidth` | `0` |
| `Usage` / `CPUAccessFlags` | `DEFAULT` / `0` |
| `BindFlags` | `0x88`，SRV + UAV |
| `MiscFlags` | `0x40`，STRUCTURED |
| `StructureByteStride` | `16` |

调用方按渲染宽高计算缓冲区容量；现场的内部渲染宽高与请求宽高均为零。
这是当前 DXVK 拒绝参数的直接原因，不是已证实的内存不足或缺 DLL。
尚不能据此判定 Windows 原生驱动是否容忍同一参数；Wine/DXVK 上的探针不是 Windows 对照。
零尺寸也不能跨接口推论：DXGI 交换链允许以零宽高请求窗口客户区大小。

独立的源码探针 `windows-probe --display-contract` 在同一 prefix 测到：

- 客户区与 `WM_SIZE` 均为 792×473；显式请求 800×500 时，swapchain 描述符及后缓冲纹理均为 800×500。
- 主屏、系统度量、DXGI output 及匹配显示模式均为 1280×720。
- `DISPLAY2` 是未连接输出，当前模式为 0×0。随后 MHW 的 `+system` 跟踪显示其渲染线程查询的是
  `DISPLAY1`，返回 1280×720；不能把未连接副屏直接定为根因。
- 同组 flags/stride 下，16 字节 `CreateBuffer` 成功，0 字节返回 `80070057`。
- `--display-contract-auto` 以零宽高创建交换链，得到 792×473 的描述符和纹理，排除了探针路径的自动尺寸故障。
- MHW `+relay` 跟踪显示自身窗口回调收到 `WM_SIZE`，`lParam=02d00500`，即 1280×720。
  内部渲染尺寸为何仍为零需要继续查；收到回调不等于内部状态更新正确。

显式设定图形配置、指定 `DISPLAY1`、改用 1920×1080、关闭 FEX multiblock 都未消除该错误。
临时 `DirectX12Enable=On` 也未建立成功切换 D3D12 的证据。曾出现只有黑屏的截图，随后确认同样的错误
对话框；按 Enter 后退出不能作为修复或场景验收。HUD 刷新率也不作为游戏帧率。

本轮调试身份与证据：

- 第一轮 MHW Unix PID `12422`，start ticks `126718086`，Wine PID `312`；
  第二轮 Unix PID `23351`，start ticks `126857046`，Wine PID `308`。
  helper 日志分别观测到 1066.278 秒和 201.978 秒的目标后检查；这是调试 helper 时间，不是游玩时长。
- 第三轮 Unix PID `15468`，start ticks `127037113`，Wine PID `300`，Android app PID `14310`。
  helper 目标后检查为 204.638 秒，同样不代表游玩时长。
  三轮均通过 MCP `stop_session` 清理并释放所有权，未插入游戏断点。
- 显式尺寸探针 r3：Unix PID `3831`、Wine PID `564`，host exit `0`；
  EXE SHA `b7ce96bcf78c58733fd274295cbef46b3717fe0bf0eed8625e4cc9b2abb533dd`，
  source SHA `d6de694588057e45d7af1ae823d242a8e899345bf156349d382817aa2a2f662d`。
- 自动尺寸探针 r4：Unix PID `15877`、Wine PID `472`，报告中 `displayContractExit=0`；
  host 等待在 20 秒超时，保留为非完整退出验证。
  EXE SHA `5ad0b2ed2001e53a9c2aff23ea6cb647cd0d3083b7fdf61db0aae48f1ceb68f1`，
  source SHA `ad05a55f21f4fda2f0843efd5a48503799964dd8c3129936e123dd7c17d22a34`。

原始日志、失败截图、少量调试内存及 helper journal 位于仓库外私有
`xrgame-native-evidence/mhw/20260927/createbuffer/`。不发布游戏内存或含账号/云地址的原始日志。
Linux 构建访问已恢复，继续使用 `/data00/xrgame-native`；凭据只在工作站用户私有配置中维护。

原生对照新增 `windows-probe --buffer-contract`：不创建窗口，记录同组 16/0 字节请求、HRESULT、
是否得到 buffer 及 `GetDesc` 大小。先尝试硬件，失败再尝试 WARP，并记录实际 driver type。
它不预设零尺寸必须失败。Windows 主机当前认证失败，因此原生结果仍为空。
该模式 r5 在恢复后的 AYN / DXVK 上记录到硬件 driver type `1`，16 字节成功、0 字节
`80070057`；Unix PID `27463`、Wine PID `476`。host 管道等待 20 秒超时，保留此限制。
EXE SHA 为 `cfcd33433aecc8fe0fe8a774555dfd597e1e73510d438f52b9bd55e10cee920a`，
source SHA 为 `75a224a3a75436d16010925f6b65159d1182cc201a0762921a81fe34acc01a25`。

## 零长度容忍实验：未修复

按用户提出的“零尺寸可能合法”假设，在独立 DXVK 源码副本中加入私有探针：
仅当完整描述符匹配上述 DEFAULT / STRUCTURED / SRV|UAV / stride 16 / CPU flags 0，
且显式设置 `XRGAME_DXVK_ZERO_BUFFER_PROBE=1`，才把 `ByteWidth=0` 临时改成 16。
默认不改变行为；不把这个试验称为完整的空资源语义实现或可发布兼容补丁。

实测在 13:34:54.057 记录到零长度请求和 16 字节替代，原 `CreateBuffer` 检查通过，
随后游戏立即显示 `CreateShaderResourceView(E_INVALIDARG)`，仍未进入菜单。
这证明**仅放宽 buffer 大小不足以修复 MHW**；尚未捕获后续 SRV 描述符，不能声称其具体字段已确定。
也不能用这个结果代替 Windows 原生零尺寸 buffer/view 行为的对照。

- 实验 APK：`ce77f91282bb5c199ac39afe41d84de1428b536302b4c811e11eee5f1041a162`。
- 实验 catalog：`ef328732c0e02f8c5cca99df9e736ed28abf226ce62a70719059151d18acaff6`。
- 实验 DXVK archive：`097d3a523ead7338a9449a9ae7b6883f46c6b8e2854543d56e47b30ebf9b2d9d`；
  `d3d11.dll`：`a26f26445a2cdd098884bc9ffe0910146c2945f7da39b0487b7c53502966944c`。
- ELF Build IDs 与上述 DebugBus APK 相同；APK 审计 123 项、0 错误。
- AYN build/boot id 同上；app PID `23933`，MHW Unix PID `25076`，start ticks `127131667`。
  设备端观测窗口 13:34:35–13:35:26，共 51 秒，含启动与对话框停留，不是游戏运行验收。
- 初版实验 APK `87a242835c803aca784b633fe67728bfc2196be577c3ef9b102ff7ac0d771ac7`
  因包内二级文件清单未同步更新而被安装校验拒绝；没有进入游戏，失败包与日志保留。

实验后恢复 `0fa585...` 原 APK、原 `.container` 和原 DXVK，重新启动再次出现原来的
`CreateBuffer` 错误（app PID `25908`，MHW PID `27038`）。已确认 prefix `d3d11.dll` SHA 恢复为
`95fbdd6812c1996ecde83de6d2de32cdf245b480a2cdbc6eea6a6f1f892dfc82`。
移除了本轮自行生成且内容未变化的 `graphics_option.ini`，保留游戏自带 preset，不修改存档。
实验补丁、来源、编译日志与两版 APK 只留在私有证据目录；主仓默认 bundle 和运行时策略未改变。

## 配置路径定位与修复（14:10 补充）

零长度来自更早的配置路径错误。连续 `+file` 跟踪确认工作目录为 `A:\`，但没有访问
`graphics_option.ini`；`+profile` 随后直接记录：游戏传给 `GetPrivateProfileStringW` 的
文件名为 `L""`，并尝试向空文件名写入 `Resolution=0x0`。
`GetModuleFileNameW` 本身能返回 `A:\MonsterHunterWorld.exe`。
独立 x64 探针的 UTF-8 转换、临时 INI 写入/读取正常；加强 FEX 的 vector/memcpy TSO
并关闭 half-barrier 优化仍复现原故障，没有证据把问题归因于这些同步开关。

只改变启动路径，通过 prefix 内的符号链接将游戏呈现为
`C:\xrgame\games\582010\MonsterHunterWorld.exe`，同一安装目录立即恢复正确配置路径，
游戏生成 `Resolution=1280x720`，不再出现零缓冲区错误。
这证明当前 `A:\` 根目录启动方式与 MHW 的配置路径处理不兼容；未将其扩展为所有游戏、
所有盘符或 Windows 原生环境的结论。无需容忍非法 buffer，也无需替换 DXVK。

用于定位的私有 DXVK 只增加尺寸日志、不改变行为：初建交换链 1920×1080；故障路径随后
请求 `ResizeBuffers(0,0)`，DXVK 正确解析为非零客户区，但游戏内部资源容量仍为零。
目录路径启动后请求变成 1280×720，随后到达“创建存档”界面。
源码正式修复仅为 MHW profile 启用目录别名，用户选择其他工具 EXE 时不启用；
旧别名可在游戏移动后原子更新，遇到真实文件/目录冲突则报错，不覆盖其内容。
游戏文件仍位于原 WP1 安装目录，不复制游戏或修改 EXE/DLL。

对照保留新生成的有效配置，再装回原 APK `0fa585...`：`A:\` 启动仍向空文件名读取配置，
重现原 `CreateBuffer` 对话框。然后安装正式路径修复包，使用原 catalog 和原 DXVK 再验证。

- 正式修复 APK SHA-256：`0e476ca0229f2ad994194ee682f4d16619076d4843e75e8c1460a785eb863e0e`。
- catalog：`69cf5cfa8ff065a550ab31f1b630cb84ac27418acabb033a76414a9feef5c361`，仍为原内置运行时。
- `d3d11.dll`：`95fbdd6812c1996ecde83de6d2de32cdf245b480a2cdbc6eea6a6f1f892dfc82`；
  `dxgi.dll`：`b7165521f0731949d1569bb9c0b716ca02704a03a1c76b1cb442e84b95ff12cb`，设备 prefix 已核对。
- 四项 ELF 的完整字节与 `0fa585...` 相同，Build IDs 见上文；设备 build/boot id 不变。
- 12 项 `XrGameSteamClientTest` / `XrGameSteamLaunchTest` 全通过，含别名迁移、现存目录保护、
  父路径越界拒绝与其他游戏/工具选择；APK 审计 123 项、0 错误。
- 私有日志 APK `fffb1751e24122ddb6f0fe271189921a2e0a2e17cd2c3cd819a504f1ed3604de`；
  临时目录路径 APK `900441448ebc643fecba36425c256ddd4a50015cbfa159aa9d2a7961fc45a5e4`；
  均使用私有尺寸日志 catalog，未进入默认 bundle。
- 有界观测：尺寸日志 PID `4990`，start ticks `127224747`；加强 TSO PID `8260`；
  路径 relay PID `10163`；module/profile PID `13298`；临时目录路径 PID `15597`，
  start ticks `127304831`；恢复原包对照 PID `18633`，start ticks `127336627`。
  各自动首段采集约 40.6–40.8 秒，精确时间保存在私有 identity JSON；不视为游玩时长。
- 本轮 Guest Debugger 会话 `gdbg_069efe3ba0084157aa2a24074bb9c028` 已清理并释放所有权。
  临时 `RelayInclude` 已移除。原始日志、截图、失败包和少量调试内存保留在上述私有目录。

### 正式修复包的界面验证

正式包 `0e476c...` 使用原内置运行时，Android PID `19299`、游戏 Unix PID `20388`，
start ticks `127344489`。2026-09-27 14:09:56.236811 启动；14:22:12.964399 再次采集
相同进程身份，距启动请求 **736.728 秒**（含初始化和提示界面停留，不是游戏帧率基准）。

- 原 `CreateBuffer` 对话框未出现，已显示 CAPCOM 标识和“首次创建存档”提示。
- Enter 未推进该提示；Space 推进到语言设置，按界面提示 F 完成默认语言设置。
- 随后显示 MHW 15.23.00 的森林 3D 标题画面，按键后完成加载并进入开场剧情。
  原始截图分别为 `fixed-privacy-state.png` 与 `fixed-loading-finish.png`。
- 首次提示只说明当前兼容客户端下未找到存档，不据此判断用户原 Steam 云存档不存在；
  没有删除、覆盖或导入用户已有存档。
- 14:23 左右画面出现非本次脚本发出的设备音量操作，已暂停进一步输入，避免与用户争用。
  此时尚未完成正常退出/重启、可操作角色场景、实际扬声器和实体手柄验收。
  诊断配置仍含 `WINEDEBUG=+profile` 和 `DXVK_LOG_LEVEL=info`，应在后续接管时恢复原值。

所有截图、动作记录、`/proc` 身份与日志均保留在私有 `createbuffer/` 目录；
HUD 即时值不作为稳定帧率或游戏性能结论。当前结论是**启动阻塞已修复并到达开场剧情**。

14:24:46 的只读截图 `fixed-observe-only.png` 进一步记录到角色创建界面。
14:25:21–14:25:29 设备上发生了一轮未由本次脚本发出的界面切换和再次启动；
新 MHW Unix PID 为 `24147`，Android PID 仍为 `19299`。
旧进程随后已不存在；不能仅凭旧 PID 消失认定闪退，也不能把这轮计作受控正常退出/重启通过。
已保留 `fixed-exit-logcat-private.txt`、新进程身份及截图，继续保持只读，不与外部操作争用。

后续 [存档与 DLC 复核](mhw-saves-20260927.md) 确认首次存档流程不是用户原存档：
已修复 Steam 身份、Cloud 路由和存档目录，恢复原云文件，并补传真实 DLC 授权。
新包显示 Iceborne 主菜单。通用 `GetAppInstallDir` 现已明确指向游戏安装目录；
实际非根目录启动仍仅对已验证的 MHW profile 启用，不能推断其他所有游戏需要同一补丁。
