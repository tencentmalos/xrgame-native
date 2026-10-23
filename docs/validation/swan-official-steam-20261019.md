# 官方 Steam 大屏模式：Swan 上的子进程超限、残留进程与退出状态（2026-10-19）

分支 `feature/big_screen_rendering`，基于 `2f74248a`（v14 的源码），含本轮未提交改动。用户要求：大屏模式（官方 Linux Steam）下修复三个问题——子进程超限导致游戏无法启动（用 Hades II 测）、杀应用后子进程没清理干净、从快捷菜单退出游戏后 Steam 状态不对——并直接在 Swan 上验证。

**结论：三个问题都已在 Swan 上复现并修复。** 过程中还发现 Swan（Android 16）上官方客户端根本起不来的四处 seccomp/SELinux 差异，一并修了。最终包上 Hades II 从官方 Steam 启动、运行、经快捷菜单退出：子进程峰值 29（修复前 33，触发系统查杀），无幻影进程查杀；退出后 6 s 内游戏链全部回收，Steam 显示「开始游戏」并完成 AutoCloud；关掉 Steam 窗口后客户端 3 s 内正常退出。

## 构建与设备

本机 Windows 的 WSL Ubuntu 24.04（`/root/xrgame-native`，与 Windows 工作区同为 `2f74248a` 加未提交改动）。Swan 上的旧包是本机 debug 证书签名，Odin 3 上的 v14 是另一台机器的证书，不能覆盖安装，所以在本机重新出包。运行时 bundle 与 Linux Steam 输入（rootfs、gpu.tgz、PRoot、`lsteamclient.so`）取自 Odin 3 上 v14 APK（SHA `72ba6e5e…`，与[大屏记录](big-screen-gpu-20261017.md)一致）；APK 打包时去掉了 `libproot.so`/`libxrgame_steam_session.so` 的调试符号，staging manifest 按包内文件重算。客户端 shim 用 WSL 的 `aarch64-linux-gnu-gcc` 13.3 按 `stage.py` 的参数重编。OpenComposite 用 Swan 现装的 `25509aaf…`（`WindowsVrPayloadManager` 校验的就是它），pin 只在 WSL 克隆里改，仓库未改。

| 项目 | 值 |
|---|---|
| 最终 APK SHA-256 | `4f8b4a4ca6822df9e5e14fea75092d138c7c0f4e32331ccbbce82c378b7cee8b`（`picoXrDebug`，原生 `RelWithDebInfo`，含 capture SDK） |
| 审计 | `tools/audit-apk` 149 项、0 errors |
| 运行时 catalog | `23d2f72e…`（同 v14：Proton `linux-steam1`、FEX xrg5、DXVK xrg7、VKD3D xrg4、Turnip xrg16） |
| Linux Steam staging manifest | `6f13430e…` |
| 客户端 shim `libsteampsp-client-compat.so` | SHA `f58db7b1…`，Build ID `b2d548e8688f8e3cf40cc305a30b049c5c713fe9` |
| `libextras.so` / `libwinlator_11.so` / `libxrgame_debugbus.so` / `libproot.so` | `bd2432ee…` / `db0927ed…` / `e1a991e3…` / `220a30b1…` |
| 设备 | Swan B3110，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，内核 6.12.58，boot id `69df66bf-d29e-4a0b-b384-89599ba4a88d` |

中间共出了 14 个本地包，逐包的哈希、截图、logcat、PRoot 详细日志都在私有证据目录 `big-screen-swan-20261019/`。用户在头显里登录了官方客户端，凭据由用户输入，记录里没有任何账号信息。

## Swan 上客户端起不来：四处 Android 16 差异

Swan 之前从没跑过官方模式（app 数据里没有 `linux-steam/`）。首次启动依次卡在：

1. **硬链接被拒。** Xephyr 用 `link()` 建锁文件，Swan 返回 EPERM（`Linking lock file (/tmp/.X1-lock) in place failed`）。Odin 3/Thor 上允许硬链接。先试过 PRoot 的 `--link2symlink`，可以过锁文件，但最终改为 shim 兜底：`link`/`linkat` 遇 EPERM 时对普通文件做 `O_EXCL` 复制（保持「只能创建一次」的语义）。PRoot 参数未改。
2. **`setuid`/`setgid` 返回 ENOSYS。** Xephyr 跑 xkbcomp 前在子进程里 `setgid(getgid())`/`setuid(getuid())`，失败就退出，键盘映射编不出来（`XKB: Failed to compile keymap`）。在 app 上下文里实测（Xephyr 包装脚本里的探针）：两个调用都是 ENOSYS。shim 让「不改变任何 ID」的调用直接成功，改变 ID 的仍交给内核。
3. **`get_robust_list`/`set_robust_list` 返回 ENOSYS。** Steam 主进程启动约 1 s 后在 `steamclient.so` 里主动断言崩溃（SIGSEGV 写 0 地址；反汇编看是 `syscall(100)` 后的检查，字符串 `futex robust_list not initialized by pthreads` / `is corrupt`）。Steam 自己的跨进程 IPC 锁读取线程的 robust futex 链表；glibc 的 `set_robust_list` 同样被拒，所以内核从来没登记过链表。shim 接管 libc `syscall()`：遇到 ENOSYS 时给每个线程一个与 glibc 布局相同的空链表头（`futex_offset` = -32，头前一个字是指回头部的 `robust_prev`，Steam 会检查它）。由于 steamclient.so 绕过预加载符号（效果等同 `RTLD_DEEPBIND`），shim 还在每次 `dlopen` 后把各对象的 `syscall` PLT 槽改指向自己。内核本来就不处理这条链表，效果等同没有 robust futex 清理的内核：持有 Steam IPC 锁的进程崩溃时，锁不会被内核回收。
4. 定位第 3 点时加了只用于诊断的崩溃记录：`linux-steam/crash-trace` 文件存在时，shim 在 Steam 自己的信号处理器之前截获 SIGSEGV/SIGBUS/SIGILL，打印 pc/lr/地址和所在映射后交还。Steam 的 breakpad 在 PRoot 下无法 ptrace 已被跟踪的进程，所以没有 minidump。`linux-steam/proot-verbose` 文件给 PRoot 设 `PROOT_VERBOSE`。

此外，Swan 上的游戏是从 Odin 3 直接拷来的，JavaSteam 的 `app_info` 表为空，官方库导出不到 appmanifest（`shared library manifests=0`），Steam 看不到 Hades II。Custom 模式只凭完成标记判断已安装（`SteamService.isAppInstalled`），所以导出也补上这类游戏：`common/` 下有 `.download_complete` 且 install 目录在 `steam_app` 里唯一对应一个非 DLC 应用的，按同样规则导出。

其余排查过的方向：SVE（Swan 有、Odin 3 没有）在 PRoot 下的多线程向量/拷贝压力测试没有问题；`process_vm_readv/writev` 在 app 上下文可用；开着 `--link2symlink` 时出现过一次 `POKEDATA workaround stub got signal 5`，去掉后没再出现。

## 问题 1：子进程超限（幻影进程）

Android 对所有 app 的子进程合计设上限（`max_phantom_processes`，Swan 上未设置，默认 32；`settings_enable_monitor_phantom_procs` 也是默认开启）。超出后按「父进程优先级低、存在时间长」的顺序杀，我们的子进程都挂在主进程下，最老的就是 PRoot 根进程。

修复前（同一包，未折叠 webhelper，13:11–13:12）：Hades II 启动后共 **33** 个子进程，13:12:12 系统 `Killing PhantomProcessRecord {…libproot.so…}: Trimming phantom processes`，Steam 以 137 退出，游戏随之 `guest_terminated`。

| 组成 | 修复前 | 修复后 |
|---|---|---|
| PRoot、launch-client.py、Xephyr、steam | 4 | 4 |
| 会话总线 | 2（`dbus-run-session` + `dbus-daemon`） | 1（launch-client.py 自己托管 `dbus-daemon`） |
| steamwebhelper | 8 | 5（浏览器、GPU、network、storage、renderer） |
| Steam 启动链（`sh`、`pw-audio-namespace`、`reaper`、launch.py） | 4 | 4 |
| Wine（含 Hades II 自己的 `crashpad_handler.exe`） | 16 | 15（不再拉起 `tabtip.exe`） |
| **合计** | **33**（被杀） | **29** |

- webhelper：shim 在浏览器主进程启动时追加 `--no-zygote`，并把 `AudioServiceOutOfProcess` 合并进 Steam 已有的 `--disable-features`（Chromium 只认最后一个同名开关）。`NetworkServiceInProcess2` 也试过，会让 Steam 前端 JS 停在初始化前（5 min 无 `SteamApp Init` 日志），关掉注入后 17 s 就绪，所以默认只折叠 `zygote,audio`。诊断文件 `linux-steam/webhelper-fold` 可改选（空文件表示保持 Steam 原样）。
- Wine：官方启动时 `WINEDLLOVERRIDES` 加 `tabtip.exe=d`（Wine 的触屏键盘助手，Android 输入法负责文字输入）。
- 修复后实测：GPU 绘制开（13:32–13:34，2 min 以上；13:55–13:56）和关（13:37–13:38）各一局，峰值都是 29，安装最终包后到测试结束 logcat 中幻影查杀 0 次。
- 余量 3。还能省的：官方启动命令里的内层 `start.exe /d … /exec` 及它带出的 `conhost.exe`（Custom 模式是直接运行程序、靠启动器的工作目录），以及会话总线本身；这轮没动，以免改变工作目录语义。临时进程（会话检查、输入法粘贴、`linux_steam launch` 的转发）会短暂占用名额。

## 问题 2：子进程没清理干净

两处原因：

1. **PRoot 忽略 SIGTERM。** PRoot 的事件循环把 SIGTERM/SIGINT 等设成 `SIG_IGN`。服务停止时 `Process.destroy()` 不起作用，3 s 后 `destroyForcibly()` 只杀 PRoot 本身，被跟踪的 Steam、webhelper、Xephyr 脱离成孤儿继续运行（PRoot 没设 `PTRACE_O_EXITKILL`）。系统按幻影上限杀掉 PRoot 根进程时同理。
   - 新增 `LinuxSteamProcesses`：先让 Steam 自己退出（`steam -ifrunning -shutdown`，等 8 s），再按 `/proc` 的父子关系杀掉 PRoot 的整棵树，并清扫所有 `HOME` 为客户端目录的进程（孤儿已不在树上）。客户端自行退出、服务启动前也会清扫一次。输入法粘贴和启动转发的 PRoot 子进程也改为整树结束。
   - 实测：`linux_steam stop` 后 1 s 内无残留，日志 `client shutdown: forward=… exited=true`、`client tree stopped: signalled=0`（Steam 正常退出，无需补杀）；重启前的一次停止补杀了 3 个残留（`signalled=3`）。
2. **关掉窗口后客户端还在跑。** 应用进程有前台服务，移除任务（关窗、划掉）不会杀进程，官方 Steam 在没有任何页面的情况下继续运行。复现：`am stack remove <Steam 任务>` 后 10 个客户端子进程一直在。现在服务在 `onTaskRemoved` 后检查是否还有显示 Steam 的任务（启动页、大屏页、官方游戏页），没有就按上面的流程停掉客户端；如果此时游戏还在跑，等游戏结束再停。实测：移除任务后 3 s 内只剩应用进程本身。

