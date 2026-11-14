# 快速上手

在小米平板 8 Pro 上第一次安装 Linux，按本页五步走完即可，Android 和里面的数据保持原样。

## 开始前

* 设备是小米平板 8 Pro（`piano`）。16 GB 和 24 GB 使用同一镜像，固件在启动时按原厂引导程序提供的布局识别实际内存。
* Bootloader 已解锁，已安装 KernelSU 或 SukiSU。
* 平板上留出足够的空间，并备份重要数据。独立分区会从 Android 的数据分区里划出空间。

## 选一个系统

四个系统共用同一个启动镜像（ESP，即 EFI 系统分区），只有系统镜像（root，即根文件系统）不同。

| 系统镜像 | 系统 | root 分区至少 | 适合 |
| --- | --- | --- | --- |
| `debian-gnome` | Debian 13 + GNOME | 8 GiB | 日常使用的通用桌面 |
| `debian-kde` | Debian 13 + KDE Plasma | 8 GiB | 习惯 KDE Plasma 的用户，限制见[已知问题](known-issues.md#其他) |
| `nyarch-gnome` | Arch Linux ARM + GNOME（Nyarch） | 16 GiB | 想用滚动更新的软件包和 Nyarch 主题 |
| `steamos` | Valve SteamOS ARM64 | 16 GiB | 玩游戏，详见 [SteamOS](steamos.md) |

## 安装

从 [Build products](https://github.com/slwyts/Project-SunUEFI/actions/workflows/build-products.yml) 同一次构建里下载三个包：`SunUEFI-Module`（引导助手模块）、`SunUEFI-ESP`（启动镜像）和 `SunUEFI-Root-发行版`（系统镜像）。把解压出来的模块 ZIP 和两个 `.sparse.img` 文件放到平板上，然后：

1. 在 KernelSU 或 SukiSU 的模块页面安装模块 ZIP，提示出现时按音量＋安装引导，重启。
2. 在模块卡片上点 **打开 WebUI**，进入引导助手。
3. 打开 **存储** 页，点 **创建独立分区**，选好分给新系统的容量，确认后重启一次。平板上已有 `sunuefi_esp` 和 `sunuefi_root` 时跳过这一步。
4. 重新打开助手，在 **存储** 页的 **安装或更新系统** 里选择启动镜像和系统镜像，点 **安装所选镜像**，等它完成。
5. 打开 **启动** 页，选 **UEFI**，再选 **Linux**，点 **保存并重启**。

每一步的细节和提示见[安装指南](install-from-artifact.md)。第 3 步在新平板上还没有验证过，见[已知问题](known-issues.md#首次分区)。

## 首次启动

首次启动会把根文件系统扩展到分区的全部容量，比以后的启动慢。看到桌面后用用户名 `piano`、密码 `piano123456` 登录，桌面默认自动登录。锁屏解锁和 `sudo` 用同一个密码，可以在系统设置里改，或者运行 `passwd`。

## 充电记录

`piano-charge-monitor.service` 默认关闭。需要观察充电时，在终端运行：

```sh
sudo systemctl start piano-charge-monitor.service
```

接电时每 2 秒记录，未接电时每 60 秒记录；插拔电源会立即重新采样。
记录包含电源电压、电流、功率估算、快充协议和认证状态、两芯电池信息、
温度、热限流档位及 CPU/GPU 频率。它只读取状态，不修改充电设置，也不
阻止系统睡眠；系统睡眠期间不采样，唤醒后继续。

日志保存在 `/var/log/piano-charge-monitor/`，旧记录自动压缩，总量不超过
20 MiB，满后删除最早记录。导出记录或结束监测：

```sh
sudo tar -C /var/log -czf "$HOME/charging-logs.tar.gz" piano-charge-monitor
sudo systemctl stop piano-charge-monitor.service
```

需要以后开机也记录时，用 `sudo systemctl enable --now piano-charge-monitor.service`；
取消开机记录并停止则用 `sudo systemctl disable --now piano-charge-monitor.service`。

## 回到 Android

在 Linux 里运行 `piano-next-boot android --reboot`，或者点桌面上的“返回 Android”。开机时在选择页用音量键选 Android 只影响这一次。其他情况见[故障恢复](recovery.md)。

安装失败，或者想用电脑刷写，见[电脑安装](install-from-computer.md)。想先试运行再决定，见[临时启动](temporary-boot.md)。
