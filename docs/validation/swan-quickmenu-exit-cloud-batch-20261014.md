# Swan：DebugBus 快捷菜单退出落空、部分失败批次的变更号（2026-10-14）

两个问题都出在 10-13/14 的 Alyx（546560）无人值守测试里，前一个导致强制结束，后一个导致之后的假冲突。

## 1. `quickmenu exit` 的确认键落空

### 现象

三个 Alyx 会话里，`quickmenu exit` 打开菜单、把焦点放到「退出」后合成的确认键没有被任何菜单项处理：

| 时间 | 证据 | 日志 |
|---|---|---|
| 10-13 18:04（18:06 重试） | `run99-texquality/alyx/b-low-50/logcat.txt`、`logcat-exit.txt` | `keyCode=23 … dispatchKeyEvent=false`、`keyCode=96 … dispatchKeyEvent=false` |
| 10-13 晚 | `run100-d3dq/alyx-low-50-follow2`（只有记录，没有日志） | 退出时菜单再次卡住 |
| 10-14 09:22 | `run105-dxvk-rs/alyx50-dump/logcat.txt` | 同上 |

菜单一直停在暂停（SIGSTOP）的游戏上，只能 `am force-stop`，退出时的云同步因此没有执行。

成功的几次（如 `run105-dxvk-rs/alyx50-fix1`、`fex-fpcr/afp-off-skip`）在确认前的日志完全相同，区别只在确认键这一步：成功时有 QuickMenu 的 `onPreviewKeyEvent keyCode=23`，随后 `Exit called: quick_menu`；失败时按键没有进入 QuickMenu。

### 原因

- `debugExit` 合成 `DPAD_CENTER` / `BUTTON_A`，交给 `Activity.dispatchKeyEvent`。按键走 Android View 的焦点链，送到持有焦点的 View。
- 游戏进行时，持有焦点的是游戏的 `TouchpadView`。它是 AndroidComposeView 里的互操作子 View，见本次各轮进场景后的 `dumpsys activity top`：`TouchpadView{… VFED..C.. .F……}`。
- 菜单打开时，QuickMenu 用 `FocusRequester` 申请 Compose 焦点。只有 View 焦点也随之移到 AndroidComposeView 上，合成的按键才会到达菜单。失败的那几次，按键没有到达 QuickMenu 的 Box，说明 View 焦点不在菜单里。
- 这个 Activity 里合成按键本来就不可靠，`ImmersiveInputBypass` 的说明也写了这一点。XR 手柄按 A 因此走另一条路：有焦点的菜单行通过 `reportActivate` 上报自己的动作，A 直接调用这个动作。DebugBus 的退出没有走这条路。
- 失败时 View 焦点具体落在哪个 View 上，旧日志里没有记录。用当时的 APK（`b94439ac`）连跑 5 轮（run1–run5）都没有复现。

### 修改

- QuickMenu 通过新的 `ImmersiveSessionHooks.registerExit` 注册「退出」项自己的动作：`onItemSelected(EXIT_GAME)`，用 `rememberUpdatedState` 取最新回调。
- `debugExit` 在需要时打开菜单，0.5 s 后直接调用这个动作，不再经过焦点和合成按键。退出仍然走菜单原来的路径（恢复进程、退出云同步、OpenComposite 还原）。
- 合成按键没被处理时，日志里会注明当时持有焦点的 View（`focus=…`），供手柄回退路径以后排查。

## 2. 部分失败的上传批次没有记录变更号

### 现象

10-13 Alyx 每次启动都报假冲突：

1. 17:56 启动同步 `14 -> 14`，发现本地有改动，上传 9 个文件，批次号 15。
2. 18:09 再次启动是 `14 -> 15`，报「本地有改动、云端也有新文件」。所谓的新文件，正是自己 17:56 的上传。用户选「保留本地」，云端到 16，本地仍记 14。
3. 19:04 启动是 `14 -> 16`，再次冲突。用户再选「保留本地」，批次号 17，9 个文件全部返回 `DuplicateRequest`（`run100-d3dq/alyx-low-50-follow*`）。按当时的处理，这一批又算失败。

### 原因

原来的设想是「变更号只在退出同步时才写」，这个设想不对。批次成功时，变更号和文件缓存会在同一事务里立即写入（`uploadUserFiles`），强杀进程或重装 APK 都不会丢。

真正的问题是：17:56 这一批**部分失败**了。

- 出站白名单拦了 sg 存储主机，`autosave01.jpg` 上传失败，提交结果 `Fail`；其余 8 个文件提交成功。证据在 `run99-texquality/alyx/b-low-50/logcat.txt`。
- 批次以 `Fail` 结束时，Steam 仍保留已经提交的文件，云端变更号照样前进到 15。
- 我们这边批次一失败就什么都不记：本地仍是 14 和旧缓存，于是下次启动就把自己的上传当成了别的设备的改动。

10-14 的 `6f42e0d6c` 修掉了 `DuplicateRequest` 被当成失败的问题，也加了按 SHA 比对云端的判断。但部分失败的情形仍会先误报一次：本地和缓存都不等于云端，那两条判断都不命中。