`am force-stop` 和对主进程 `kill -9`（模拟崩溃/低内存查杀）都在 2 s 内由系统整组清掉了子进程，这两条路径本来就没有残留。

## 问题 3：快捷菜单退出后 Steam 状态

三处原因叠加：

1. **启动没成功时 Steam 一直显示运行中。** 桥接只在「游戏窗口出现过、之后 X server 视图消失」时回 `finished`；Android 侧启动失败、用户在启动页按 Home 离开等情况下什么都不回，launch.py 要等 5 min 超时，期间 Steam 认为游戏在运行。现在按游戏会话的生命周期判断：会话（X server 视图或 Wine 环境）出现过又消失并持续 1.5 s，就回报——游戏窗口出现过回 `finished`，否则回 `failed`。
2. **首次登录后共享存档失败。** 存档链接（`compatdata/<appid>/pfx` → 现有容器）只在客户端启动时建立，而首次启动时还没登录、拿不到账号就跳过了。登录后启动 Hades II，Steam AutoCloud 先把云存档下载进它自己新建的 `pfx/drive_c/users/steamuser/…`，随后我们建链接时被保护逻辑拒绝：`Failed to setup wine: A separate Steam save prefix already exists`。现在客户端运行中一拿到官方账号就立刻共享存档；并且当 Steam 预建的树只含 AutoCloud 存档目录里的文件、且与容器内容一致（或容器缺失的）时，把缺的补进容器（不覆盖）、原树改名为 `pfx.steampsp-autocloud-<时间>` 保留，再建链接。有任何内容不同仍然拒绝；真正独立的前缀（含系统文件）不会被接管。本机这次预下载的 3 个存档与容器一致，已被接管。
3. 快捷菜单退出本身：退出后 6 s 内 Wine 和 Steam 启动链全部回收，Steam 日志 `Game Recording - game stopped` 并依次 `Removing process`，AutoCloud 经共享链接找到 3 个存档，未改动跳过上传，`Upload complete`；DebugBus `steamCommand` 清空；页面回到 Steam 大屏，Hades II 页面显示「开始游戏」、「Steam 云：已是最新」、最后运行「今天」（软件绘制截图 `sw-3-after-exit.png`）。

## 官方启动的启动面板

用户反馈 JavaSteam 侧的启动面板在官方模式下没有了。官方启动经 `LAUNCH_GAME` 意图进入，三个入口（冷启动的挂起请求、延后处理、`ExternalGameLaunch` 事件）都直接调 `launchIntentApp`。现在统一经 `launchHandedOverApp`：官方游戏且开启了启动面板时先显示面板，「启动」继续原外部启动流程；「取消」结束这次官方启动（桥接回 `failed`）并回到 Steam 大屏。Swan 实测面板显示 XR · 2D 大屏、尺寸、分辨率、透视、限帧等，点「启动」后正常进游戏。

## 输入法：组字实时上屏

主线完成后按用户要求处理下面「已知问题」里原来的输入法一条：Pico 输入法每按一个字母都 `setComposingText`，到词尾才 `commitText`，而接收器只在提交时把文字发给 Steam，打字时输入框不变。

- `IMEInputReceiver` 新增构造参数 `streamComposition`（默认关，游戏里的行为不变），只有 Steam 页面的 `SteamImeSession` 打开。打开后组字每变一次就同步给 Steam：和输入框里已显示的组字比较，用退格删掉公共前缀之后的部分，再补打其余部分（`IMEInputReceiver.replacement`，按码点计数）。`commitText` 用同样的差分替换组字，所以自动纠正、拼音转汉字只改不同的字符；`finishComposingText` 保留已显示的内容；关掉键盘时未提交的组字留在输入框里（与 Android 输入框的默认行为一致）。输入法那侧看到的编辑区内容和原来相同。
- Steam 输入队列 32 → 256：一次替换会连发多个退格，常排在一次中文粘贴（`ime-text.py`）后面。
- Pico 输入法转发自己不处理的按键（实测为方向键）时只转「按下」：它自己的日志里 `onKeyDown`/`onKeyUp` 都有，我们的 `sendKeyEvent` 只收到 `action=0`，X 里的键会一直按着。Steam 页面现在在按下时就发一次按下 + 抬起，忽略抬起（`SteamImeSession.forwardsKey`）。
- 调试：DebugBus 新增 `steam_ime`（仅 debug 包），像输入法一样调用 Steam 页面的输入连接，测试串由调用方给出、返回里不含文字。见 [DebugBus](../debugging/debugbus.md)。
- 单测：`IMEInputReceiverTest` 15 项（新增差分、自动纠正、拼音逐段提交、组字中退格、回车与关键盘后不重复 5 项）、`SteamImeKeyTest` 1 项，全部通过。

设备验证（Swan，boot id 同上；ime3 `1ed2a0be…`，软件绘制，app PID 19936，14:56:43–14:57:01）：用户在 Steam 搜索框里用 Pico 输入法打英文和拼音。日志（只有长度）：英文 4 次 `setComposingText`（长度 1→4）各打出 1 个字符，输入框逐字出现；切到中文时 `finishComposingText` 保留已显示的内容；拼音组字（长度 1、3）先出现在输入框里，选词 `commitText`（2 个汉字）时退格 3 次、粘贴 2 个字符。逐帧截图显示最终内容没有重复或残留字符。用户确认中英文输入都正常。

- 截图方式：头显画面有畸变。当时 scrcpy capture SDK 对 2D 页面的截取（`steampsp.panel`）只画 Android 自己的 View，不含 X server 的渲染 Surface，截出来只有键盘按钮；所以改为在 PRoot 里直接读 Xephyr 根窗口（1280×720，证据目录 `xshot.py`），每 2 s 一帧、去重保存。同日晚些时候 SDK 已补上 X server 画面，见下文「截图与录屏」。
- 测试途中输入框的光标每约 10.4 s 被移到开头一次。来源是同时在跑的 GPU 崩溃复现脚本：它用 `adb shell input keyevent` 依次按下、右、上、左，在 Pico 上经 PxrMRService 以 `vr2d_key_event` 注入（deviceId -1、source UNKNOWN），不是系统或用户的操作。这暴露了上面「只转按下」的问题。
- 未由真人在设备上覆盖：自动纠正、组字中退格、回车、组字未提交时关键盘。这几项由单测覆盖。

证据（截帧、只含长度的 IME 日志）在私有证据目录 `ime-swan-20261019/`。

## GPU 绘制：Zink 段错误与黑屏

主线修完后发现：GPU 绘制下 GPU 进程每局几分钟内段错误（Zink，`libgallium-26.3.0-devel.so`，调用方 `libGLX_steampsp.so.0`，访问 0xd8），Chromium 重启 GPU 进程后大屏一直黑屏。用加了诊断打印的 Turnip（只在测试时换进设备）复现，原因如下。

1. 宿主呈现（`turnip-x11-host-ahb.patch`）让 Android X server 按窗口尺寸分配 AHB，客户端 Turnip 原来把交换链图像直接建在导入的 dma-buf 上（线性，显式 pitch）。
2. Turnip 把线性图像最后一级的行数向上取整到 4 的倍数（`fdl6_layout_image`：GMEM 按 16×4 块拷贝会越过末行，所以多分配几行）。gralloc 只按请求的行数分配，再按页取整。窗口高度不是 4 的倍数、页尾余量又补不齐时，导入的内存比 Turnip 要的小，`x11_host_create_mem` 返回 `VK_ERROR_OUT_OF_DEVICE_MEMORY`，`vkCreateSwapchainKHR` 报 `VK_ERROR_INITIALIZATION_FAILED`。实测：一个 722 行的窗口（gralloc 行距 1344 像素）buffer 为 3,883,008 字节，Turnip 要 3,892,224 字节（724 行）。
3. 已有窗口换交换链失败时，Zink 调 `kill_swapchain`，把窗口纹理换成普通纹理（`obj->dt` 为空），GLX drawable 却仍当窗口有效。接着 `glXSwapIntervalEXT` 进 `zink_kopper_set_swap_interval`，那里只有 `assert(res->obj->dt)`（release 下不生效）就解引用：这就是 0xd8 段错误。
4. GPU 进程重启后，同一窗口（高度没变）第一次建交换链就失败（`zink: could not create swapchain`），drawable 无效，从此不出画面：这就是一直黑屏。kopper 没有重建已杀交换链的路径。

所以问题取决于窗口高度和 gralloc 的取整，不是 A840 特有的。Odin 3 的 GPU 模式记录里没出现，应是窗口尺寸凑巧，但没有在 Odin 3 上复测。

修复（只进官方客户端的 glibc Turnip/Zink，即 `build-gpu.sh` 的组件；游戏用的 Android Turnip 和共享的 `malos/main` 没改）：

- `turnip-x11-host-ahb.patch`：宿主图像改为 buffer blit。应用渲染进 Turnip 自选布局的普通图像，每次呈现由 WSI 拷进建在宿主 dma-buf 上的 `VkBuffer`，行距取宿主 buffer 的 pitch。buffer 的大小是精确的，不加填充。这是 Mesa 给 PRIME 用的同一套机制（`WSI_SWAPCHAIN_BUFFER_BLIT`），Android X server 和 DRI3 协议都不变。代价是每帧多一次 GPU 拷贝（1280×720 约 3.7 MB）。
- 新补丁 `mesa-zink-killed-swapchain.patch`：交换链被杀后，`zink_kopper_set_swap_interval` 直接返回，`zink_kopper_query_buffer_age` 返回 0，不再崩溃。这只是兜底，被杀的 drawable 仍不会自己恢复。

验证（Swan，boot id 同上）：

- 诊断版（旧 WSI 加打印）：复现脚本每 10 s 按一轮方向键，第 28 轮（约 5 分钟）出现上述失败。GPU 进程 `exit_code=11` 重启后报 `could not create swapchain`，大屏黑。
- 确定性测试：在 PRoot 里对 Xephyr 建窗口、建交换链、清屏并呈现 60 帧（测试程序 `wsi-host-test.c` 在证据目录）。
  - 旧 WSI：1300×722、1280×721、640×481 建交换链失败（-3）；1280×720、1280×723 成功。
  - 新库：这 5 个尺寸，加上 1281×722、333×333、1920×1081，全部成功，每个尺寸 60 帧都呈现成功。
  - dmesg 中 kgsl fault 为 0。（当时用系统截图看到测试窗口的颜色出现在 SteamPSP 面板上。按用户之后定的规则，头显输出不算证据；画面以下文 SDK 截图为准。）
- 官方客户端 GPU 绘制回归（草稿构建的库换进设备，15:17:57–15:43:37，约 25.7 分钟，脚本每 10 s 按一轮方向键）：
  - GPU 进程从头到尾是同一个（PID 12989），没有 `CreateSwapchainKHR` 失败、没有 `swapchain killed`、没有 `GPU process exited`。
  - 每 2 s 在 Android X server 上列一次 GPU 覆盖窗口，期间出现过 1282×721 的窗口（高度不是 4 的倍数），正常。
  - dmesg 中 kgsl fault 为 0。
