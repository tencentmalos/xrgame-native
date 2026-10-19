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

- 键盘按钮：默认放在右侧 30% 高度（原来在中间）。可以拖动，轻点仍是开关键盘，拖动后的位置按页面比例记住（`steam_page` 偏好）。
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

同时发现、未修（需要用户决定）：

- Turnip 的 KGSL 后端里 `kgsl_device_get_gpu_timestamp` 是 `UNREACHABLE`，但设备有 `has_persistent_counter` 时仍声明 `VK_KHR/EXT_calibrated_timestamps`。release 构建下调用它是未定义行为：Zink 每次取 GL 时间戳都报 `vkGetCalibratedTimestampsEXT failed (VK_ERROR_OUT_OF_HOST_MEMORY)`，客户端启动约 2 分钟就有 3459 行，拿到的时间戳也不可信。游戏用的 Turnip 是同一份代码。修法是用 `IOCTL_KGSL_READ_CALIBRATED_TIMESTAMPS` 实现它，或者在 KGSL 上不声明这两个扩展；这要改共享 Turnip，需要用户同意。
- ANGLE 每帧调用 `eglGetMscRateANGLE`。Mesa GLX 用 XF86VidMode 实现它，Xephyr 没有这个扩展，所以 webhelper 日志每帧多两行。
- ANGLE 启动时建 pbuffer 失败 4 次（GLX `CreateDRIDrawable` 打印 `failed to create drawable`），修复前后都有，界面正常。

## 已知问题（未修）

- ~~Swan 上 GPU 绘制的 GPU 进程会段错误~~：已修，见上文「GPU 绘制：Zink 段错误与黑屏」。
- ~~Swan 输入法行为不对~~：已修，见上文「输入法：组字实时上屏」。诊断日志保留（只记录长度）。
- 未测：真人长时间游玩、MHW 等更大的游戏（Wine 进程更多）、其他 app 也有子进程时的余量、Odin 3/Thor 上的回归。
