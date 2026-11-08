# SteamOS

SteamOS 镜像在平板上提供 Steam 游戏模式和 KDE 桌面模式，默认进入游戏模式。游戏模式的显示、输入，以及游戏模式和 KDE 桌面之间的切换可用，Proton 与 FEX 支持良好。个别游戏自身的启动配置（如锁处理）与系统本身无关。

## 实机画面

| Steam 游戏模式 | 原神游戏内 |
| --- | --- |
| ![中文 Steam 游戏模式与游戏库](../assets/gallery/steamos-gaming-mode.webp) | ![原神角色进入游戏世界](../assets/gallery/steamos-genshin-world.webp) |
| **原神官方启动器** | **魔法少女的魔女审判** |
| ![原神官方启动器](../assets/gallery/steamos-genshin-launcher.webp) | ![魔法少女的魔女审判剧情画面](../assets/gallery/steamos-witch-trial.webp) |

## 安装

在 [Build products](https://github.com/slwyts/Project-SunUEFI/actions/workflows/build-products.yml) 里下载 `SunUEFI-Module`、`SunUEFI-ESP` 和 `SunUEFI-Root-steamos`。启动镜像和其他发行版共用，安装步骤和 Debian 一样，见[安装指南](install-from-artifact.md)。

系统镜像是 16 GiB，独立 root 分区至少也要这么大。登录用户名 `piano`，密码 `piano123456`。第一次启动 Steam 需要联网检查客户端更新并登录。

## 日常使用

* 输入法：默认带 IBus 拼音，按 `Ctrl+空格` 切换中英文。
* 触控板：游戏模式默认开启自然滚动，Steam 里没有对应的设置页。KDE 桌面模式可以在系统设置的触控板页调整，两种模式各自保存输入偏好。
* 显示：游戏模式分辨率是 3200×2136，菜单固定 120 Hz。KDE 桌面默认两倍缩放，可在显示设置中开启 HDR 和广色域。
* 电源：游戏模式下短按电源键挂起。KDE 桌面默认 2 分钟熄屏，用电池 5 分钟、接电 10 分钟后挂起。
* 自动亮度：Steam 里的自动亮度没有接通，需要手动调节背光。
* 重装系统：SteamOS 没有 Valve 的出厂恢复功能，需要重装时在引导助手里重新安装系统镜像。

## 系统更新

系统更新走 pacman 软件包，和 KDE Discover 共用软件源，不使用 Valve 的 A/B 镜像更新。Mesa、Gamescope、FFmpeg、传感器、平台管理，以及带修正的 GTK、IBus 和 KWin 包跟着系统镜像更新，不随 pacman 升级。其他库升级以后，这些本地包有可能需要重新编译，所以现在还不能当作完整的滚动更新系统。内核和设备适配的更新，同样通过引导助手刷写新的镜像。Steam 客户端自己的更新不受影响。

## 运行《原神》

原神国服 7.1.0 可以通过官方启动器进入游戏世界。由于其引擎本身的锁机制，如果卡在协议页，可在启动项中加入该游戏自身的参数（与系统本身无关）：

```text
STEAM_FEX_MULTIBLOCK=0 %command%
```

镜像预装了 ARM64 版的 GE-Proton11-11 和 Steam Linux Runtime 4.0，默认兼容工具已经配置好。Proton 里自带 FEX、DXVK 和 vkd3d-proton，首次启动游戏时不需要再下载运行库。Steam 登录、游戏内容和后续更新仍然需要网络。技术原因详见[Proton/FEX 协作挂起](../devel/linux/steamos-proton-fex-suspend.md)。

构建方式、补丁和调试记录见[SteamOS ARM64（开发文档）](../devel/linux/steamos.md)。