- 安装包 `steampsp-gpu1` 上的 Hades II（15:48:33–15:51:12）：官方 Steam（GPU 绘制）启动，点启动面板，游戏运行约 2 分钟（Hades2.exe 在跑，子进程峰值 29），然后快捷菜单退出。6 s 内游戏链全部回收，Steam 日志 `Game Recording - game stopped`，页面回到 LinuxSteamActivity。Steam 的 GPU 进程（PID 19034）从头到尾是同一个，没有交换链失败，kgsl fault 为 0。
- 配方产物（`build-gpu.sh`，WSL）：`turnip-x11-host-ahb.patch` `d54f981e…`，`mesa-zink-killed-swapchain.patch` `dc8c2a01…`。Build ID：`libvulkan_freedreno.so` `0f7ebb23d961fe69bb1e876fe8534c39837ad6b8`，`libgallium` `35bfa56e84f1aeb9037311c8de853d14766feaa0`，`libGLX_steampsp.so.0` `0abb961d…`（与 v14 相同）。上面两项设备测试用的是同一份源码的草稿构建（`libgallium`、`libGLX` 逐字节相同；`libvulkan_freedreno.so` 因构建目录不同而不同，Build ID `d5d72638…`）。
- 安装包 `steampsp-gpu1`：基于 `3bb321485`（输入法提交）加本记录的未提交改动，`gpu.tgz` 换成上面的配方产物，其余输入同上文。

  | 项目 | 值 |
  |---|---|
  | APK SHA-256 | `285a58643b7d3e14d50b08f8bcc7d1d4400ca7bd50f4ccc527f536d2555b8c91`（`picoXrDebug`；`tools/audit-apk` 149 项、0 errors；picoXr Steam 单测 43 项全过） |
  | 运行时 catalog | `23d2f72e…`（同上） |
  | Linux Steam staging manifest | `3a632337…`；`gpu.tgz` `6dacf38a…`，`gpu-build.json` `03b1ea31…` |
  | 原生库 Build ID | `libextras.so` `bd2432ee…`、`libwinlator_11.so` `db0927ed…`、`libproot.so` `220a30b1…` 同上；`libxrgame_debugbus.so` `0e5fc1bd…`（输入法提交加了 `steam_ime`）；客户端 shim 同上 |
  | 设备 | Swan，boot id 同上，app PID 22926 |

  装包后 app 按新的 `gpu.tgz` SHA 整体重装 `/opt/steampsp-gpu`，设备上三个库的 SHA 与配方产物一致，测试时留下的旧库备份也随之清掉。在这套组件上重跑确定性测试：9 个尺寸（另加 1282×721）全部成功。

### 截图与录屏：补上 X server 画面（scrcpy capture SDK）

用户随后报告 GPU 绘制下「左下角有一块一直闪烁的区域」。要看清画面，头显输出不能用：系统截图和 Pico 录屏随头部朝向变化，没人戴时只有黑屏或透视画面（实际只录到天花板）。SDK 的 2D 窗口截取（`steampsp.panel`）原来只画 Android 自己的 View，X server 的 SurfaceView 是个洞。用户要求先补齐 SDK，并定下规则：截图录屏一律走 SDK，不再拿头显输出（已写入 AGENTS.md、CLAUDE.md）。

- 改动：X server 的两种渲染器都实现 `CaptureTap`，只在截取进行时工作，细节见 [DebugBus](../debugging/debugbus.md) 的截图与录屏一节。
  - Vulkan（GPU 绘制）：第一次打开时以 `TRANSFER_SRC` 重建 swapchain，每帧把呈现的图像拷进 3 块线性 AHB 的环，等 fence 后交出。
  - GL（软件绘制）：每帧 `glReadPixels`。
  - `XrPanelCapture` 把最新一帧画在窗口下面，被挖空的区域用 `DST_OVER` 补上，盖在上面的按钮仍在上层。
- 包 `steampsp-cap1`：SHA `6ca1da1075e113a60fbd8e9fd87db3bfd2af955655a4c12e0278f366f2e01c39`，`libvulkan_renderer.so` Build ID `092af95b…`，审计 149 项 0 errors；其余同 gpu1。
- Swan（GPU 绘制）：SDK 截图 `cap1-panel-1.png`（1920×1080）拍到了完整的 Steam 大屏页面，右侧键盘按钮在上层。SDK 录像 20 s、610 帧（30 fps）。
- 软件绘制的 GL 截取没有在设备上测。
- 闪烁排查：
  - 退出游戏后，Steam 大屏窗口下有一个 1282×721 的子窗口（层级键 `12,5,1`）每隔 1–3 s 销毁重建一次，每次都对应一个新的宿主覆盖窗口。
  - 在这段时间里用 SDK 录 20 s：609 帧里只有 17 帧有轻微变化，左下角没有可见闪烁；重建的窗口从没呈现过内容。停在页面上的另一段 20 s 录像也一样。
  - 所以 X server 交给 SurfaceView 的画面是稳定的。闪烁不在 Steam 页面上，而在游戏里，见下一条。
- 闪烁在 Hades II 的 2D 大屏（XR）会话里，与官方 Steam 的 GPU 绘制无关。
  - 容器设置：`XRGAME_XR_UPSCALE=fsr1`，`XRGAME_XR_FOVEATION=eye`（balanced），`XRGAME_XR_EYE_BUFFER=maximum`。
  - SDK 录 `canvas`（2250×1264，45 fps，10 s）：营地场景里左下角间歇出现一块随机噪点矩形，约 384×96 像素，即 Wine 屏幕 1280×720 上 (0,647)–(218,701)，正贴在 Wine 任务栏上方。462 帧里有 28 帧出现，连续时每 3 帧一次，像一块没写入的显存。同一配置停在主菜单 10 s，0/460。
  - 游戏 X server 上的窗口 15 s 内没有变化，没有落在这个位置的临时窗口。游戏进程也没有加载 Windows 版 Steam 覆盖层。所以噪点在 XR 合成画出的大屏图像里。
  - 用户在头显里确认：关掉 FSR 就不闪了，问题出在 FSR1 加眼动注视点（ETFR）的组合。同一局里的开关对比没做成（两次都在对比前退出了游戏）。
  - 用户决定之后再查。

### 键盘按钮与手柄射线拖动

用户反馈了两点：键盘按钮挡住了 Steam 游戏页的设置按钮；Pico 手柄射线（默认的触摸模拟模式）拖不动画布。原因是触摸被当成「按住鼠标左键拖动」，而 Chromium 按住鼠标拖动不会滚动列表。

- 键盘按钮：放在右侧 30% 高度（原来在中间）。kb1 曾支持拖动并记住位置，kb2 按用户要求改回固定位置，只响应轻点。
- 触摸手势（`LinuxSteamActivity.TouchPointer`）：
  - 轻点：单击。
  - 拖动：超过两倍 touch slop 后转成滚轮，每 40 个 X 屏幕像素一格。按起手时的主方向锁定轴：竖向用 4/5 键，横向用 6/7 键。内容跟手走，指针停在按下的位置。
  - 按住 0.4 s 不动：按下左键，之后拖动的是指针，用于滑块和拖放。
  - 双指：照旧开关键盘。
- Swan 验证（包 `steampsp-kb1`，SHA `c61365aed6b4ac4495b66f9079f1bcab9cd80c654010651b78c456aea02b7797`，审计 0 errors；GPU 绘制，SDK 截图）：
  - 默认按钮中心约在 (1840, 324)。
  - `input swipe` 向上拖，主页向下滚了约一屏；在新闻行向左拖，这一行横向翻页。
  - 拖动按钮能移到 (1700, 160)，拖回后保存为 (0.958, 0.299)。
  - 轻点按钮能开关输入法（`mInputShown` true/false）；轻点「好友」标签能切换。
- 没测：按住后拖动（滑块），以及真人用手柄射线的手感（测试用的是 adb 注入的触摸）。

### 新装游戏首次官方启动「闪退」；超分默认关闭

用户报告《我在地府打麻将》（App 3444020，18:41 经官方 Steam 新装）闪退。

- 游戏其实没有启动。19:08:15 在启动面板点了启动，19:08:21 我们的准备步骤抛出异常：`Failed to setup wine: Existing save container is not ready`（`SteamSaveSharing.prepareContainer` → `linkDirectory`）。XR 会话随即关闭，Steam 在 19:08:24 记为游戏结束。
- 原因：官方启动要在 Wine 启动之前，把 Proton 的 `drive_c/users/steamuser` 链到这个 Wine 的 `drive_c/users/xuser`。新容器从模板来，还没跑过，没有 `drive_c/users`，链接目标不存在，检查失败。Hades II 的容器早在 Custom 模式下跑过，所以没碰到这个问题。
- 修复：`linkWindowsUser` 在链接前先建空的 `xuser` 目录，Wine 首次启动时会把它填满。新增单测 1 项：从没跑过的容器能建出目录并链上，第二次启动保留游戏写入的文件。
- 启动面板的超分默认改为关闭（`WindowsVrUpscale.Settings.filter = OFF`，用户要求）。已存的选择照旧生效；这个游戏的容器里本来就是 `off`。
- 包 `steampsp-kb2`：SHA `f529b2c4acb827265910aa19f54cf7dc58ad9cb233300553396e05065c7d5d28`，审计 0 errors，picoXr 单测 56 项全过（含 `WindowsVrUpscaleTest`）。
- Swan 验证（GPU 绘制，SDK 截图）：19:20:07 从官方 Steam 启动，`Demonic Mahjong.exe` 正常运行。观察 1 分多钟，没有启动错误，画面停在游戏自己的 DLC 公告页。新容器出现了 `steamuser → xuser` 链接，Wine 也填好了 `xuser`。

### 键鼠游戏默认打开 XR 指针模式

用户要求：《我在地府打麻将》这类默认用键鼠操作的游戏，进 2D 大屏时直接用手柄射线当鼠标。

- `ImmersiveXrActivity` 加载会话设置时，如果容器是 Steam 游戏（`STEAM_`）、JavaSteam 缓存的 `controller_support` 为 none，而且不是 Windows VR 游戏，就默认打开指针模式；原来的切换手势仍然可以切回去。
- 设备缓存里，3444020 是 none；Hades II、MHR 是 full，保持原样；Alyx 是 none，但它是 Windows VR 游戏，不受影响。
- 包 `steampsp-kb3`：SHA `0a84c042947717847985ab1fdb208711f3cfb613be1fe13c163c071bf24b1fba`，审计 0 errors，单测 50 项全过。
  - 换用 `bundle-turnip17`（catalog `97a05f69…`），因为 `bc17c7733` 把默认 Turnip 改成了 xrg17。
  - `gpu.tgz` 按新的 mesa pin `55186f1f` 用配方重出：`libvulkan_freedreno.so` Build ID `c88acd9c…`，`libgallium`、`libGLX` 同上。
