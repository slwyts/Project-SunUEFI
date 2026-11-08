# Piano 蓝牙配置

UEFI 启动时从原厂 `persist` 只读获取蓝牙地址，通过设备树的标准 `local-bd-address` 属性交给 Linux。HCI 核心与 QCA 驱动完成控制器配置，原版 BlueZ 可以直接使用，不需要项目的地址初始化程序。公共镜像和 ESP 不保存某台平板的 MAC，安装器不改写设备专属地址。

## 板级 NVM 与连接距离

WCN7861 使用 p81 板级 `brhbtnv20.b2e`，蓝牙键鼠等外设的连接距离正常。
[`piano-bluetooth-nvm.dtso`](../../../linux/dts/piano-bluetooth-nvm.dtso) 已接入共用设备树构建，只替换 NVM，rampatch 保持 `brhbtfw20.tlv`。

控制器初始化时报告的 board ID 为 `0x0000`，驱动无法自动选出板级文件。
通用 `brhbtnv20.bin` 缺少 p81 射频前端校准，会导致外设只能在约 20 cm 内连接。
板级文件随共用固件提供，与原厂 `bluetooth_a` 中的同名文件一致；此修复不改变本机蓝牙地址的来源。

## 地址来源与内核接口

同机原厂 Android 的 `BluetoothAddress::GetLocalAddress` 从
`/mnt/vendor/persist/bluetooth/.bt_nv.bin` 读取六字节地址，再反转字节顺序。
已核对的原厂文件是 raw6 格式，没有额外记录头；Android 的 AIDL 实现依赖这份
QTI HIDL 库。分析依据是原厂 `OS3.0.309.0.WPYCNXM` 的二进制与 persist 文件，
没有假定芯片提供可直接读取的 OTP 地址。

Linux 的 `hci_qca.c` / `btqca.c` 在控制器仍使用固件默认地址时，要求引导端提供
`local-bd-address`。HCI 核心读取该属性，再调用已有的 `qca_set_bdaddr()`。
这是六字节、低字节在前的标准属性，见
[通用蓝牙设备树绑定](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/net/bluetooth/bluetooth-controller.yaml)
和 [WCN7850 绑定](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/net/bluetooth/qcom,wcn7850-bt.yaml)。
原厂文件按人类地址顺序保存，传给设备树或 MGMT 时反转一次。

QCA 驱动没有读取这个 ext4 文件的 NVMEM／OTP provider。不能只添加
`nvmem-cells`，也不应让内核解析 Android 文件系统；引导固件负责把板级身份
转换为已有的设备树属性。`qcom,local-bd-address-broken` 是字节序兼容标志，
不能代替地址来源。

## 固件初始化与系统交接

[`PianoBluetoothAddress.c`](../../../uefi/components/board-identity/PianoBluetoothAddress.c)
在 UFS 文件系统就绪后查找唯一的原厂 `persist` 卷，仅以读模式打开
`bluetooth/.bt_nv.bin`。文件必须恰好六字节，地址不能全零、全 `FF` 或为多播／
本地地址。文件句柄在读取后关闭，数据只保留在本次启动的固件内存中，
不修改原厂文件、NVM 或分区。

原厂文件缺失或无效时，固件通过 Qualcomm ChipInfo 读取 SoC 型号和芯片序列号，
使用与既有实现相同的 SHA256 派生规则生成稳定地址，并标记为本地生成来源。
不使用 `machine-id`、分区 UUID 或随机数，更换 rootfs 不改变这一身份。
32 位芯片序列号不保证全球绝无重复；两个来源均不可用时记录错误，不给所有设备
一个固定地址，也不阻止其他硬件和系统启动。

标准 EFI 设备树修正、Linux EFI 启动会话和旧 BOOTv2 路径共用同一份地址。
只处理唯一的板载 WCN7850／WCN7861 节点，保留调用者已提供的有效地址，
为空或缺失时填入 `local-bd-address`。配套固件必须与移除地址初始化程序的 BSP
一起更新；旧固件不会自动获得这项交接能力。

蓝牙手写笔连接、压感绘画和轻捏反馈的实现见[触控笔协议](pen-protocol.md)。
