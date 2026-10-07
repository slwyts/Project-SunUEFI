# Piano 设备配置层

这里保存与发行版、桌面选择分开的设备配置。来源是固定提交的公开 `debian-piano` overlay，保留 MIT 许可和逐文件 SHA256；配置包为 `piano-device-config`，Debian 包 `Architecture: all`，Arch 配方 `arch=(any)`。各目标的依赖解析和设备运行仍未测试。

默认包含 UCM、驱动加载参数与硬件服务单元。`--feature camera` 才加入真实的相机 udev / WirePlumber 配置及服务，仍需对应 runtime 和同内核 v4l2loopback。没有新增 libinput 补丁或空的“支持”目录。服务不自动启用。

键盘背光走内核 LED 与 UPower 标准接口。`upower.service.d/20-piano-keyboard.conf` 让 UPower 等待键盘初始化，避免初次扫描漏掉背光节点；这份文件也由 Debian release 适配工具安装。服务和程序的具体来源见 [Linux 服务源码地图](../../docs/devel/linux-services.md)。

蓝牙 HID 使用标准 `etc/modules-load.d/piano-bluetooth.conf` 加载 `uhid`，这是 Project SunUEFI 的本地配置。内核对应 `linux/configs/piano-bluetooth.config`，Full / Next 构建只允许新增 `CONFIG_UHID=m`；模块必须匹配目标内核。该配置解决 BlueZ 创建 HID 输入设备的前提，不代表笔尖坐标、压力或倾斜已经支持。

UCM 的 DMIC1、双通道和 DEC0 Volume 84 修正，来自现有 `stage_piano_ram_hardware.py`，差异保存在 `patches/0001-ucm-dmic1-gain0.patch`，原始、补丁和派生文件哈希均有记录。该适配器把 84 标为 0 dB；麦克风底噪仍未解决。显示服务只保留通用 display-manager 顺序，相机 modprobe 去掉 Debian 特定路径。

不提取桌面/GDM/dconf、账号、machine-id、SSH、软件源、fstab/swapfile/userdata、qbootctl 写槽或固定 USB 地址策略。`80-drivers.rules` 会覆盖 systemd 同名规则并广泛禁止自动加载，因此未纳入；不能把这份配置包当作已经验证的完整 BSP。

```sh
python3 tools/package_bsp.py inspect --target debian
python3 tools/package_bsp.py stage --target debian --output /absolute/new-stage
python3 tools/package_bsp.py package --target debian --format deb --output /absolute/new-package --version 0.1.0
python3 tools/package_bsp.py package --target arch --format tar --archrecipe --output /absolute/new-arch-recipe
```

Debian 默认 trixie，Ubuntu 默认 noble，Arch Linux ARM 默认 rolling；Deepin 必须显式传 `--suite`。同样的配置不代表原生程序 ABI 相同：runtime、固件、kernel modules 和 Mesa 是独立目标层，组装系统时必须与 distro/suite/aarch64 一致。包目录的 `manifest.json` 提供 target、payload 元数据和实际包文件的 SHA256。Arch 的 tar/PKGBUILD 只是配方，仍需在目标环境构建成原生包。

安装前可用 `inspect --against /absolute/target-root` 只读检查文件和 dpkg/pacman 归属。UCM 使用标准 ALSA 路径；已有同名发行版文件或其他包归属时停止，不加宽泛 Replaces，也不使用 force-overwrite。Debian 包附只读 preinst 检查，Arch 依赖正常 pacman 文件冲突检查。没有通用 `install` 子命令；目标环境使用标准 APT/dpkg 或 pacman。

`layers.json` 记录 native runtime 的既有 producer、固件来源及 kernel 版本约束；不读取个人 build 输出。内核 GPL、源文件 BSD 等许可继续按各组件保留，固件另按原分发条款。Mesa 继续来自固定的公开 `piano-mesa` source：`layers.json` 保存其上游补丁路径与哈希，不复制 Mesa 源码或假设 Debian 二进制能跨发行版使用。
