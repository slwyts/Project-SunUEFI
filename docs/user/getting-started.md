# 快速上手

在小米平板 8 Pro 上第一次安装 Linux，按本页五步走完即可，Android 和里面的数据保持原样。

## 开始前

* 设备是小米平板 8 Pro（`piano`）。16 GB 和 24 GB 使用同一镜像，固件在启动时按原厂引导程序提供的布局识别实际内存。
* Bootloader 已解锁，已安装 KernelSU 或 SukiSU。
* 平板上留出足够的空间，并备份重要数据。独立分区会从 Android 的数据分区里划出空间。

## 选一个系统

各系统共用同一个启动镜像（ESP，即 EFI 系统分区），只有系统镜像（root，即根文件系统）不同。

| 系统镜像 | 系统 | root 分区至少 | 适合 |
| --- | --- | --- | --- |
| `debian-gnome` | Debian sid + GNOME | 8 GiB | 日常使用的通用桌面 |
| `debian-kde` | Debian sid + KDE Plasma | 8 GiB | 习惯 KDE Plasma 的用户，限制见[已知问题](known-issues.md#其他) |
| `debian-denial` | Debian sid + Denial | 8 GiB | 想用 APT 和 Denial 触控桌面 |
| `nyarch-gnome` | Arch Linux ARM + GNOME（Nyarch） | 16 GiB | 想用滚动更新的软件包和 Nyarch 主题 |
| `arch-denial` | Arch Linux ARM + Denial | 16 GiB | 想用 pacman 和 Denial 触控桌面 |
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

## 软件源与更新

已经配置 SunUEFI 软件源的镜像，可以直接用包管理器更新。Debian 运行 `sudo apt update && sudo apt upgrade`，Arch 系（Nyarch、Arch Denial 和 SteamOS）运行 `sudo pacman -Syu`。更新内核前可用 `sudo bootctl --print-esp-path` 确认启动分区已挂载；重启失败时可在 systemd-boot 菜单选旧内核。

### 手动添加 APT 软件源

该源面向 Debian **sid ARM64**，不要添加到 Debian stable。下载[归档签名公钥](https://sunuefi-repo.okassets.uk/keys/sunuefi-archive-keyring.gpg)，运行 `gpg --show-keys --fingerprint sunuefi-archive-keyring.gpg`，核对主密钥指纹为 `1B89 034B 2C88 8929 63C0 C1CA D7D5 F9A7 080E 6560`。

```sh
sudo install -m 0644 sunuefi-archive-keyring.gpg /usr/share/keyrings/sunuefi-archive-keyring.gpg
sudoedit /etc/apt/sources.list.d/sunuefi.sources
```

在 `sunuefi.sources` 里填入：

```text
Types: deb
URIs: https://sunuefi-repo.okassets.uk/apt
Suites: sid
Components: main
Architectures: arm64
Signed-By: /usr/share/keyrings/sunuefi-archive-keyring.gpg
```

### 手动添加 pacman 软件源

下载并核对同一份签名公钥后，将它加入 pacman 的信任库：

```sh
sudo pacman-key --add sunuefi-archive-keyring.gpg
sudo pacman-key --lsign-key 1B89034B2C88892963C0C1CAD7D5F9A7080E6560
sudoedit /etc/pacman.conf
```

Nyarch 只添加 `[sunuefi]`；SteamOS 添加下面两个仓库，保持顺序。把它们放在发行版原有仓库之前，保留原有仓库和签名设置。

```ini
[sunuefi-steamos]
SigLevel = Required TrustedOnly
Server = https://sunuefi-repo.okassets.uk/pacman/$repo/os/$arch

[sunuefi]
SigLevel = Required TrustedOnly
Server = https://sunuefi-repo.okassets.uk/pacman/$repo/os/$arch
```

保存后运行 `sudo pacman -Syu`。`sunuefi-steamos` 只给 SteamOS 使用，不要把 Valve 的包混装进 Nyarch。

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
