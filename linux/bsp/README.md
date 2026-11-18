# Piano 设备配置层

这里保存与发行版、桌面选择分开的设备配置。来源是固定提交的公开 `debian-piano` overlay，保留许可和逐文件 SHA256。`piano-device-config` 在 Debian 上是 `Architecture: all` 的 `.deb`，在 Arch 上是 `makepkg` 构建的 `any` 包。

默认包含 UCM、驱动加载参数与硬件服务单元。`--feature camera` 才加入的相机 udev / WirePlumber 配置及服务，仍需对应 runtime 和同内核 v4l2loopback。没有新增 libinput 补丁或空的“支持”目录。服务不自动启用。

相机 unit 使用 `Type=notify`，需配套当前 `debian-piano 25babfe3ff5d8ddee98b1e0ea88152d69a0c01b1` 编译的 `piano-camerad`。它先创建 loopback 相机并发送就绪通知，再允许用户会话启动；相机采集仍按应用请求启动。

键盘背光走内核 LED 与 UPower 标准接口。`upower.service.d/20-piano-keyboard.conf` 让 UPower 等待键盘初始化，避免初次扫描漏掉背光节点；这份文件也由 Debian release 适配工具安装。服务和程序的具体来源见 [Linux 服务源码地图](../../docs/devel/linux/services.md)。

蓝牙 HID 使用标准 `etc/modules-load.d/piano-bluetooth.conf` 加载 `uhid`，这是 Project SunUEFI 的本地配置。内核对应 `linux/configs/piano-bluetooth.config`，Full / Next 构建只允许新增 `CONFIG_UHID=m`；模块必须匹配目标内核。该配置解决 BlueZ 创建 HID 输入设备的前提，不代表笔尖坐标、压力或倾斜已经支持。

`usr/lib/piano/bluetooth-address.py` 由现有无线初始化脚本运行一次，只为缺少地址的板载控制器读取原厂 persist。原厂地址不可用时使用 SoC 型号与序列号派生稳定地址；已配置的控制器直接跳过。该程序使用标准 Bluetooth MGMT，不增加常驻服务，也不写原厂分区。Python 和挂载工具由发行版配方声明，手工刷写通用 ESP 不再需要预先填写地址。

UCM 打开四路扬声器，PipeWire 的普通立体声流使用标准 `simple` 混音将左右各复制到同侧两只扬声器；日常界面只有一个输出。四路功放沿用原厂增益 235（−7.5 dB）。麦克风使用原生单声道 PCM，DMIC2/DEC0 提供信号，桌面输入为 MONO，默认数字增益为 +40 dB（`VA_DEC0 Volume=124`）。这份配置由 Debian、Nyarch 和 SteamOS 共用，没有额外虚拟音频设备。UCM 差异保存在 `patches/0001-ucm-piano-microphone-and-speakers.patch`，原始、补丁和派生文件哈希均有记录；音频配置也由 `stage_piano_ram_hardware.py` 安装。显示服务只保留通用 display-manager 顺序，相机 modprobe 去掉 Debian 特定路径。

不提取桌面/GDM/dconf、账号、machine-id、SSH、软件源、fstab/swapfile/userdata、qbootctl 写槽或固定 USB 地址策略。`80-drivers.rules` 会覆盖 systemd 同名规则并广泛禁止自动加载，因此未纳入；不能把这份配置包当作已经验证的完整 BSP。

```sh
./build.sh bsp inspect --target debian
./build.sh bsp stage --target debian --output /absolute/new-stage
./build.sh bsp package --target debian --format deb --output /absolute/new-deb --version 0.1.0
./build.sh bsp package --target arch --format pacman --output /absolute/new-pacman --version 0.1.0
```

Debian 默认 sid，Ubuntu 默认 noble，Arch Linux ARM 默认 rolling；Deepin 必须显式传 `--suite`。Arch 构建请用装有 `makepkg` 和 `base-devel` 的环境，以普通用户运行；`--format pacman` 直接生成 `.pkg.tar.zst`。包目录的 `manifest.json` 记录目标、文件清单和包的 SHA256。同一配置适用于多个发行版，但原生程序、固件、内核模块和 Mesa 仍需分别匹配目标系统。

安装到目标系统时用 `apt install ./piano-device-config_版本_all.deb` 或 `pacman -U ./piano-device-config-版本-1-any.pkg.tar.zst`。安装前可用 `inspect --against /absolute/target-root` 只读检查文件归属。两类包都保留管理员对 `/etc` 配置的修改；发行版或其他软件包已占用同名文件时停止，不强制覆盖。旧产品镜像曾直接复制这些文件，不能在仍有未登记文件的系统上直接装包。

`layers.json` 记录 native runtime 的既有 producer、固件来源及 kernel 版本约束；不读取个人 build 输出。内核 GPL、源文件 BSD 等许可继续按各组件保留，固件另按原分发条款。Mesa 使用 `config/mesa.json` 固定的官方 27 开发源码，以各发行版原生配方构建，不复制 Debian 二进制到其他发行版。830v1 芯片 ID 和 DirectX-Headers 修正已在官方源码中，无需回移植；SteamOS renderonly 的显示缓冲高度修正尚未合入，单独保留。
