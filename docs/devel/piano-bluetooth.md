# Piano 蓝牙地址

公共镜像不包含某台平板的蓝牙地址。电脑安装器和 Android 模块在更新 ESP 时，自动读取本机原厂地址，写入 Linux 启动文件的标准设备树属性。用户无需填写 MAC 地址，Linux 启动后直接使用原生 QCA 驱动和 BlueZ。

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
原厂文件按人类地址顺序保存，因此安装时反转一次。

当前 QCA 驱动没有读取蓝牙地址的 NVMEM／OTP provider。仅添加 `nvmem-cells`
不能接通一个不存在的读取路径，`qcom,local-bd-address-broken` 也不能代替地址来源。
若缺少地址，固件下载和 UART 初始化可以成功，但控制器会保持 unconfigured，
BlueZ 不会提供可用适配器。

## 安装与更新

UEFI 的 `PianoEspBootSource` 读取 `EFI/Piano/stable/boot.img`，
`PianoRawLinuxBoot` 使用其中的 BOOTv2 内嵌 DTB。**只更新 ESP 的 `board.dtb`
不会改变这条启动路径。** 更新内核时必须从实际启动文件保留设备属性，或者重新
运行地址配置步骤。

* 电脑安装器使用 [`tools/provision_piano_bluetooth.py`](../../tools/provision_piano_bluetooth.py)。
* Android 模块由 `storage-operations.sh` 读取已挂载的原厂 persist，调用
  [`piano-bluetooth-provision.c`](../../android/native/piano-bluetooth-provision.c)
  提供的 `piano-storage provision-bluetooth` 操作。

两条路径均保留内核、initramfs、命令行及其余设备树内容，只写目标蓝牙节点的
`local-bd-address`，更新 BOOTv2 的 DTB 大小和 AOSP SHA1。
模块使用固定上游 libfdt 与 libmd；原始镜像完成读回检查后，原子替换派生的启动文件并同步。
原文件和设备专属文件分别记录摘要，不把修改后的文件继续当作原始通用镜像。
地址配置失败会明确报告，不生成随机地址。

UEFI 当前没有用于读取原厂 persist 的 ext4 文件系统链，安装时配置一次可以避免
为此增加启动期存储依赖。Linux 无需地址配置守护进程、BlueZ 补丁或额外的虚拟适配器。
公共 DTS 和发布包保持通用，每台设备在安装时使用自己的原厂地址。

## 当前设备结果

2026-10-10，`7.2.9-piano-gnome-gd33a42990baf` 正常重启后自动出现蓝牙适配器，
状态为 `Powered: yes`，无需再次执行临时地址命令。蓝牙手写笔连接、压感绘画和
轻捏反馈可用，具体输入实现见 [触控笔协议](piano-pen-protocol.md)。

此前替换内核时误用未配置地址的通用 DTB，曾导致适配器消失；补回实际启动文件的
标准属性后恢复。新模块已包含自动配置步骤，但本次没有为了验证它重新刷写整套
ESP／root，不能把独立工具与正常重启的结果扩大为完整刷写流程的实测。