另外还有一个风险：上传失败后，`reconcileNeverSyncedCloudFiles` 仍会执行。只要云端有本机从未同步过的文件，它就会把完整的本地扫描写成缓存，等于把没传上去的改动标成「已同步」。下次启动如果落后于云端，就会走「本地无改动」直接下载，覆盖这些改动。

### 修改（XRGame）

- **批次失败后核对云端**：重新读取完整的云端清单。
  - 上传：清单里 SHA 与本地一致的，计入缓存。
  - 删除：只有 `ClientDeleteFile` 返回 OK 的才计入。上传结果新增 `filesDeleted` 字段；SteamCloud 回退路径只在批次里声明删除，不计入。
  - 合并后的缓存与云端清单逐路径、逐 SHA 完全一致时，才连同清单的变更号一起写入。失败的文件仍是本地改动，下次启动会重新上传。
  - 云端只要有任何别的变化（例如其他设备新增的文件），就什么都不记，保持原来的处理。
- **上传失败后不再补下载从未同步的文件**：等号分支和「保留本地」分支都不再执行 reconcile。

### 单测（picoXr，`SteamAutoCloudTest`）

- `xrgame_equalCn_batchPartlyFails_recordsWhatTheCloudHolds`：
  1. 两个文件改动，其中一个提交被拒。
  2. 第一次同步结果为 `UpdateFail`，变更号记为 6。已提交的文件计入缓存，被拒的文件保留旧 SHA。
  3. 第二次同步只上传被拒的那个文件，结果 `Success`，记为 7，没有冲突。
- `xrgame_equalCn_batchFails_cloudChangedElsewhere_recordsNothing`：云端多出一个别处的文件。批次失败后不记录，变更号仍是 5，改动仍是本地改动，也没有发起下载。
- 原有的 `rejectedUploadCommitDoesNotAdvanceCacheOrReportUploadedFiles` 仍通过：SteamCloud 路径不执行删除，所以不计入。
- 整个类 54 个测试全部通过。
- 对照：只在 WSL 副本里关掉这两处修改，新增的两个测试都失败。

## 3. 设备验证

### 版本与设备

| 项 | 值 |
|---|---|
| 设备 | Swan（`B3110`），`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `6d4335a5-667c-426d-815b-1a82edaa0fbb` |
| 修改前 APK | `b94439acbd4cfd4e2133edecb7f78c9f00f86412b1aeed11288e50af8c7efbb7`（10-14 run122），app PID 13627 |
| 修改后 APK | `2215572cfa2d04346018fe6b6c4be40750aa30673ba386c5a497a5218cf61a1d`，app PID 2948 |
| 源码 | `malos/main` `8ba68b0a8` 加本次改动；Foundation `d859388`（gitlink） |
| 组件清单 | `bundle-rs6` `manifest.json` `8b3e4fc78fc05816c2e089f8c0c27c2b5175b4782cfe84fbef8c5b8c5e548470`；OpenComposite `f5a979e0` |
| `.so` Build ID | `libxrimmersive.so` `007f465d45c55e4619790ed3336469f709b7923b`，`libwinlator.so` `55fd2c184d32acd5c8114caddc48ebf15521db8b`，`libxrgame_debugbus.so` `82ee8f950b9c8ce1a6c505b798c1432969ef4d8f`；全部 31 个见证据目录 `apk-2215572c/so-build-ids.txt` |

证据在仓库外 `xrgame-native-evidence/swan-20261014-quickmenu/`。开始前已备份 Alyx 的 `hlvr/save`：9 个文件，带 SHA-256 清单，见 `save-backup-start/`。

### 快捷菜单退出

用无人值守流程跑：`launch 546560 load=s0/autosave` → 等进场景 → `quickmenu exit` → 等会话结束。每轮在退出前都用 `dumpsys activity top` 记录持有焦点的 View。

| 轮次 | APK | 时间 | 退出前持有焦点的 View | 结果 |
|---|---|---|---|---|
| run1–run5 | 修改前 | 15:33–15:44 | `TouchpadView`（run2–5；run1 的解析脚本有误，原始 dump 中同样是它） | 5 轮都正常退出，没有复现卡住 |
| fix-run1–3 | 修改后 | 15:49–15:55 | `TouchpadView` | `DebugBus runs the quick menu Exit item (registered=true)`，8–9 ms 后 `Exit called: quick_menu`，会话结束 |
| fix-run4 | 修改后 | 15:55–15:56 | `TouchpadView` | 启动 50 s 后、Alyx 还在主菜单准备读档时退出：同上，会话结束 |

修改前的失败没能按需复现，今天 5 轮都成功。证据目录里有日志的旧路径退出共 31 次，其中 3 次落空：10-13 18:04 及同一会话 18:06 的重试，以及 10-14 09:22。另有 10-13 晚上一次没有留下日志。修改后的路径不再经过 View 焦点，所以不受这个条件影响。真实手柄在菜单里的操作没有改动，本轮也没有人佩戴头显手动验证。

### 云同步回归

- 修改前 run1 退出时上传了本局的存档改动（`20 -> 20`，上传后为 21），之后每轮的启动和退出同步都是 `21 -> 21`，没有本地改动。
- 修改后 4 轮同样都是 `21 -> 21`，没有冲突，也没有出现 `Upload batch failed`。
- 部分失败批次的处理在设备上没有触发，需要上传途中出错才会走到；这部分由上面的单测覆盖。
