# SteamPSP

2026-10-07：应用显示名从 **XRGame Native** 改为 **SteamPSP**。
这里的 **PSP = PICO Space Pro**，表达在目标空间设备上运行 Steam 游戏的用途。
当前普通 Android 验证和实验性 SBS/VR 的范围仍以项目 spec 为准，名字不代表头显适配已验收。

![SteamPSP](steampsp-icon.svg)

## 图标

使用原创矢量图形：白色头显轮廓内融合十字键和操作按钮，外围是青色空间轨道，背景为深蓝渐变。
深蓝/白色建立 PC 游戏入口的视觉联想，头显、轨道和青色强调 PICO 与空间体验。
参考入口：[Steam 品牌页面](https://partner.steamgames.com/doc/marketing/branding) 与
[PICO 官网](https://www.picoxr.com/global)。没有复制或拼接官方 logo 素材。

- 深蓝背景：`#153D59` → `#071523`；面板：`#102C42`。
- 空间青：`#36D6ED`；前景白：`#F3FAFF`。
- Android 采用 108dp VectorDrawable 分层和 adaptive icon；主体在中央安全区内。
- 默认、圆形、备用及备用圆形入口统一；Android 13 themed icon 有独立的透明镂空单色资源。
- 图标不放小字，缩小后保留头显、方向键和空间环的轮廓。

生产资源位于 `app/src/picoXr/res/drawable/ic_launcher_xr_{background,foreground,monochrome}.xml`，
四个入口位于 `app/src/picoXr/res/mipmap-anydpi-v26/`。
[SVG](steampsp-icon.svg) 是依据生产彩色矢量导出的预览；[PNG](steampsp-icon.png) 供快速查看。
源图为仓库内可编辑矢量，不依赖生成式位图或远端图片。

## 更名范围

更新 picoXr 的 15 组默认/语言资源中的桌面名、登录页名，以及中英文首次启动说明。
AGENTS、CLAUDE 和 README 记录新产品名；上游项目说明和历史验收文档保留原名。

兼容标识继续使用：

| 项目 | 保留值 |
| --- | --- |
| applicationId | `com.tencentmalos.xrgamenative` |
| 仓库 | `tencentmalos/xrgame-native` |
| 游戏安装根目录 | `/sdcard/XRGameNative` |
| 源码包名、DebugBus、运行库与 catalog 标识 | 现有值 |
| 签名、Steam 账号、存档和容器目录 | 现有值 |

这些标识保证现有安装可继续覆盖升级；本次没有数据迁移。
构建检查：`assemblePicoXrDebug` 成功；APK 源码审计 126 项、0 错误；
`aapt dump badging` 中全部 16 个 application-label 条目均为 `SteamPSP`，包名保持一致。
APK SHA-256：`e2afcce875d6aaf27c6df0bb7e815ee749e755bd9d1aada3ec889a569286a35f`。
本地日志、原生库 Build IDs 和 APK 资源检查结果位于 `build/branding/`。
本次未安装到设备，不把矢量预览或 APK 资源检查表述为实际设备桌面验收。
