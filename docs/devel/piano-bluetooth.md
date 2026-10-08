# Piano 蓝牙地址

当前 fresh root 的 UART/固件初始化成功，rfkill 未阻止无线，但 management
接口报告 `Unconfigured / missing public-address`，BlueZ 没有 Adapter1。
这需要补齐每台设备的真实 BDAddr 来源，不能用随机／公共 NVM 默认地址完成。

当前板级 `&uart14/bluetooth` 没有 `local-bd-address`。`btqca.c` 的
`qca_check_bdaddr()` 在 controller 地址仍等于 NVM tag2 默认值时设置
`HCI_QUIRK_USE_BDADDR_PROPERTY`；`hci_sync.c` 随后读取 controller firmware
node 的 `local-bd-address` 并通过已有 `qca_set_bdaddr()` 写入控制器。
未取得有效 public address 时，`mgmt.c:get_missing_options()` 会保留缺地址标志，
控制器不能作为正常 adapter 使用。固件完成日志本身不证明该地址已配置。

标准 ABI 是 **6 字节、最低有效字节在前** 的 `local-bd-address`，见
[上游 binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/net/bluetooth/bluetooth-controller.yaml)。
Piano 当前 QCA driver 不读取 `nvmem-cells / bd-address`；已有该接口的是 TI
`hci_ll`，不能只加一个 DT cell 就声称 QCA 已接入。也不能用
`qcom,local-bd-address-broken` 代替地址来源；它仅处理已有地址的错误字节序。

旧 7.2.6 的用户扫描成功记录存在。已查旧、新 root 的 `brhbtnv20.bin` SHA 均为
`8fe2ef20d2dca6532498ff383e0599846f872df3e188054d6086a116e5cf76b2`，
`btqca.c` 也相同；旧 radio 脚本／service 没有发现 BDAddr 导入动作。
这些证据保留旧成功事实，但没有证明当时地址来自哪个接口。2026-10-03 采集的
原厂 DTB 没有找到 BDAddr 属性，properties.json 也没有采集蓝牙属性。

本轮设备 owner 已将原厂 persist 以 `ro,noload` 挂载，取回
`/bluetooth/.bt_nv.bin` 后卸载。实际文件为 **6 字节 raw BDAddr，人类／MSB
顺序**，没有 BTNV1/TLV header。真实 ROM 的
`android.hardware.bluetooth@1.0-impl-qti.so` 中 `BluetoothAddress::GetLocalAddress`
构造 `/mnt/vendor/persist/bluetooth/.bt_nv.bin`，读取 6 字节，反转后复制到 QTI
输出。其 ASCII address 路径同样是解析六字节后反转，因此原始文件的顺序与
人类地址相同，Linux `local-bd-address` 应写入这六字节的逆序。
AIDL 实现实际依赖该 1.0 实现；不是套用其他设备的旧 NV record 格式。

文件已核对非全零／全 FF、unicast/universal、不是 QTI 拒绝的 sentinel。
地址只在本机 private 记录中。Root 可用标准 `btmgmt public-addr` 做一次控制器
定位对照；它仍是临时配置，不能替代产品的自动地址来源。
真实库来自固定原厂 OTA 的两个 SHA 校验 operation，未执行 vendor ELF；
源路径、函数地址、读取／交换指令和格式结果保存在本机分析记录中。

最小持久方案是显式安装／升级时只读取得本机 persist 文件，生成含标准
`local-bd-address` 的设备 DTB，再放入实际启动的 `boot.img`。当前
`PianoEspBootSource` 读取 `\EFI\Piano\stable\boot.img`；`PianoRawLinuxBoot`
消费 BOOTv2 内嵌 DTB，再处理 panel/chosen/initrd。**单独替换 ESP 的
`board.dtb` 不会改变该启动路径。** `package_release.py` 已使用固定 AOSP
mkbootimg 制作 BOOTv2/4096；设备配置应复用同一格式，保留 kernel/initrd
字节与命令行，记录原／派生 DTB 和 boot SHA，并在后续显式升级重新配置。

默认 product FDF 集成的是 `FatPkg/EnhancedFatDxe/Fat.inf`，尚未集成 ext4
读取链。UEFI 每次直接读 persist 需要另补 ext4 provider 与资源退出路径，范围
更大；本轮优先正式一次安装生成设备 DTB，通用固件和公开 DTS 不写地址。
正常启动仍由标准 QCA firmware-property 读取路径取得地址，不加隐藏 daemon、
随机 fallback、虚拟 adapter 或第二个 kernel profile。独立 `tools/provision_piano_bluetooth.py` 使用纯 Python 和现有 FDT parser/
writer，保持完整 BOOTv2 header（仅 DTB size/AOSP SHA1 ID 更新）、kernel、
initramfs 与命令行，并检查 DT 仅改变目标地址属性。无需克隆 AOSP 或 host FAT
工具。`export_installer.py` 同时导出 helper/FDT 模块并校验三份 Python source。

standalone `apply --execute` 先刷 generic ESP/root 并逐项记录 prefix readback，
回到正常 Android 后只读 raw6，挂载 `sunuefi_esp` 更新实际 boot.img、sync/核对
文件 SHA，再卸载。最终报告明确 ESP 已与 generic 不同；配置失败时保留已经
完成的 generic 读回阶段。`provision-bluetooth --execute` 只补配置，不重刷 root。
helper 已用本机真实 BOOTv2/raw6 生成文件并核对组件；没有执行设备安装流程。
在线标准设置及持久 DTB 的硬件结果仍由设备 owner 另行验证。
