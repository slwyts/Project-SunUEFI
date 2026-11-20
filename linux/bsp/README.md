# Piano 设备配置层

这里保存与发行版、桌面选择分开的设备配置。来源是固定提交的公开 `debian-piano` overlay，保留许可和逐文件 SHA256。`piano-device-config` 在 Debian 上是 `Architecture: all` 的 `.deb`，在 Arch 上是 `makepkg` 构建的 `any` 包。

通用包包含 UCM、驱动加载参数和硬件服务单元，也为标准 DMA-BUF system heap
设置会话访问权限，应用分配相机缓冲不依赖相机服务。相机服务和它的 WirePlumber
配置由独立的 [`piano-camera`](../../docs/devel/hardware/camera-controls.md) 安装。
相机包依赖通用包中的前后摄模块参数，不需要定制 rootfs。

笔的 BlueZ 初始化程序、systemd 服务和无线参数由 `piano-device-config`
一起安装。包管理器会安装 BlueZ 和 Python GI；不需要从镜像复制脚本，
也不绑定某个内核版本。坐标、压感和按键仍由匹配的内核驱动上报。

Goodix 的设备访问和状态目录由 `piano-device-config` 的 fprintd 配置提供。
升级会移除未经修改的镜像旧默认配置，保留管理员覆盖和已录入的指纹。
仍需匹配的 libfprint、内核和指纹固件，见[侧指纹](../../docs/devel/hardware/led-ir-fingerprint.md#侧指纹)。

键盘背光走内核 LED 与 UPower 标准接口。`upower.service.d/20-piano-keyboard.conf` 让 UPower 等待键盘初始化，避免初次扫描漏掉背光节点；这份文件也由 Debian release 适配工具安装。服务和程序的具体来源见 [Linux 服务源码地图](../../docs/devel/linux/services.md)。

蓝牙 HID 使用标准 `etc/modules-load.d/piano-bluetooth.conf` 加载 `uhid`，这是 Project SunUEFI 的本地配置。内核对应 `linux/configs/piano-bluetooth.config`，Full / Next 构建只允许新增 `CONFIG_UHID=m`；模块必须匹配目标内核。该配置解决 BlueZ 创建 HID 输入设备的前提，不代表笔尖坐标、压力或倾斜已经支持。

`usr/lib/piano/bluetooth-address.py` 由现有无线初始化脚本运行一次，只为缺少地址的板载控制器读取原厂 persist。原厂地址不可用时使用 SoC 型号与序列号派生稳定地址；已配置的控制器直接跳过。该程序使用标准 Bluetooth MGMT，不增加常驻服务，也不写原厂分区。Python 和挂载工具由发行版配方声明，手工刷写通用 ESP 不再需要预先填写地址。

UCM 打开四路扬声器，PipeWire 的普通立体声流使用标准 `simple` 混音将左右各复制到同侧两只扬声器；日常界面只有一个输出。四路功放沿用原厂增益 235（−7.5 dB）。麦克风使用原生单声道 PCM，DMIC2/DEC0 提供信号，桌面输入为 MONO，默认数字增益为 +40 dB（`VA_DEC0 Volume=124`）。这份配置由 Debian、Nyarch 和 SteamOS 共用，没有额外虚拟音频设备。UCM 差异保存在 `patches/0001-ucm-piano-microphone-and-speakers.patch`，由 `piano-device-config` 安装。

不提取桌面/GDM/dconf、账号、machine-id、SSH、软件源、fstab/swapfile/userdata、qbootctl 写槽或固定 USB 地址策略。`80-drivers.rules` 会覆盖 systemd 同名规则并广泛禁止自动加载，因此未纳入；不能把这份配置包当作已经验证的完整 BSP。

```sh
./build.sh bsp inspect --target debian
./build.sh bsp stage --target debian --output /absolute/new-stage
./build.sh bsp package --target debian --format deb --output /absolute/new-deb
./build.sh bsp package --target arch --format pacman --output /absolute/new-pacman
```

Debian 默认 sid，Ubuntu 默认 noble，Arch Linux ARM 默认 rolling；Deepin 必须显式传 `--suite`。Arch 构建请用装有 `makepkg` 和 `base-devel` 的环境，以普通用户运行；`--format pacman` 直接生成 `.pkg.tar.zst`。包目录的 `manifest.json` 记录目标、文件清单和包的 SHA256。同一配置适用于多个发行版，但原生程序、固件、内核模块和 Mesa 仍需分别匹配目标系统。

四个产品镜像通过包管理器安装配置。装好软件源后可用 `apt install piano-device-config`
或 `pacman -S sunuefi/piano-device-config`；本地包则使用 `apt install ./<包名>.deb`
或 `pacman -U ./<包名>.pkg.tar.zst`。两类包都保留管理员改过的 `/etc` 配置。

旧镜像曾直接复制这些文件。迁移时先按安装报出的路径，用 `dpkg-query -S` 或
`pacman -Qo` 确认没有其他包拥有文件，再将同名旧文件移到备份目录，保留路径和权限。
装包成功后，逐项比较 `/etc` 中的旧设置，按需将改动合并回新配置；装包失败时可以原样
移回备份。其他包拥有的文件由该包处理，不用强制覆盖。`./build.sh bsp inspect --against
/absolute/target-root` 可在电脑上只读检查迁移目标。

之前改过 `/usr/share/piano/pen/p81c-radio.json` 时，把修改后的副本保存为
`/etc/piano/pen-radio.json`，新镜像的笔服务优先读取它。

`layers.json` 记录 native runtime 的既有 producer、固件来源及 kernel 版本约束；不读取个人 build 输出。内核 GPL、源文件 BSD 等许可继续按各组件保留，固件另按原分发条款。Mesa 使用 `config/mesa.json` 固定的官方 27 开发源码，以各发行版原生配方构建，不复制 Debian 二进制到其他发行版。830v1 芯片 ID 和 DirectX-Headers 修正已在官方源码中，无需回移植；SteamOS renderonly 的显示缓冲高度修正尚未合入，单独保留。

## 音频拓扑包

`piano-audio-topology` 提供 Qualcomm ASoC 驱动加载的开源 AudioReach 拓扑。
四个镜像通过 APT 或共用 `sunuefi` 仓安装，驱动从标准固件目录加载这个文件。

本地构建使用上面的 `bsp package` 命令加 `--audio-topology`。向旧系统安装前，
用 `bsp inspect --audio-topology --against /absolute/target-root` 检查同名文件
和压缩版本的归属。未登记的旧文件按上面的迁移方法备份；发行版软件包已有
同名文件时保留其归属。

本地使用原厂杜比扬声器处理时，给 `./build.sh bsp package --audio-topology`
加上 `--speaker-acdb /path/to/Mi_acdb_cal.acdb`。构建从匹配的 Piano 原厂 ACDB
提取 95 项扬声器参数，加入 AudioReach 拓扑的 DAP 模块。配套内核在播放准备时
分批发送完整参数，最后启用处理。ALSA UCM 仍提供同一个扬声器，DAP 路径接收
48 kHz 立体声并向 TDM 输出四路。普通拓扑没有这条路径时，UCM 使用原有四声道
PCM。该配置适用于本地打包，原厂调音数据随输入提供。

## 设备固件包

`piano-firmware-keyboard` 提供键盘套和触控板共用的固件
`/usr/lib/firmware/nanosic/MCU_Upgrade.bin`。手动构建用
`--keyboard-firmware vendor/firmware/piano-keyboard`。

`piano-firmware-touch` 提供 Xiaomi Pad 8 Pro 的 BOE、CSOT 两份触控固件。
内核按面板型号从 `/usr/lib/firmware/novatek/` 加载。包版本取自
[固件清单](../../vendor/firmware/piano-touch/manifest.json)的原厂 ROM 号。
手动构建用 `--touch-firmware vendor/firmware/piano-touch`。

两个包都从签名 APT 或共用 `sunuefi` 仓安装。旧镜像里和包内字节相同的原装文件
可以直接接管。第一次安装时，如果有人换过原文件，Debian 会提示冲突并保留原文件；
pacman 保留原文件，并将包内版本另存为 `.pacnew`。长期使用自定义固件时，把替换件
放到 `/usr/lib/firmware/updates/nanosic/` 或 `/usr/lib/firmware/updates/novatek/`，
文件名不变，内核优先读取那里。不要直接修改包管理的文件。
触屏在启动和唤醒时重新读取固件；如果没有可用固件，触屏将无法使用。

两个软件包都会拒绝覆盖旧系统里另有归属或未经登记的同名 `.xz`、`.zst` 固件。
要继续使用自定义固件，可以先解压，再移到上面的 `updates/` 目录。

### 本地安装的固件包

`piano-firmware-display` 包含 BOE、CSOT 两份显示校准；内核按面板型号加载
`qcom/sm8750/piano-{boe,csot}-display.bin`，不需要额外服务。`--display-firmware`
指定仓库中的 `vendor/firmware/piano-display`，或[校准生成工具](../../vendor/firmware/piano-display/README.md)
的输出目录。四个产品镜像通过原生包装入校准；校准包没有加入公网软件源。
构建会按[固件清单](../../vendor/firmware/piano-display/manifest.json)核对两份文件。

```sh
./build.sh bsp package --display-firmware vendor/firmware/piano-display \
  --against /absolute/target-root --target debian --format deb --output build/display-deb
./build.sh bsp package --display-firmware vendor/firmware/piano-display \
  --against /absolute/target-root --target arch --format pacman --output build/display-pacman
```

`piano-firmware-fingerprint` 包含 Goodix 的原厂签名程序和三个模型。
构建按[固件清单](../../vendor/firmware/hyperos4-fingerprint/manifest.json)逐个校验，
包版本取自原厂 ROM 号。文件原样安装到 `/usr/lib/firmware`。各成品镜像通过原生包装入；
需要单独安装时，在上面的构建命令中换用
`--fingerprint-firmware vendor/firmware/hyperos4-fingerprint`。指纹固件包不在公网软件源中。

SteamOS 与 Nyarch 使用相同的 `any` 格式 pacman 软件包。这两个包不在公网软件源中。
旧镜像若有未登记的文件，先按上面的迁移方法备份再构建；其他包拥有的文件不覆盖。
安装前用 `./build.sh bsp inspect` 加组件参数及 `--against <目标根>` 检查
文件归属和同名的 `.xz`、`.zst`；这两个本地 pacman 包只检查完全相同的路径。

把包复制到目标系统，用 `apt install ./<包名>.deb` 或 `pacman -U ./<包名>.pkg.tar.zst`
安装。换用保留的旧包可以回退，显式卸载用 `apt remove <包名>` 或 `pacman -R <包名>`。
显示校准在下次内核初始化内屏时加载；移除校准后，普通 SDR 仍可用。若系统把校准打进
了 initramfs，重启前按发行版的方式重建。
指纹固件在后端初始化传感器时加载；移除固件不会删除指纹录入。