- Swan：19:41:10 从官方 Steam 启动《我在地府打麻将》，`Demonic Mahjong.exe` 运行，DebugBus `quickmenu status` 报告 `pointerMode: true`。日志在 19:41:24 记下 `XR pointer mode enabled for a keyboard-and-mouse game`。真人用射线操作没测。

### XR 会话只显示状态层

用户的系统截图（20:14:01）显示，2D 大屏上方有 XR 状态层，大屏左上角还叠着 Android 的性能面板（`PerformanceHudView`，FPS、CPU、RAM、温度）。后者是 `PrefManager.showFps` 打开后加在 X server 视图上的 View，在 XR 会话里随覆盖层一起进了头显。用户要求 XR 2D 大屏和 XR 模式只显示状态层。

- `XServerScreen` 由 `ImmersiveXrActivity` 托管时（`immersiveHooks` 非空，即 2D 大屏和 Windows VR），不再添加 `PerformanceHudView`。
- XRGAME 构建的 XR 会话里，快捷菜单的「性能 HUD」开关改为读写 `PrefManager.xrgameStatusLayer`。`ImmersiveXrActivity` 的 HUD 同步随之显示或隐藏状态层，这个开关不会变成无效开关。非 XR 页面照旧。
- 包 `steampsp-kb4`：SHA `38a0e02b7cc6b762ee17614bae3b1c0c9e7c4eb51716c8363dd171b6d64580d7`，审计 0 errors，输入同 kb3。
- Swan：先经快捷菜单退出正在运行的游戏，再装包。20:20:25 重新启动《我在地府打麻将》：
  - `vr_hud` 报告 `visible: true`（游戏 60 fps、XR 90 fps）；
  - `dumpsys activity top` 中 `ImmersiveXrActivity` 的 View 树里已经没有 `PerformanceHudView`；
  - 指针模式仍为 `true`。

### XR 指针模式：在牌上按下拖不动

用户反馈：《我在地府打麻将》里用手柄射线抓牌，在牌上按下拖不动；先在牌旁边按下再划过去，反而能拖起来。

- 原因：指针模式把扳机当成手指，合成触屏事件交给 `TouchpadView`。容器默认是触屏模式（XR/XRGAME 构建），它的手势逻辑在手指按下时只移动光标、不按鼠标键，手指移出 50 像素（`LONG_PRESS_MOVE_TOLERANCE`）后才「开始拖动」：在按下的位置补发左键，紧接着把光标移到当前位置，两者在同一时刻送进 X server。游戏在同一帧里收到按下和移动，按光标的当前位置判断点中什么。在牌上按下，判断点已在 50 像素外，落空；在牌旁边按下再划过牌，判断点正好落在牌上，所以能拖。
- 修复（`ImmersiveXrActivity`）：
  - 射线的事件改为手写笔事件（`TOOL_TYPE_STYLUS`）。`TouchpadView` 对手写笔按真正的指针处理：按下立即在该点按左键，按住时移动，抬起时松开。与触屏/触控板模式及手势配置无关。
  - 不按键时，最后按过键的那只手（起初是右手）的射线以悬停事件移动游戏光标，按下前游戏已经知道光标在牌上，也能显示悬停效果。悬停直接交给 `TouchpadView.onHoverEvent`，因为 Compose 的 `pointerInteropFilter` 只转交触摸事件。
  - 按住时射线移出大屏，移动和抬起都夹到大屏边缘，不再丢掉；关闭指针模式或手柄位姿失效时补发抬起，鼠标键不会一直按着。
  - 按下、抬起各记一行日志：`Immersive: pointer down|up hand=… at (x,y)`。
- 包 `steampsp-kb5`：SHA `88714cf0c59e16e6bc9cc2c5667771ab384b766d79da7857b81f4c2975a826c8`，审计 0 errors，输入同 kb3（`bundle-turnip17`，catalog `97a05f69…`）。
- Swan：21:11 装上 kb5。装包前只查了有没有 Wine 游戏在运行，没注意到用户正在官方 Steam 里重新下载这个游戏；装包停掉了应用和 Steam 客户端，打断了下载。之后两次 DebugBus 启动只得到 `Game not installed`（游戏已被用户删除重下）。用户下载完后在头显里测试，确认牌能正常拖动。日志里 21:24:00–21:24:12 有 14 次 `pointer down`，按下和抬起成对出现，都是右手。按住时间 0.14–1.09 s；按住较久的几次，抬起点离按下点约 25–40 像素，即拖动。

### Alyx 官方启动提示「需要网络」：lsteamclient 缺 SDK 1.65 接口（2026-10-20）

用户反馈：从官方 Steam 启动 Half-Life: Alyx，进不去，提示需要网络。

- 现象：09:27 启动，`hlvr.exe` 建好 Vulkan 设备和窗口，09:28:10 前后退出。Alyx 自己的 `-condebug` 日志最后两行是 `CNetworkSystem::Init() m_SteamAPIContextClient.Init ( ) failed (no Steam?)` 和 `Need Steam Networking sockets available to connect locally. Is Steam running?`。所谓「需要网络」是 Steam API 初始化失败，不是联网问题。
- 原因：
  - 官方模式下游戏的 Steam API 走 Proton 的 `lsteamclient`（PE `lsteamclient.dll` → Unix `lsteamclient.so` → 官方 Bionic `libsteamclient.so`）。Source 2 的 `CSteamAPIContext` 一次取一组接口，任何一个为空，`Init()` 就失败。
  - Alyx 当前版本的 `networksystem.dll` 请求 `SteamClient023`、`SteamUser023`、`SteamUtils011`、`SteamNetworkingSockets013`、`SteamNetworkingUtils004` 等；`steam_api64.dll` 还有 `SteamInput007`。
  - 我们的 lsteamclient（proton-wine pin `5d0d333e`）等于 Valve Proton 的 `336d8d72` 加 GameNative 的改动（`owned_dlcs` 等），最高只到 SDK 1.64：SteamUtils010、SteamNetworkingSockets012、SteamInput006。`create_win_interface` 不认识 `SteamUtils011` 和 `SteamNetworkingSockets013`，返回空接口，`Init()` 随即失败。Hades II 和麻将用的是旧接口版本，所以不受影响。
  - Valve Proton `proton_11.0` 用 `f9d6f6e4 lsteamclient: Support SDK 1.65.` 加上了这些版本；`0745bfbc` 修 32/64 位 `networking_message` 结构布局。`SteamNetworkingSocketsSerialized004`（Alyx 自带的 `steamnetworkingsockets.dll` 用）Valve 也不支持，Alyx 在 Linux Proton 上照样能运行。
- 另一个问题：官方线的运行时包（`bundle-turnip17`）里的 Proton 是 `linux-steam1`，它从 xrg3 派生，没有 xrg6 的 `VK_EXT_host_image_copy`（Alyx `-vulkan` 进场景要用）和 xrg7 的新线程信号屏蔽修复。
- 修复：
  - `tools/xrgame/patches/lsteamclient-sdk165.patch`：`f9d6f6e4` 和 `0745bfbc` 应用在 pin 的 lsteamclient 上，无冲突；`owned_dlcs` 改动保留（`STEAMAPPS_INTERFACE_VERSION009` 原本就没有）。补丁之后 `wine-linux-steam-client.patch` 照常能打。
  - Unix 侧：`linux-steam/build-wine-client.py` 先打 SDK 1.65 补丁，再打原补丁，构建记录增加 `sdk_patch_sha256`。产物 `lsteamclient.so` `667304c4…`（打包时 strip 为 `dfd1e955…`）。
  - PE 侧：新配方 `tools/xrgame/build-lsteamclient-pe.sh`，在云端已配置的 Android Wine 树里增量编译两份 `lsteamclient.dll`。补丁前用同一棵树重编，结果与 xrg7 包里的哈希一致（`c810eed1…`、`687b3e83…`），说明树能复现。补丁后为 `aarch64-windows` `742b9bbe…`、`i386-windows` `3b29b619…`，环境 `environment.sh` `f6583fcb…`，与 bootstrap 构建相同。树里的 Makefile 写死了 `ccache`，镜像里没有，配方用只转调编译器的垫片代替（不影响产物）。
  - `package-proton-validation.py --release linux-steam2`：以 xrg7（`a0de997e…`）为底，换上这三份 lsteamclient，并核对两边的构建记录。`derivedFrom.patch` 仍为 `wine-linux-steam-client.patch`（`LinuxSteamLauncher` 靠它识别官方运行时），另记 `patches`。包 `proton-11.0-2-arm64ec-linux-steam2.wcp` `2bf62c8e…`。
  - `bundle-steam2`：`bundle-turnip17` 只换 Proton，catalog `fed4d3ff…`。
