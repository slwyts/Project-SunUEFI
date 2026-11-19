# 安装指南

在平板上用 SunUEFI 引导助手安装 Linux：创建独立分区，写入启动镜像和系统镜像，再选择从 Linux 启动。不需要电脑，也不用填写序列号。第一次安装的简要路线见[快速上手](getting-started.md)。

## 准备

平板已解锁 Bootloader，并安装了 KernelSU 或 SukiSU，重要数据已备份。官方 Magisk 需要 MMRL 等支持 WebUI 的管理器。

从 [Build products](https://github.com/slwyts/Project-SunUEFI/actions/workflows/build-products.yml) 同一次完整构建里下载三个包：

| 下载包 | 解压后使用的文件 | 用途 |
| --- | --- | --- |
| `SunUEFI-Module` | `SunUEFI-Piano-版本.zip` | 引导助手模块 |
| `SunUEFI-ESP` | `esp.sparse.img` | 启动镜像（ESP） |
| `SunUEFI-Root-发行版` | `root-发行版.sparse.img` | 系统镜像（root） |

系统镜像可从[发行版列表](getting-started.md#选一个系统)中选择。ESP 和模块各只有一份，供所有系统使用。手动选择 `uefi` 或 `linux` 的开发构建不含完整系统。`SunUEFI-UEFI-Fastboot` 只用于[临时启动](temporary-boot.md)，`logs` 用于排查构建问题，安装时都不需要。

把模块 ZIP 和两个镜像文件保存在平板上。模块 ZIP 不要解压，镜像文件放在任意位置，之后在助手里用文件选择器选取。刷写稀疏镜像需要 0.5.2 或更新的助手。

## 1. 安装引导助手

在 KernelSU 或 SukiSU 的模块页面选择模块 ZIP。安装提示出现时：

* 音量＋：安装或更新引导，保留当前 Android 内核。
* 音量－：只安装助手，稍后在 **维护** 页点 **重新安装引导**。

已经装有相同版本的引导时会自动跳过。安装完成后正常重启，在模块卡片上点 **打开 WebUI**。

自定义 GKI 只要使用兼容的引导格式，也会原样保留，不需要先换回原厂内核。若提示当前 BOOT 的校验信息缺失或与内容不一致，确认它就是目前能正常启动 Android 的引导后，按提示在 30 秒内按下并松开音量－三次继续；音量＋或超时取消。无法识别的内核入口、空间不足或不能完整还原的镜像仍会停止安装，并显示原因。

> [!WARNING]
> 首次安装引导，或 HyperOS 更新恢复了原厂 BOOT 之后重新安装时，如果提示无法确认 Bootloader 已解锁，可能是状态伪装模块隐藏了真实状态。只有临时 Root、实际并未解锁的设备写入引导才可能变砖。确认设备确实已解锁后，在 30 秒内按下并松开实体音量－键三次继续。按音量＋键或超时都会退出，不写入任何内容。这个确认只对本次安装有效。

## 2. 创建独立分区

打开 **存储** 页，点 **创建独立分区**，选择分给新系统的容量并确认。完成后正常重启一次，再打开助手。平板上已有 `sunuefi_esp` 和 `sunuefi_root` 时跳过这一步。首次分区在新平板上的情况见[已知问题](known-issues.md#首次分区)。

## 3. 安装系统镜像

在 **存储** 页的 **安装或更新系统** 里，分别选择 `esp.sparse.img` 和 `root-发行版.sparse.img`，点 **安装所选镜像**，确认后等待完成，期间不要重启或断电。刷写会覆盖独立分区里原有的系统和文件，Android 数据不受影响。

## 4. 启动 Linux

在 **启动** 页选择 **UEFI**，再选择 **Linux**，点 **保存并重启**。以后普通开机沿用这个选择，也可以在开机选择页临时选 Android。登录信息和首次启动的说明见[快速上手](getting-started.md#首次启动)。

## 怎样确认安装成功

在 **维护** 页点 **检查引导**，显示“引导正常”说明模块和引导安装完好。重启后看到 Linux 桌面，就完成了安装。

## 后续更新

日常更新用系统自带的包管理器。带 SunUEFI 签名软件源的镜像会一同更新项目提供的内核、设备树和其他已打包的设备组件，不需要重新刷系统镜像。

* Debian：运行 `sudo apt update && sudo apt upgrade`。
* Nyarch 和 SteamOS：运行 `sudo pacman -Syu`。SteamOS 也可以在 [Steam 或 Discover 中更新](steamos.md#系统更新)。

新版 Arch 镜像自带 `piano-sensors`，随系统更新。早期镜像中的传感器服务已经能
运行，不必重装。如果手动安装 `piano-sensors` 时 pacman 报告文件已存在，用
`pacman -Qo <报错路径>` 检查归属。只把无主的旧服务或配置移到备份目录，
保留其他软件包拥有的文件、修改过的设置和 `/var/lib/piano-sensors` 中的数据，
再运行 `sudo pacman -Syu piano-sensors`。重启后用 `monitor-sensor` 查看传感器。

更新内核前，运行 `sudo bootctl --print-esp-path` 确认启动分区已挂载。使用 pacman 的新镜像挂在 `/boot`，Debian 挂在 `/boot/efi`。重启后如果新版内核不能启动，在 systemd-boot 的三秒菜单里选择之前的内核。重新刷系统镜像用于重装或更换发行版，会覆盖已有 Linux 文件；操作前先备份。

HyperOS 更新后，在引导助手的 **维护** 页点 **重新安装引导**，恢复合体 BOOT。

## 出问题怎么办

启动失败、屏幕停在 Fastboot 或想回 Android，见[故障恢复](recovery.md)。助手不能用时，可以用[电脑安装](install-from-computer.md)更新已有分区。
