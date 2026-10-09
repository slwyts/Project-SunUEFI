# Piano 蓝牙地址

公共镜像不包含某台平板的蓝牙地址。Linux 在无线初始化时自动补齐板载控制器缺少的地址，用户无需填写 MAC。安装器仍可预先写入标准设备树属性；直接刷写通用 ESP 时，Linux 也能自行完成配置。

## 地址来源与内核接口

同机原厂 Android 的 `BluetoothAddress::GetLocalAddress` 从
`/mnt/vendor/persist/bluetooth/.bt_nv.bin` 读取六字节地址，再反转字节顺序。
已核对的原厂文件是 raw6 格式，没有额外记录头；Android 的 AIDL 实现依赖这份
QTI HIDL 库。分析依据是原厂 `OS3.0.309.0.WPYCNXM` 的实际二进制与 persist 文件，
没有假定芯片提供可直接读取的 OTP 地址。

Linux 的 `hci_qca.c` / `btqca.c` 在控制器仍使用固件默认地址时，要求引导端提供
`local-bd-address`。HCI 核心读取该属性，再调用已有的 `qca_set_bdaddr()`。
这是六字节、低字节在前的标准属性，见
[通用蓝牙设备树绑定](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/net/bluetooth/bluetooth-controller.yaml)
和 [WCN7850 绑定](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/net/bluetooth/qcom,wcn7850-bt.yaml)。
原厂文件按人类地址顺序保存，传给设备树或 MGMT 时反转一次。

当前 QCA 驱动没有读取蓝牙地址的 NVMEM／OTP provider。仅添加 `nvmem-cells`
不能接通一个不存在的读取路径，`qcom,local-bd-address-broken` 也不能代替地址来源。
若缺少地址，固件下载和 UART 初始化可以成功，但控制器会保持 unconfigured，
BlueZ 不会提供可用适配器。

## Linux 自动配置

[`bluetooth-address.py`](../../linux/bsp/common/usr/lib/piano/bluetooth-address.py)
属于通用 BSP，由现有 `piano-radio.service` 的 `radio-start` 在 BlueZ 启动前运行一次。
它等待实际 Bluetooth MGMT 控制器事件，只处理板载 QCA 控制器。已配置的控制器直接
跳过，不重置地址或用户设置。

只有控制器报告缺少公共地址、并支持设置地址时，程序才临时挂载
`/dev/disk/by-partlabel/persist`，使用 `ro,noload,nodev,nosuid,noexec` 读取
`bluetooth/.bt_nv.bin`，然后立即卸载。通过标准 MGMT `Set Public Address` 提交
地址后，等待控制器重新出现在正常列表中，再让 BlueZ 接管。索引重新枚举，
不固定假设控制器一定是 `hci0`。见 [BlueZ MGMT 协议](https://github.com/bluez/bluez/blob/master/doc/mgmt-protocol.rst)。

若原厂地址不存在或无效，程序从 Qualcomm `socinfo` 提供的 SoC 型号和芯片序列号
派生稳定的六字节地址，使用固定版本标记和 SHA256，并标记为本地生成来源。
它不使用 `machine-id`、分区 UUID 或随机数，所以更换发行版和重装根系统不会改变
同一硬件标识派生的地址。该地址不冒充原厂地址；32 位芯片序列号也不构成全球
绝无重复的证明。原厂来源与硬件标识均不可用时明确报错，不给所有设备同一个地址。

程序只在初始化期间运行，有总超时，没有新增常驻服务，也不改写 persist 或原厂 NVM。
Python 和挂载工具依赖由各发行版 BSP 配方声明，蓝牙功能继续使用原生 HCI／BlueZ。

## 安装与更新

UEFI 的 `PianoEspBootSource` 读取 `EFI/Piano/stable/boot.img`，
`PianoRawLinuxBoot` 使用其中的 BOOTv2 内嵌 DTB。**只更新 ESP 的 `board.dtb`
不会改变这条启动路径。** 旧系统没有自动地址程序时，更新内核需保留实际启动文件的
地址属性；当前 BSP 可在启动时自行配置，不再要求安装器修改通用镜像。

电脑安装器和 Android 模块的默认刷写流程已取消强制地址注入，原厂地址文件缺失不会
阻止 ESP／root 安装。对旧系统仍保留显式工具：

* 电脑端 [`tools/provision_piano_bluetooth.py`](../../tools/provision_piano_bluetooth.py)，
  安装器的 `provision-bluetooth` 子命令可调用它。
* Android 原生工具 [`piano-bluetooth-provision.c`](../../android/native/piano-bluetooth-provision.c)
  保留 `piano-storage provision-bluetooth` 命令。

两条路径均保留内核、initramfs、命令行及其余设备树内容，只写目标蓝牙节点的
`local-bd-address`，更新 BOOTv2 的 DTB 大小和 AOSP SHA1。
Android 原生工具使用固定上游 libfdt 与 libmd，原子替换派生的启动文件并同步。
原文件和设备专属文件分别记录摘要，不把修改后的文件继续当作原始通用镜像。
地址配置失败会明确报告，不生成随机地址。

UEFI 当前没有用于读取原厂 persist 的 ext4 文件系统链。地址初始化留在 Linux BSP，
无需为 UEFI 增加这条读取依赖，也无需修改 BlueZ。公共 DTS 和发布包保持通用。

## 当前设备结果

2026-10-10，在 `7.2.9-piano-gnome-gd33a42990baf` 的实际启动文件中移除
`local-bd-address` 后正常重启，程序自动从 persist 提交地址，控制器完成初始化，
BlueZ 状态为 `Powered: yes`。读取完成后 persist 已卸载，无需安装器预配置。
SoC 派生逻辑也已核对同机输出稳定；本次控制器实际使用的是原厂地址。
蓝牙手写笔连接、压感绘画和
轻捏反馈可用，具体输入实现见 [触控笔协议](piano-pen-protocol.md)。

此前替换内核时误用未配置地址的通用 DTB，曾导致适配器消失；现在由启动程序补齐
这一来源，不再把设备专属 DTB 当作功能前提。