- 包 `steampsp-kb6`：SHA `dec6c079251309cefd81e786d6b4dcdeb98d475c935d9e160ce4c385e54b25a3`，审计 0 errors，含 kb5 的指针修复。Linux Steam 组件里留档的 `lsteamclient.so` 和构建记录也换成了新版本（运行时加载的是 Proton 包里那份）。
- Swan（boot `69df66bf-d29e-4a0b-b384-89599ba4a88d`，fingerprint `…/20260922015006:user/test-keys`）：
  - 09:54:31 装上 kb6。首次启动时应用装上新的 Proton（`Installed bundled archive: proton-11.0-2-arm64ec`）。
  - 第一次由我经 DebugBus `linux_steam launch 546560` 发起，没有起来：
    - 10:01:02 设置里打开了 Pico 的 IPD 调节窗口（用户当时戴着头显），OfficialSteamGameActivity 被切到后台。
    - 10:01:20 启动准备做完，要拉起 ImmersiveXrActivity 时应用还在后台，这一步丢了。回到前台后停在转圈页面，没有起任何 Wine 进程。
    - 这是独立的问题（启动准备期间被切到后台会卡住），修复见下文 kb7。
    - 另外，DebugBus `launch <appId>` 在官方模式下进的是 Custom 游戏库首页，不会转给官方 Steam。
  - 用户关掉转圈页面，从 Steam 里点 Play：
    - 10:10:35 Alyx 启动（`hlvr.exe` PID 23346）。控制台日志依次出现：
      - 10:10:36 `[Networking] Got ISteamNetworkingSockets user interfaces from steam client`、`Network System Initialized`；
      - 10:10:57 `[SteamNetSockets] AuthStatus … OK`；
      - 10:11:09 `[VR] Submitting first frame to the compositor`；
      - 10:11:38 `Spawn Server: a1_intro_world`（读的是 `s0/autosave`，来自先前 DebugBus 留下的一次性读档参数）。
    - 4 分钟时 `game_state`：`inScene: true`，未暂停。状态层：游戏 47.5 fps，XR 90.1 / 90 Hz，GPU 99%，输入为 XR 手柄。
    - `/proc/23346/maps` 里有 Proton 目录的 `lsteamclient.so`（`dfd1e955…`）和官方 `androidarm64/libsteamclient.so`。Alyx 容器的 `system32`、`syswow64` 中 `lsteamclient.dll` 分别为 `742b9bbe…`、`3b29b619…`。
    - SDK `xr` 左眼截图：`alyx-sdk165/1015-alyx-in-scene-xr-left.png`（`44481248…`），场景画面正常。
  - 10:26 用户从 Steam 启动 D3D11 版本：`Network System Initialized`，10:27:08 `AuthStatus … OK`，进入主菜单（`Spawn Server: startup`）。
  - 上面第一次启动卡在转圈页，修复（kb7）：
    - 原因：`MainViewModel.launchApp` 在启动准备（`restoreSteamApi` 等，这次约 5 s）做完后直接 `startActivity` 拉起 ImmersiveXrActivity（Windows VR）或 2D 大屏。当时应用已在后台：10:01:03 Activity 进入 onStop，10:01:20 才调用 `startActivity`，超出 Android 后台启动的宽限期，这次启动被系统丢弃。10:01:22 Activity 回到前台，但没有任何代码再发起启动。系统日志在调查时已滚掉，没留下 BAL 拦截的那一行。
    - 修复：新增 `startWhenResumed`，在发起启动的 Activity 回到 RESUMED 后再调用 `startActivity`（`lifecycle-runtime-ktx` 的 `withResumed`）；Activity 本来就在前台时立即启动。需要等待时记一行 `Launch of … waits for the launching activity to return to the foreground`；等待期间 Activity 被关掉时，放弃这次启动并记录。两条沉浸式分支都改了，`LaunchApp`（同一 Activity 内导航）不受影响。
    - 包 `steampsp-kb7`：SHA `f4e3de9d00f094e8224313391e2036e5658194a8b3bb2e6f10d7f24fd510ca0c`，审计 0 errors，输入同 kb6。
    - Swan 复现（boot `06ddeeff-c3f7-46ca-8a3b-221d8ef96e6d`；设备约 10:24 重启过，之后 adb 需重新 `adb root`）：
      - 10:31:03 装上 kb7。装包前的检查没有拦住：Steam 为 Alyx 起的启动链（`reaper SteamLaunch AppId=546560` 等）还在，装包把它们一并停掉了。
      - 用户从 Steam 点 Play 并在面板上点「启动」，脚本在交接后立即以 root 拉起 IPD 窗口。时间线：
        - 10:47:16.7 面板交接；
        - 10:47:18.0 OfficialSteamGameActivity onStop；
        - 10:47:19–21 `restoreSteamApi`、ACF；
        - 10:47:22.8 `Launch of STEAM_546560 waits for the launching activity to return to the foreground`（前台是 `EtIpdStageActivity`）；
        - 10:47:30.9 Activity 回到前台（IPD 窗口由用户关掉，早于脚本预定的关闭时间）；
        - 10:47:31.1 ImmersiveXrActivity 创建，`hlvr.exe` PID 18649 启动；
        - 10:47:54 Alyx 控制台开始写日志，`game_state` `session: true`。
      - 这次从 onStop 到准备完成只有 4.8 s，没有证明旧代码在这么短的间隔里一定会失败（10:01 那次失败的间隔是 17 s）。但它证明了：被盖住时启动会等待，回到前台后约 160 ms 内接上。
      - 复现过程中的一次失误：面板处于触摸模式时，adb 发的 A 键先把焦点给了第一个可选项「SBS」，并把草稿改成了 SBS。随后按 B 取消、丢弃了草稿，保存的设置（`windowsVrEnabled` true、`xrPresentationMode` openxr）没有变。
  - 上面这次是 Vulkan 渲染（容器 `execArgs` 为 `-condebug -vulkan`）。10:21 按用户要求改回 D3D11：`execArgs` 改为 `-condebug`，原文件备份在证据目录 `alyx-sdk165/STEAM_546560.container.before-dx`。不带 `-vulkan` 时 `WindowsVrPayloadManager` 会删掉 `opencomposite.ini`。root 用 `sed -i` 改完后文件标签丢了 MLS 分类（变成 `s0`），已 `chcon` 回目录的 `s0:c155,c256,c512,c768`。
  - 没测：长时间游玩、Hades II 与麻将在 `linux-steam2` 下的回归（两者用旧接口版本，lsteamclient 只是新增接口）、Custom 模式（不加载 lsteamclient，但 Proton 基线从 xrg3 系换成了 xrg7）。

### Steam XR 模式（2026-10-20）

用户要求：进 Steam 前在 Swan 上选择 2D 或 XR，Steam 客户端本身也能跑在 XR 大屏里；进 Steam 后不再给每个游戏保留 2D/SBS/XR 选项，普通游戏跟随入口；Alyx 这类 VR 专有游戏要能正确启动；SBS 暂时不需要；注意功耗。方案和实施见[规格 §13](../specs/xrgame-native-big-screen-rendering-v1.md)。

- VR 专有游戏的识别：用官方客户端 `appcache/appinfo.vdf`（v29）验证过：
  - Alyx 546560、The Lab 450390 有 `common/onlyvrsupport = 1` 和 `openvrsupport = 1`，VR 启动项类型为 `vr`；
  - Hades II 1145350、麻将 3444020 没有这些键。
- 包与上机过程：
  - kb8 `7db8ae88…`：单元测试 13 项通过。首页右上角显示「Steam 显示方式：2D 窗口 / XR 大屏」，用方向键选 XR 后偏好写成 `display=xr`。进入后 `SteamXrActivity` 在前台，但没有建立 XR 会话。
    - 原因：`onResume` 里生命周期还是 STARTED，代码却要求 RESUMED；改为 STARTED。
  - kb9 `c2b60704…`：会话建立：72 Hz、1280×720 大屏、Vulkan 合成，客户端 VulkanRenderer 的直通桥接上。但 SDK `xr` 截图 99.9% 黑。
    - 原因：沿用了 2D 页面的黑色页面背景，被当作叠加层盖在画面上；改为透明。
  - kb10 `f1e6b1a6…`：装包后直接用 `am start` 拉起 `SteamXrActivity`，主线程卡死，DebugBus 超时。
    - 原因（堆栈留档 `steam-xr/stack-kb10-egl-linker-deadlock.txt`）：
      - XR 运行时线程在建会话时 `dlopen` Pico 的 `libpxrcompositor.so`，持有链接器锁执行构造函数，构造函数调用 `eglGetProcAddress`，等 EGL 初始化锁；
      - 主线程第一次初始化 EGL（X server 的 `GPUImage.createImageKHR` → `eglGetDisplay`），持有 EGL 锁，又在 `dlsym` 里等链接器锁。
    - VR 页面不画 HWUI 帧，新进程直接进 XR 页面时没人提前初始化 EGL。修法：`ImmersiveXrActivity.onCreate` 先在主线程初始化 EGL（游戏页面也受益）。期间强停过一次应用。
  - kb11 `0ab18cd5…`：同样的冷启动不再卡死。
    - SDK 截图：`steam-xr/1225-kb11-xr-left.png`（头部视角合成）、`steam-xr/1226-kb11-canvas.png`（大屏 1280×720，Big Picture 首页和射线光标）。
    - 叠加层中心像素透明。但它仍以约 28 Hz 持续重画：每次画叠加层都会翻转渲染视图的透明度，这一翻转又被记成页面变化。改为忽略渲染视图自己的重绘。
  - kb12 `e4f10729…`：
    - 12:27:42 会话建立，直通桥接上；
    - 加载页动画期间，叠加层约每 1.8 s 重画 30 次（约 16 Hz）；
    - 12:27:49 加载页消失后，到 12:28:07 没有新的「overlay layer refreshed」记录，即 18 s 内不到 30 次；
    - 光标静止、页面不变时不再重画。
  - 用户在 kb12 上从 Steam XR 启动 Hades II（12:43，`Official launch of STEAM_1145350: xr (vr=false, entry=xr)`），游戏一直不出帧，OpenXR 一直转圈。
    - 原因：2D 启动面板弹出时 `SteamXrActivity` 没有暂停，Steam 的会话还在（12:43:02 降为 VISIBLE）。
    - 12:43:09.590 游戏页面 `onResume` 建会话，报 `xrInitializeLoaderKHR failed: -6`，会话线程放弃；`SteamXrActivity` 的 `onStop` 12:43:09.782 才到。
    - 修复（kb13 `4cf90413…`，审计 0 errors，未装机）：游戏页面建会话前，如果有托管客户端的会话，先在主线程结束它。
  - 用户随后决定（不共用会话）：
    - 游戏用自己的会话和新图层；
    - 游戏期间 Steam 的图层隐藏、渲染停下；
    - 分辨率 Steam 和游戏各自设置，大屏尺寸共用。
    - kb14 实现见[规格 §13.5](../specs/xrgame-native-big-screen-rendering-v1.md)。代码路径核对：Steam 页面停止后，原来的 `XServerView.onPause` 是空操作，`stopXrSession` 又把平面呈现打开了，所以游戏期间 Steam 的每个 Present 仍会拷进渲染器。kb14 在 `onStop` 关掉平面呈现，回来时补画最后一帧。
    - kb14 `be773606…`：单元测试 16 项通过，审计 0 errors。用户同意后，13:12:48 装机（boot `06ddeeff`；装机前先用快捷菜单正常退出了 kb12 那局 Hades II）。
      - 13:13:01.9 Steam XR 会话建立：72 Hz、1280×720，直通桥接上；Big Picture 首页截图 `steam-xr/1318-kb14-steam-canvas.png`。
      - `linux_steam launch 1145350` 转交成功（`launchForward` 0）。启动面板在 13:16:42 被按下开始（有人在头显里按的，不是我们注入的）。
      - 13:16:51.610 游戏页面打出 `ending the hosted session before the game's`；游戏会话 13:16:51.921 进入 FOCUSED，没有 `xrInitializeLoaderKHR` 错误。
      - Hades II 出帧，截图 `steam-xr/1318-kb14-hades-canvas.png`（大屏层 2250×1264）。
      - 同一时刻 Steam 页面 `onStop` 关掉了它的平面呈现。游戏期间 5 s 线程采样里，占 CPU 的都是游戏会话的线程，Steam 那一路的线程都是 0%。
      - 13:17:59 有人按了右手柄 Home 键，13:18:05.71 Pico 系统界面强制停止了 SteamPSP（`Force stopping … from com.picoxr.systemui`）。我们随后的 `quickmenu exit` 发到时进程已经没了，所以"退出游戏回到 Steam XR"这一步这轮**没有测到**。
      - 之后从启动器重开，首页把 Steam 分辨率选成 1920×1080（偏好里 `resolution=1920x1080`），从首页进入 Steam XR，客户端按 1920×1080 启动。日志 13:18:36.044 `hosted screen now 1920x1080, starting the session again`，新会话 13:18:36.243 进入 FOCUSED，画面环为 1920×1080。
  - 用户随后要求：大屏下游戏的启动面板直接画在大屏上（ImGui 图层，不切回 2D Activity），XR 大屏支持输入法。设计见[规格 §13.6](../specs/xrgame-native-big-screen-rendering-v1.md)。
    - Foundation 里没有拼音引擎，用户选择移植 AOSP PinyinIME。
    - WSL x86_64 上的 64 位验证：用自带词库，`nihao` 出 你好、`zhongguo` 出 中国、`woaibeijingtiananmen` 出 我爱北京天安门。
    - kb15 `25e67ad0…` 没打进词库，作废。kb16 `5dc7fd74…` 包含 `assets/pinyin/dict_pinyin.dat`（审计 verified），单元测试 16 项通过，审计 0 errors。
    - kb16 装机（用户同意，14:00:34，boot `06ddeeff`）：
      - 从首页进 Steam XR，客户端按偏好的 1920×1080 起；`linux_steam launch 1145350` 后 14:01:27.558 `launch panel shown`。前台一直是 `SteamXrActivity`，没有切到 2D 页面。
      - SDK `xr` 截图 `steam-xr/1401-kb16-launch-panel-xr-left.png`：面板在大屏位置、和大屏一样大，但中文全是"?"。日志 `ImGui could not load the CJK font`：Swan 的 `NotoSansCJK-Regular.ttc` 是 CFF2 可变字体（5 个面，表里有 `CFF2`/`fvar`），ImGui 用的 stb_truetype 不支持 CFF2。改为依次尝试 Pico 的 `PICOSansSC-Regular.ttf`（TrueType glyf，7.7 MB）等字体，先检查表、跳过 CFF2（kb17）。
      - `input btn a` 在面板上按开始：14:02:51 保存选项，`preLaunchApp` 跑完（游戏修正、`restoreSteamApi`、ACF），14:02:59.998 游戏的沉浸式页面起来，`ending the hosted session before the game's`。全程没有 2D 页面。
      - 14:03:00.191 主线程崩溃：`ControllerManager.scanForDevices` 中 `InputManager` 为空。这个单例和 `ContainerUtils.setContainerDefaults`（进程级的 XRGame 默认运行时版本）原来都由 MainActivity 初始化；新流程里进程从没跑过 MainActivity。系统随后以 force-crash 结束了两个页面并杀掉进程。kb17 在 `ImmersiveXrActivity.onCreate` 补上这两项（`installLaunchReadiness` 在 picoXr 是空函数，不需要）。
    - kb17 `2d3910c0…`（单元测试 16 项通过，审计 0 errors）装机：
      - 14:08:36.8 面板出现，字体为 `PICOSansSC-Regular.ttf`，中文正常（`steam-xr/1408-kb17-launch-panel-xr-left.png`）。
      - 有两个显示问题：这个字体没有 Ⓐ Ⓑ，显示成"?"；面板背景 4% 透明，底下的 Steam 页面透出来。
      - `input btn a` 开始：14:09:04.9 保存选项，14:09:13.5 游戏页面起来并结束 Steam 的会话，14:09:13.776 进入 FOCUSED，没有崩溃。Steam 页面关掉平面呈现。Hades II 主菜单出帧（`steam-xr/1409-kb17-hades-canvas.png`）。
      - `quickmenu exit`（14:11:33.4）：游戏页面在 14:11:33.78 结束。0.1 s 后 Steam XR 重新建会话（1920×1080 画面环），平面呈现重新打开；Steam 页面立即有画面，不是黑屏（`steam-xr/1411-kb17-steam-after-exit.png`，Hades II 详情页，"STEAM 云: 已是最新"）。**游戏退出回到 Steam XR 这一步通过。**
      - 拼音：两个摇杆同时按下（`input btn l3_r3`）打开键盘，键盘默认拼音模式（设备语言是中文），14:12:59.926 `pinyin engine open`。用左摇杆导航加 A 打 n-i-h-a-o 后，状态为组字中；但按 Y（空格）没有上屏。
        - 原因：AOSP 这版引擎的 `im_add_letter` 是空函数（直接 `return 0`），逐个加字母一直是 0 个候选。在 WSL 上用相同调用顺序复现。改为每加一个字母对整段拼音调用 `im_search`（引擎按公共前缀增量搜索），WSL 上 `nihao` 出 62 个候选，`im_choose(0)` 后整句固定（fixed 2 = 拼音节数 2）（kb18）。
    - kb18 `8eadb026…`（单元测试 16 项通过，审计 0 errors；面板背景改为不透明，按键提示改用字母 A/B，键盘翻页键改为 `<` `>`）装机：
      - 拼音：14:18:51 打开键盘，打 n-i-h-a-o 后按 Y（空格），键盘提交一段文字（`typed` 1，组字结束）。14:19:10.817 Steam 输入法会话 `pasting 2 characters`，即"你好"经客户端剪贴板粘贴进 Steam。当时 Steam 里没有聚焦的输入框，所以只验证了送达路径，没有验证文字落进哪个输入框。
      - 面板：不透明，底部提示为"射线指向选项按扳机选择 · A 开始 · B 取消 · 右摇杆滚动"（`steam-xr/1419-kb18-launch-panel-xr-left.png`）。
      - 取消：14:19:44.665 按 B，`XR launch panel: STEAM_1145350 cancelled`，面板隐藏，没有启动游戏，Steam 的 `SteamLaunch` 进程退出，前台仍是 Steam XR。
  - 用户随后要求：XR 键盘的中文送进 Wine 游戏；面板和键盘参考 shadPS4 加图标、调样式；分辨率等长列表用下拉框。设计见[规格 §13.6](../specs/xrgame-native-big-screen-rendering-v1.md)。每版审计 0 errors，装机前都先用快捷菜单正常退出上一局 Hades II。
    - kb19 `c664a19d…`：游戏中文（`typeUnicode`，120 号键位）和测试入口 `guest_exec`。原生命令表漏了登记，`guest_exec` 报 unknown command。
    - kb20 `9782e05b…`：shadPS4 样式，合并系统符号字体后图标正常。面板 `steam-xr/1448-kb20-launch-panel-xr-left.png`；Hades II 里打开 XR 键盘打 nihao，候选栏为"你好 你 拟 尼…"（`steam-xr/1452-kb20-keyboard-pinyin-xr-left.png`）。`guest_exec` 这版发 WinHandler EXEC，没有反应：官方启动链里没有 `winhandler.exe`。
    - kb21 `8a577e40…`：超过 3 个选项的行改为下拉框（`steam-xr/1459-kb21-launch-panel-xr-left.png`）。`guest_exec` 改用启动器组件，报 `no_game_running`：`XEnvironment.getComponent` 按确切类匹配，启动器是子类。
    - kb22 `8be18f28…`：`guest_exec notepad` 返回 started，但没有 notepad 进程。用 root 手工复现（游戏 `Hades2.exe` 的 `/proc/<pid>/environ`，经打过补丁的 run-as `--app-domain` 在 App 域里跑 wine）找到三个原因：
      - 带着游戏进程自己的 `WINESERVERSOCKET`：`Bad server socket 23`；
      - 带着 `WINELOADERNOEXEC`：wine 不经 preloader 直接退出 1，没有输出（`WINEDEBUG=-all` 又排在前面，加的调试通道不生效）；
      - 新进程落在默认桌面，要另起 explorer 而失败：`nodrv_CreateWindow … The explorer process failed to start`。
      - 去掉前两个变量、改为 `wine explorer /desktop=shell,1280x720 notepad` 后，notepad 出现在游戏桌面上（15:15，`steam-xr/1515-kb22-notepad.png`）。
    - kb22 上的中文测试（15:17–15:18）：XR 键盘打 nihao，首选"你好"（`steam-xr/1517-kb22-game-keyboard-pinyin.png`）；按 Y 后 notepad 的光标从 Col 1 到 Col 3，即收到两个字符，但看不见（Wine 自带字体没有汉字，`steam-xr/1518-kb22-notepad-nihao.png`）。
      - 用 `WINEDEBUG=+key` 重开 notepad 再打一次：两次按键都是 `keysym=100597d (U597D)`，`ToUnicodeEx returning 1 with L"\597d"`，`WM_CHAR` 两次都是"好"，"你"丢了。
      - 原因：120 号先映射成"你"、按下，再映射成"好"、按下；Wine 在 notepad 的 `TranslateMessage` 时才按当时的映射查键位，那时已经是"好"了。
      - 修复：120–131 号键位轮流用，已映射到同一个字的键位直接复用（`Keyboard.unicodeKeycode`）。`guest_exec` 改为借用游戏 `explorer.exe` 的环境和桌面，前置 `名=值` 覆盖环境，输出写 `files/guest-exec.log`，见规格。
      - kb23 `c23d8fc3…`、kb24 `30349ffe…`（审计都是 0 errors）作废，没有装机：新单测 `KeyboardUnicodeKeycodeTest` 发现上限常量声明成 `byte`，131 溢出为 −125，备用键列表是空的，打中文会抛 `IndexOutOfBoundsException`；`setKeysyms`/`hasKeysym` 也按有符号数算下标，128 号以上会越界。kb25 改为 `short` 常量、下标按无符号取，单测 4 项（含原有 `KeyboardPauseTest`）通过。
    - kb25 `7b5ee3ed…`：审计 0 errors，构建内单测 20 项通过。装机前没有游戏在跑（kb22 那局已在 15:28 因下文的 OOM 结束）。
      - 从 Steam XR 启动 Hades II，大屏面板按 A 开始。游戏出帧后 `guest_exec WINEDEBUG=key notepad`：返回 `desktop shell,1280x720`，notepad 出现在游戏桌面上（`steam-xr/1538-kb25-guest-exec-notepad.png`），跟踪写进 `files/guest-exec.log`。
      - XR 键盘打 nihao、按 Y 上屏：跟踪里 `KeyPress : keysym=1004f60 (U4F60)`、`keysym=100597d (U597D)`，`TranslateMessage` 依次投递 `WM_CHAR` `L"\4f60"`（扫描码 0x60）和 `L"\597d"`（0x61），即"你好"两个字都对了（`steam-xr/1540-kb25-notepad-key-trace.log`；notepad 截图 `1540-kb25-notepad-nihao.png` 看不见汉字，原因同上）。**XR 键盘的中文送进 Wine 游戏这一步通过。**
  - Steam XR 下游戏运行时应用 OOM 崩溃（kb22 上发生，kb25 上复现并定位，kb26 修复待测）：
    - kb22 那局 Hades II 在 15:28:02 结束：`FATAL EXCEPTION … java.lang.OutOfMemoryError: Failed to allocate a 8310747 byte allocation`，线程是 Steam 客户端那个 X server（`linux-steam/root/tmp/.X11-unix/X0`）的请求线程，栈在 `DRI3Extension.pixmapForHardwareBuffer` → `Drawable.<init>`；Java 堆 509/512 MB。系统随后以 force-crash 结束页面、杀掉进程（`oom-kb25/kb22-oom-crash-logcat.txt`）。这次没有人操作头显，手柄读数一直是 0。
    - kb25 每 30 s 记一次 `dumpsys meminfo`（`oom-kb25/heap-watch-kb25.txt`）：只有 Steam XR 时 Dalvik 已分配约 82 MB 且平稳；游戏开始后约 108 MB，约 3 分钟后开始每分钟涨约 16 MB（关掉 notepad 后照样涨），15:49 到 343 MB。
    - 15:46 的 hprof（388 MB，分析后已删除，SHA 见 `oom-kb25/hprof-sha256.txt`；解析脚本 `oom-kb25/hprof_big.py`）：≥1 MB 的原始数组 43 个、共 319 MB，全是 `Drawable.data` 的直接缓冲。从 GC 根出发的最短路径（`oom-kb25/root-paths.txt`）：28 个（21 个 1922×1081、4 个 1920×1080、3 个 1280×800）挂在 `GraphicsContextManager.graphicsContexts` → `GraphicsContext.drawable` 上。按客户端分（`oom-kb25/graphics-contexts.txt`）：Steam X server 上客户端 `0x00c00000` 的 19 个 GC 引用着 1922×1081、已不是现存窗口内容的 drawable。
    - 原因：
      - 我们的 X server 里 `GraphicsContext` 强引用它创建时所在的 drawable（窗口内容或 pixmap），而这个字段没有任何地方读。GC 不释放，窗口改尺寸或销毁后的旧内容（每块约 8 MB）就一直留在 Java 堆里。
      - Mesa X11 WSI（`wsi_common_x11.c` 建交换链时 `xcb_create_gc`）销毁交换链时从不 `xcb_free_gc`，每重建一次交换链就漏一个 GC。Steam 的 GPU 进程（Zink）经这个 WSI 呈现；推断是 Steam 的窗口反复重建交换链，每次都漏一个 GC。游戏退出、回到 Steam XR 后堆照样涨（15:51 为 369 MB），所以不只发生在游戏期间；开头那 3 分钟没涨，什么时候开始重建没有量。
    - 修复（kb26，在 X server 侧，不改 Mesa/Turnip）：`GraphicsContext` 不再保存 drawable（X 协议里 GC 也只记 root 和深度）；`CreateGC` 仍校验 drawable 存在。
    - kb26 `6f83dabc…`（审计 0 errors，构建内单测 20 项通过；同一次开机 `06ddeeff`，PID 17996）同样从 Steam XR 启动 Hades II，每 30 s 记堆（`oom-kb25/heap-watch-kb25-kb26.txt`）：
      - 游戏 15:53–16:08 共 15 分钟：Java 堆 70 MB，15:58 一次 +16 MB，16:02 一次 +8 MB 后又回落，其余时间平稳，结束时 87 MB。kb25 同样时长内持续每分钟 +16 MB，到了 343 MB。
      - 期间前 10 分钟不碰键盘、不起 notepad；16:03–16:04 在游戏里用 XR 键盘打了 3 次"nihao"上屏，之后 4 分钟堆仍为 86 MB。
      - 退出游戏回到 Steam XR 后 4 分钟：56 MB，平稳（kb25 这一段仍在涨）。
      - 更长时间、2D 模式的 Steam 没有测。
  - 用户要求键盘放低、倾斜，往下看刚好看到（规格 §13.6「键盘位置」）。kb27 `f91892a7…`（审计 0 errors，单测 20 项通过）装机后，Steam XR 里打开键盘：日志 `XR keyboard placed 34 degrees below the eyes`（按这块大屏下沿算出来的，比默认 32° 低）。随后用户戴上头显试用，并反馈：
    - recenter 后键盘留在原处：键盘的锚点在 LOCAL 空间里，recenter 时只有状态层重新放置。改为两者都重新放。
    - OpenXR 手柄拖不动 Steam 大屏：日志里 16:58:47–16:58:57 有 5 次扳机按下，都是 `not in a grab zone`，位置在画面中间或离右下角 0.095 m（角的判定半径 0.06 m），而且控制点要先在菜单里打开才生效。改为侧握键在屏上任意位置抓起移动。
    - 两处都在 kb28。
  - kb28 `7dd0574c…`（审计 0 errors，单测 20 项通过）在用户摘下头显后装机（17:41，同一次开机 `06ddeeff`）：
    - Steam XR 里打开键盘：`XR keyboard placed 32 degrees below the eyes`。SDK `xr` 截图 `steam-xr/1742-kb28-keyboard-tilted-xr-left.png`：键盘在视野下方，近处的下沿宽、远处的上沿窄，是向后仰的。没人戴时头显朝向和大屏不一致，所以大屏在画面右侧。
    - 关键盘后用 DebugBus 按住右侧握键 1.5 s：`pointer grab started hand=1 resize=false grip=true`。松开后按扳机，`pointer down`/`pointer up` 照常送到页面，说明抓取已结束。
    - 没测，等用户在头显里试：手移动时大屏是否跟着走、位置是否保存；recenter 后键盘是否重新放置。Pico 在 recenter 时是否发 LOCAL 的 `REFERENCE_SPACE_CHANGE_PENDING` 还没在设备上见过（状态层依赖同一个事件，也没验证过）。
  - 用户确认侧握拖屏好用，又要求 OpenXR 手柄能拖动 Steam 当前页的内容（像 2D Steam 页那样）。原因和修法见规格 §13.6「用扳机拖动 Steam 页面内容」。kb29 `cb249486…`（审计 0 errors，单测 20 项通过）17:54 装机，Steam XR 正常起来（1920×1080）。拖动滚动要人拿着手柄才能测，等用户在头显里试。
  - 用户要求 XR 大屏也有键盘显示/隐藏图标（规格 §13.6「Steam 大屏上的键盘图标」），kb30。
    - kb30 `aa36e453…`（审计 0 errors，单测 20 项通过）18:01 装机。大屏截图 `steam-xr/1805-kb30-scroll-before.png` 右边缘 30% 高度处有键盘图标。用户随后试用说"好很多"。
    - 用户在 kb29 上反馈拖动不跟手、键盘开着时主面板的射线不能操作（后者 kb30 已改）。kb31 `8ce9789e…`（审计 0 errors，单测 20 项、shim 测试 11 项通过）加了 DebugBus `vr_pen`（模拟射线拖动）和即时滚动开关（`webhelper-instant-scroll`，shim 把 Steam 的 `--enable-smooth-scrolling` 换成 `--disable-smooth-scrolling`；默认关）。用右摇杆注入 3 格滚轮测每格滚动距离时，X 指针停在底栏上，页面没动（`1806-kb30-scroll-after3.png`），每格像素数还没量到。
    - 用户要求键盘不跟头转、跟主面板位置稳定（规格 §13.6「键盘位置」），kb32。
    - 用户说拖动"还是不太跟手"，让我测试优化（设备可用）。kb32 `ff967d43…`、kb33 `db274fc2…`（审计都是 0 errors，单测 20 项通过）。
      - kb32 的 `vr_pen` 在 DebugBus 里卡住：DebugBus 在主线程上应答并等命令做完（2 s），`vr_pen` 又等主线程处理每个笔事件，结果超时、笔划被拖到 2 s 以后才一起处理。这是测试工具的问题，真实射线不经过 DebugBus。kb33 改为在单独线程按 11 ms 一次发事件，命令立即返回，并记下落下/抬起的 uptime。
      - 测法：`capture_video` 录大屏（36 fps，时间戳与 uptime 同一时钟），`vr_pen` 匀速拖动，逐帧累加相邻帧的竖直位移（脚本 `drag/drag-video.sh`、`drag_timeline2.py`，结果在 `drag/` 下）。Steam XR 为 1920×1080。
      - 首页（kb33，每格 53 像素）：拖 400 像素，两个方向都停在 372 像素（7 格），中段页面速度是笔的 2.6 倍：首页按行吸附，不能用来量。
      - 商店，平滑滚动（Steam 默认 `--enable-smooth-scrolling`）：拖 400 像素，页面滚 1193 像素（比例 2.98），抬起后还滚了约 170 ms。
      - 商店，即时滚动（`webhelper-instant-scroll`=1，重启客户端）：每格固定 170.4 像素（170、341…1193，共 7 格）；从笔走到该发一格的位置，到画面动起来约 80 ms（含 Xephyr、Chromium 绘制、合成和录像）。
      - 库：一格 170、171 像素（两次）。
      - 结论：Steam 的页面每格滚轮约 170 像素（1080p），而拖动按 53（kb33）或 40（kb29–kb32）像素一格发，页面是手的 3.2 或 4.3 倍快，这是不跟手的主因。kb34：每格的拖动距离 = 屏幕高度 × 170/1080，四舍五入发格（误差在半格内），平滑滚动保持开（即时模式每次跳 170 像素）。其他分辨率下的每格距离没有量，按 Big Picture 随屏幕高度缩放推算。
      - kb34 `5f5b9cfa…`（审计 0 errors，单测 20 项通过）18:34 装机，平滑滚动恢复（webhelper 带 `--enable-smooth-scrolling`）。库里拖 400 像素、600 ms（`drag/v-library-kb34`）：页面滚 341 像素（2 格，比例 0.85，不到半格的零头不滚）；第 1 格在笔走过 85 像素时发出，页面约 120 ms 后开始平滑移动；笔抬起时页面正好到第 2 格，之后不再滑动。
      - 局限：Steam 每格就是 170 像素（屏幕高度的六分之一），这是滚轮模拟能做到的最细粒度；手在半格以内的小幅移动不会让页面动。要做到逐像素跟手，需要 Chromium 收到像素级的精确滚动，而 Xephyr 只把宿主的滚轮按键转给 Steam，做不到。
  - 用户要求首页选项改成两排、透视（VST）能在首页直接开关、Steam 分辨率像游戏的 2D 大屏那样按单眼尺寸和当前屏幕折算（规格 §13.5「首页两排选项」）。
    - kb35 `89fb5710…`（审计 0 errors，构建内单测 21 项通过，`SteamClientDisplayTest` 4 项覆盖折算、合并、上限、档位映射和旧尺寸映射）19:04 装机，用户同意。
    - 首页截图 `steam-xr/1905-kb35-home.png`（SDK `steampsp.panel`）：标题行是显示方式；第一排大屏尺寸 65/80/100/120″、弧形、透视；第二排 Steam 分辨率。80″ 时为 1280×720、推荐 1392×782、高 1824×1026、极高 2240×1260。原来存的 1920×1080 按最接近的宽度对应到「高 1824×1026」。
    - 没测：在首页点选（2D 面板在 Pico 的虚拟窗口里，`adb input tap` 的坐标对不上，没有乱点）；换尺寸后的分辨率只由单测覆盖；透视开关进 Steam XR 后是否生效，等用户在头显里试。
    - 用户看过 kb35 后决定：分辨率回到固定的 1280×720、1600×900、1920×1080；弧形和透视改成和单选区分开的样式；首页直接用英文（只改首页）。
    - kb36 `db8b76cd…`（审计 0 errors，单测 20 项通过）装机，用户同意。截图 `steam-xr/1915-kb36-home.png`：首页为英文，分辨率为固定三档，弧形和透视是开关。两个问题：
      - 两个开关接在第一排尺寸后面，英文下放不下：「Passthrough」折行，开关被右边缘截掉。
      - 选中的是 1280×720：偏好里还是 kb35 的档位名 `resolution=maximum`，固定三档不认识，按默认 1280×720 读。Steam 实际也按 1280×720 启动，显示和实际一致，所以不另做映射，用户重新点一次即可。
    - 用户要求排成两列，两个开关不必挤一行。kb37 `cd2272e8…`（审计 0 errors，单测 20 项通过）装机，用户同意。每排左边是单选胶囊，右边缘一个开关：第一排尺寸和 Curved，第二排分辨率和 Passthrough，两个开关上下对齐。截图 `steam-xr/1916-kb37-home.png`（SDK `steampsp.panel`，1920×1080）显示 Curved 关、Passthrough 开，与偏好一致；去掉了 kb36 的竖分隔线。
    - 用户要求 Steam 分辨率默认 1920×1080。kb38 `4cf9ef67…`（审计 0 errors，单测 20 项通过）：没存过或存的值不在三档里（包括 kb35 的档位名）时用 1920×1080，`SteamClientDisplayTest` 覆盖 `null`、`2560x1440` 和 `maximum`。装机，用户同意。截图 `steam-xr/1948-kb38-home.png` 选中 1920×1080。装机前偏好里已经是 `resolution=1920x1080`（kb37 期间在首页点选过），所以设备上走的是读已存值，没有走默认值那条路；默认值只由单测覆盖。
  - 用户反馈：进 XR 大屏后首页挡在前面（规格 §13.4「首页面板」）。kb38 时的任务：`SteamXrActivity` 在任务 #112（Pico 给 VR 类型的 Activity 单开任务），首页在 #110，标为 `visible=false`，但首页的 2D 面板仍在前面。游戏页面靠 `moveTaskToBack` 收起启动器（10-10），Steam XR 走托管分支，没有这一步。
    - kb39 `62d87f03…`（审计 0 errors，单测 20 项通过）装机，用户同意。用方向键在首页选中 Steam 卡片、按 Enter 进入，再用 DebugBus `input btn menu 800` 打开 Steam XR 菜单，用 `dumpsys activity top` 找到按钮位置，`vr_pen` 点击：
      - 进入：20:01:10.200 `launcher task 113 moved to the back`（`SteamXrActivity` 在任务 #115）。
      - 「返回首页」：20:03:13.277 `launcher task 113 brought back`，前台是原来的首页（同一个 ActivityRecord，任务 #113，没有多出一个首页），截图 `steam-xr/2003-kb39-home-after-home-item.png`。
      - 再进入：20:03:45.556 首页任务又移到后面（`SteamXrActivity` 在 #117）。
      - 「切到 2D」：`LinuxSteamActivity` 落在首页任务 #113、首页之上，是前台；20:04:17.826 首页任务拿回前面也没有盖住它。截图 `steam-xr/2004-kb39-switch-to-2d.png`（`steampsp.panel`，Big Picture 首页）。
      - 头显里首页面板是否真的不再挡住大屏，SDK 截不到 Pico 的 2D 面板（`xr` 只合成我们会话的图层），等用户在头显里确认。
    - 测试中发现：「切到 2D」把显示方式存成 `display=2d`，但首页回到前台时不重读，仍然显示「XR big screen」，这时点 Steam 卡片会按 XR 进入，和存的设置不一致。kb40 在首页 `onResume` 里重读显示方式。测试改掉了用户的显示方式（原为 XR），需要改回去。
    - kb40 `ab8f3f0a…`（审计 0 errors，单测 20 项通过）装机，用户同意。新进程的首页显示存着的「2D window」；用方向键选「XR big screen」（每步用 SDK 截图比对焦点所在的按钮），存回 `display=xr`。
    - 同一进程里重复 kb39 的路径：进 Steam XR（任务 #121），菜单「切到 2D」（`display=2d`，`LinuxSteamActivity` 在首页任务 #119 之上），再回到同一个首页（`--activity-clear-top`，同一 ActivityRecord）。首页显示「2D window」，截图 `steam-xr/2015-kb40-home-after-2d.png` 与新进程显示 2D 的截图 SHA 相同（`e90e5034…`）。之后再选回 XR，结束时为 `display=xr`、首页在前台，与测试前一致。
    - 用方向键时要先确认焦点：一次从 XR 选项往下按三格，焦点落在「Other」卡片上（亮青色边框），这时按 Enter 会切到 Custom 模式；改为按左一格、截图确认后再按 Enter。
    - 用户在头显里确认首页不再挡在大屏前面。
  - 用户要求状态层能在首页设置，默认可见（规格 §13.5「首页两排选项」）。kb41 `2866260d…`（审计 0 errors，单测 20 项通过）装机，用户同意。说明文字一行的右边缘加了「Status layer」开关，和 Curved、Passthrough 排成一列，读写的就是游戏启动面板里的同一个设置 `PrefManager.xrgameStatusLayer`。
    - 首页截图 `steam-xr/2019-kb41-home.png`：三个开关对齐，Status layer 开；DebugBus `vr_hud` 为 `statusLayer: true`。
    - 关：在首页用方向键选中开关按 Enter，`statusLayer: false`；进 Steam XR 后 `vr_hud` 为 `visible: false`，`xr` 截图 `steam-xr/2020-kb41-steam-xr-status-off.png` 大屏上方没有状态层。
    - 开：菜单「返回首页」，再按开关，`statusLayer: true`；进 Steam XR 后 `visible: true`，截图 `steam-xr/2022-kb41-steam-xr-status-on.png` 大屏上方有状态层。结束时状态层为开，Steam XR 在前台，与装机前一致。
    - 游戏启动面板里的「状态层（所有游戏）」没有去掉，两处是同一个设置；本轮没有从游戏里看。
  - 用户报：大屏上选中游戏后的启动面板（ImGui）里，射线指到几个同名的「开」「关」时提示 ID 冲突。原因：药丸按钮的 ID 只有序号加文字（`PushID(i)` + 标签），各行没有自己的 ID 作用域，几行「关 / 开」顺序相同，同名选项 ID 就相同；只有下拉框按行压了 ID。kb42 把按行的 `PushID(section * 1000 + index)` 提到整行外层，并在 `NewFrame` 后读 ImGui 的 `DebugDrawIdConflictsId`，发现新的冲突时记 `ImGui ID conflict on the pointed item`（标签 `xrimmersive_launch`）。ImGui 只检查射线指着的控件，所以要在头显里把选项指一遍才能确认。
    - kb42 `41e36769…`（审计 0 errors，单测 20 项通过）装机，用户同意（取消了正在等面板的 Hades II 启动）。从首页进 Steam XR，DebugBus `linux_steam launch 1145350`，20:31:04 面板出现；截图 `steam-xr/2031-kb42-launch-panel.png` 有 VST 透视、曲面屏、ETFR 调试视图等几行「关 / 开」。
    - 用户在头显里用射线指过这些选项，确认不再提示冲突；面板 20:35:29 关闭，这段时间 logcat 里 `ImGui ID conflict` 为 0 行。
  - 用户问大屏模式能否按左右眼或单眼录像，要的是模拟合成器的画面，不是游戏窗口本身。这就是 SDK 的 `xr` 源（按眼把会话提交的图层做光线求交合成，无镜头畸变），不用改代码。kb42 上 Hades II 2D 大屏（XR）同一局里各录 5 s（`eye-video/`）：
    - `xr` 左眼、右眼各 1296×1200（推荐眼尺寸的一半），两眼 2592×1200；分别 189、184、183 帧，约 36 fps（72 Hz），`skipped` 全为 0，合成耗时平均 4.5–5.0 ms。第 90 帧：大屏和状态层按各自的眼位置透视，左右眼有视差。
    - `canvas` 为大屏图本身（2252×1266，mono），选眼不起作用。
    - 和真实合成器的差别：没有镜头畸变，没有透视（VST）和 Pico 系统图层，按我们提交时的位姿合成（不含运行时的重投影）；录像最大为推荐眼尺寸的一半（`max_size` 只能再缩小），全尺寸只有截图。
    - 用户要求从应用启动开始用 `xr` 源录到喊停。录像随会话结束，新进程的截取源是 `canvas`，所以在电脑上跑守候脚本：前台是 `SteamXrActivity`/`ImmersiveXrActivity` 时设 `capture_source xr` 并 `capture_video start both`，只认 `stereo_sbs` 的录像，会话结束后等下一个会话接着录。kb42 冷启动后 20:50–20:56 录了 4 段（`eye-video/session-20261020-2050/`，每段原始 H.264 加逐帧 CSV）：Steam XR 62.4 s / 36.0 fps，Hades II 74.5 s / 36.0 fps，Steam XR 36.1 s / 35.9 fps，Alyx（VR）222.0 s / 32.6 fps（24 处帧间隔超过 60 ms，最长 139 ms）。每段比会话开始晚约 1–3 s（轮询加启动）；会话之间的 2D 页面不在 `xr` 源里。按各段平均帧率封装为 MP4 后拼成 `session-xr-both.mp4`（6:35，2592×1200）。
    - SDK 截图的局限：`xr` 截图按头显当前位姿合成。没人佩戴时头显朝向是固定的，面板和键盘只有一部分在视野里，所以截图只用来核对字体和样式，完整画面要戴上头显看。
    - 还没测，等用户在头显里试：
      - 用射线点面板上的选项（DebugBus 只能注入手柄，不能注入射线）；
      - 射线点候选、翻页、中/EN 切换；
      - 中文输入进 Steam 的实际输入框（比如商店搜索）；
      - 从 XR 入口启动 Alyx（VR 游戏的面板选项和启动）。
  - 还没测，等用户在头显里操作：射线点击、右摇杆滚动、A/B、XR 键盘、长按 Start 的菜单、从 XR 入口启动普通游戏和 Alyx 并返回、切到 2D 入口后的游戏方式。

同时发现、未修（需要用户决定）：

- （已由 `bc17c7733` 修复：Turnip xrg17 用 `IOCTL_KGSL_READ_CALIBRATED_TIMESTAMPS` 实现。）Turnip 的 KGSL 后端里 `kgsl_device_get_gpu_timestamp` 是 `UNREACHABLE`，但设备有 `has_persistent_counter` 时仍声明 `VK_KHR/EXT_calibrated_timestamps`。release 构建下调用它是未定义行为：Zink 每次取 GL 时间戳都报 `vkGetCalibratedTimestampsEXT failed (VK_ERROR_OUT_OF_HOST_MEMORY)`，客户端启动约 2 分钟就有 3459 行，拿到的时间戳也不可信。游戏用的 Turnip 是同一份代码。修法是用 `IOCTL_KGSL_READ_CALIBRATED_TIMESTAMPS` 实现它，或者在 KGSL 上不声明这两个扩展；这要改共享 Turnip，需要用户同意。
- ANGLE 每帧调用 `eglGetMscRateANGLE`。Mesa GLX 用 XF86VidMode 实现它，Xephyr 没有这个扩展，所以 webhelper 日志每帧多两行。
- ANGLE 启动时建 pbuffer 失败 4 次（GLX `CreateDRIDrawable` 打印 `failed to create drawable`），修复前后都有，界面正常。

## 已知问题（未修）

- ~~Swan 上 GPU 绘制的 GPU 进程会段错误~~：已修，见上文「GPU 绘制：Zink 段错误与黑屏」。
- ~~Swan 输入法行为不对~~：已修，见上文「输入法：组字实时上屏」。诊断日志保留（只记录长度）。
- 未测：真人长时间游玩、MHW 等更大的游戏（Wine 进程更多）、其他 app 也有子进程时的余量、Odin 3/Thor 上的回归。
