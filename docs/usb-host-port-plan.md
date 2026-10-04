# piano USB Host 源码审计与执行计划

2026-10-05。先完成只读源码审计，随后按授权实施独立 facade core 与主机测试。没有改动 USB Device 实验、prepare/build 脚本，没有固件构建、设备访问或 commit。USB Device 已有枚举证据；这不是 USB Host、VBUS source 或外接介质启动的证据。

## 结论与最小接入点

固定 Mu 树包含 XhciDxe、UsbBusDxe、UsbKbDxe、UsbMassStorageDxe 的完整源码与 INF。推荐独立 Host profile/平台适配器，提供**仅 USB0 使用的 PCI I/O facade**，把 MMIO 与共享 DMA/SMMU 接入现有 XhciDxe；上层 Bus/Keyboard 可以复用。Host 与当前 EP0 Device 不能同时拥有同一个控制器、USB0 SID40 或端口角色。

不能仅把 XhciDxe 放进 FV：它必须获得正确 PCI I/O/DevicePath、真实可用的 Host role、command/event/context DMA、必要的 64-bit register write quirk；外设还要求实际 Type-C/data role 与供电。当前未确认 source/VBUS 控制 ABI，不能从已工作的 device-mode session override 推断 Host 已供电。

## 固定源码与真实驱动链

已核对本地 HEAD：Mu-Silicium `66e7bd1e7bcb757d4b28629bd6409d7209d3b242`；Mu_Basecore `bb557081f80f4883ed832e34ab36bdca6ede1e10`。以下均为该本地固定树实际 INF/源文件，非其他设备的预编译 XhciDxe：

| 阶段 | Mu_Basecore 内模块路径 | 必需输入 | 产出 / 实际意义 |
| --- | --- | --- | --- |
| 平台适配 | 自有 `PianoUsbHostPciIoDxe`（待实施） | 验证过的 USB0 MMIO、role、shared DMA provider | PCI I/O＋DevicePath，尚不表示连接外设 |
| xHC | `MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf` | PCI I/O；Class 0x0C/0x03/0x30；BAR0 | EFI_USB2_HC_PROTOCOL，支持 LS/FS/HS/SS，并不表示任何设备已枚举 |
| USB Bus | `MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf` | USB2 HC＋父 DevicePath | USB_IO 子设备、设备/配置/接口/端点枚举、hub polling |
| 键盘 | `MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf` | USB_IO＋DevicePath，boot HID keyboard | SimpleTextIn / SimpleTextInputEx，需要真实 interrupt-IN key report |
| U 盘 | `MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf` 的受控只读变体 | USB_IO＋DevicePath，BOT 或 CBI transport | BlockIO / DiskInfo，需额外真实写屏障 |
| 文件 / 载入 | 已有 DiskIoDxe / PartitionDxe＋合适 FAT driver | 外接 BlockIO、分区、文件系统 | 读取 ARM64 EFI image；LoadImage/StartImage 与 OS 实际启动分别验收 |

首次键盘链仅需平台适配、XhciDxe、UsbBusDxe、UsbKbDxe；不需要 PCI 枚举全栈、USB Mass Storage 或 SCSI PassThru。首次 U 盘选择已知 BOT 设备，当前 stock mass transport 表只有 CBI0/CBI1/BOT，没有 UAS；不能假定任意 USB NVMe enclosure 可用。

INF library 依赖：

- XhciDxe：MemoryAllocationLib、BaseLib、UefiLib、UefiBootServicesTableLib、UefiDriverEntryPoint、BaseMemoryLib、DebugLib、ReportStatusCodeLib、TimerLib、PcdLib；消费 PcdDelayXhciHCReset。使用周期 timer，不必先移植平台中断 handler 才能进行轮询型 UEFI 验收。
- UsbBusDxe：MemoryAllocationLib、DevicePathLib、UefiLib、UefiBootServicesTableLib、UefiDriverEntryPoint、BaseMemoryLib、DebugLib、ReportStatusCodeLib。
- UsbKbDxe：上述基础 library 加 UefiRuntimeServicesTableLib、PcdLib、UefiUsbLib、HiiLib；HII Database 可用时读布局，否则受 PcdDisableDefaultKeyboardLayoutInUsbKbDriver 控制的默认布局。当前平台有 HiiDatabaseDxe，但应实际确认键盘 Start 成功与按键输入。
- UsbMassStorageDxe：BaseLib、MemoryAllocationLib、UefiLib、UefiBootServicesTableLib、UefiDriverEntryPoint、BaseMemoryLib、DebugLib、DevicePathLib。

FV 中模块 entry 仅安装 driver binding；平台 Handle 发布后仍需受控 `ConnectController(Handle, ..., Recursive=TRUE)` 和 protocol/child 枚举检查。不能用 dispatch/load 成功替代 Start 或真实设备结果。

## 固定 Mu PCI 桥的缺口

现成 `MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe` 可将 NonDiscoverable XHCI MMIO 转为 PCI I/O，registration library 也会生成 DevicePath，但这个固定 Mu 版本并非简单 identity-map 桥：

1. INF 加了 IoMmuLib；平台公共 DSC 选的是 `MdeModulePkg/Library/IoMmuLib/IoMmuLib.inf`。
2. `NonDiscoverablePciDeviceIo.c:851` 必须调用 IoMmuMap，随后 `:864` 调 IoMmuSetAttribute，DeviceHandle 传 **NULL**。对应 library 必须找到 gEdkiiIoMmuProtocol，否则 ASSERT / EFI_NOT_READY。
3. 当前 PianoOwnedSmmu 是私有 shared-DMA backend，并未发布这个标准协议。启用 native 全局 IOMMU 或用 IoMmuLibNull 的伪成功不能解决地址翻译，也会绕开现有隔离。
4. `PciIoAttributes():1802` 调 Initialize callback **忽略返回值**，随后标为 Enabled。不能只把 role/power 初始化塞进该 callback 并相信失败会阻止 xHC 启动。
5. `PciIoFlush():1676` 直接成功返回，不执行 CPU cache 同步。名字不能当作 ring coherency 证明。

因此优先自有 USB0 facade；若选择改现成桥，则必须解决设备身份、初始化错误传播与 DMA/cache 语义，不能把 NULL handle 的全局标准 IOMMU provider 当作已完成方案。

## DMA 适配必须覆盖的实际路径

固定 XhciDxe 已区分 Host/Device 地址，`UsbHcMem.c:75` Map common buffer，`:88–90` 保存 BufHost 和 MappedAddr；`UsbHcGetPciAddrForHostAddr()` / 反向函数做池内偏移翻译。可接入非 identity IOVA，不要求扩大成全局 identity map。

| PCI I/O 请求 | 共享 foundation 对应 | 约束 |
| --- | --- | --- |
| AllocateBuffer / FreeBuffer | 自有注册表＋PianoDmaAllocate / Free | 仅 tracked pages；保留失败 map/unmap/free 的 quarantine；初期拒绝 DAC/64-bit DMA attribute，继续已验证的 32-bit IOVA |
| BusMasterRead | controller 读取，PianoDmaToDevice | caller buffer 若不自有则用完整页 bounce；复制后 clean，再提交 |
| BusMasterWrite | controller 写入，PianoDmaFromDevice | DMA 接收 bounce；完成/取消确已停止后 invalidate，再复制回 caller |
| CommonBuffer | command/event/transfer rings、DCBAA、device/input/output contexts、scratchpad | CPU 与 DMA 持续共享，不能只一次 Begin/Complete 或用复制型 bounce 替代 |
| Unmap | 有界 token＋TLB sync | 调用者必须已完成/停止该 DMA；不能在 timeout 时凭成功返回释放 |
| MMIO | BAR0 有界 Read/Write | 宽度、alignment、Count×width、offset overflow 全检查；doorbell 前同步提交内容 |

**common-buffer cache 是当前实质缺口。**PianoDmaAllocate 当前要求 WB，Begin 之后禁止 CPU 修改 active ring；xHC 则在运行中写 command/transfer ring、读 event/output context。DT 的 dma-coherent 是重要平台信息，但现有 EP0 使用显式 cache sync，只证明了该路径有效，并未单独证明 xHC common-buffer 任意 CPU/DMA 更新均 coherent。必须选择并实测：验证 CPU/SMMU/device coherency；或扩展 shared allocator 提供正确的 uncached common buffers；或给 xHC 的 ring publish/consume 路径加精确 shared sync hook。单纯在 PCI Map/Unmap 加 clean/invalidate 不足以覆盖 common buffers。

现有 owned backend限制为 **16 MiB IOVA window、64 个 mapping token**。先读 HCSPARAMS2 scratchpad 数量、HCCPARAMS context size/AC64 和 PAGESIZE，计算初始 ring/context/scratchpad budget，超限时在发布控制器前拒绝；不能让 XhcInitSched 的 ASSERT/void 返回掩盖资源不足。UFS_MEM/SID60 及其他流必须保持原样；USB0 同一时刻只允许一个 owned domain。

## 必须考虑的 xHC 编程差异

[MiCode piano dwc3/host.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/dwc3/host.c) 为 xHC 加 **write-64-hi-lo-quirk**，其 [xhci-plat.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/host/xhci-plat.c) 启用 XHCI_WRITE_64_HI_LO。固定 Mu `XhciSched.c` 却按 low→high 写 DCBAAP (`605/606`)、CRCR (`630/631`)、ERDP、ERSTBA 与 ERDP 更新 (`1311/1312`)。

Host 变体应对这些**成对 64-bit 指针**用高→低 helper；同时保留 CRCR abort 等独立 32-bit RMW。不能让 facade 盲目缓存所有低半字等待高半字，因为不少合法操作只有低半字写入。Linux还设置 sg-trb-cache-size quirk；先审查短 transfer/64 KiB 边界/串行 command ring 是否触发相关问题，不应复制不明 quirk 或忽略它。

`XhcDriverBindingStop():2287` 与 ExitBootService 调 XhcHaltHC 后没有检查返回值，Stop 接着 XhcFreeSched。自有 mapping registry 必须能阻止未确认 halt 的 common-buffer unmap/free，并记录/保留 DMA 内存直到 cold reset；可在受控 xHC 变体中直接修正错误传播。不能因为标准 driver Stop 返回成功就释放 owned SMMU tables。

## SM8750 平台资源和角色

实际 `live.dts` 的 DWC3 child base **0x0A600000 / length 0xD93C**、SID **0x40**；Qualcomm glue 父范围 1 MiB。xHC 本身在 base 起始区域，DWC3 header 定义 xHC register region **0x0000–0x7FFF**，全局控制从 +0xC100 起；先以 **0x8000 BAR0** 限定 facade，再用真实 CAPLENGTH/DBOFF/RTSOFF/extended-cap 范围检查确认。不能将 +0xC100 当 xHC base。

| 资源 | 当前证据 | Host 阶段需要 |
| --- | --- | --- |
| GDSC | `gcc_usb30_prim_gdsc` 已可持有 | 保持到 xHC/consumers 停止 |
| core/NOC clocks | cfg_noc_usb3_prim_axi、aggre_usb3_prim_axi、usb30_prim_master、sleep、mock_utmi | 读取实际频率；DT core=200 MHz，主线同芯片 mock_utmi=19.2 MHz，不能以 Enable 成功代替 rate |
| SS clocks | usb3_prim_phy_aux、phy_com_aux、phy_pipe 已可持有 | PHY ref sources/pipe mux 和 USB3_DP_PHY GDSC 还须按实际路径核对 |
| M31 | base 0x088E3000；现有 device 时 CTRL0=01、COMMON0=6B、CTRL2=07 | Host flags、repeater tuning、EUD 处理；不要假设 device 保留状态足够 |
| repeater | PM8550B SID7 / FD00 ready-enable 已观察 | host override 与 FDE8/FDED BIT6 workaround；须有经过验证的 PMIC write owner/ABI |
| QMP DP combo | base 0x088E8000 / DT offset table | 真实 CC orientation/lane、mode、power/reset/status；设备模式 COM=01 不证明新 OTG 插头 orientation |
| data role | DRD/usb-role-switch，UCSI connector endpoint 接 usb0 | 确认 Host/DFP 与 peer USB data capability；不能只写 GCTL |
| power role/VBUS | 尚无 source 实测或已验证控制接口 | unpowered 键盘/U盘需要真实供电；powered PD dock 可有独立 power/data role，必须读实际状态 |
| SMMU | USB0 attach/detach arg=03000000、32-bit IOVA 已验证于 device DMA | Host No-Op completion 进一步确认实际流/地址；不能凭 DT 断言 Host DMA 已工作 |

[Linux SM8750 DTSI](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8750.dtsi) 也使用 base 0xA600000、SID40、200 MHz core / 19.2 MHz mock-utmi 和 dma-coherent；这是同芯片支持证据，piano 的 live DT/实测仍优先。

## VBUS、PHY 和 Type-C 的具体约束

Device 实验设置 `0x0A6F8810 |= 0x10100000` 与 `0x0A6F8830 |= 0x01000000` 是**给 DWC3 的软件 session/power-present 信号**，不会启用 PMIC 5 V boost。Host 不能沿用这些位冒充 source。实际 `usb0.extcon=<0x344>` 指 EUD，物理 role-switch 则走 PMIC GLINK/UCSI；也不能把 EUD extcon 当成物理供电状态。

[MiCode dwc3-msm-core.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/dwc3/dwc3-msm-core.c) 的 start_host 路径先尝试可选 vbus_dwc3 regulator，再设置 PHY_HOST_MODE、恢复电源/时钟、设置 core rate、通知 PHY/redriver、切 DRD role。piano live DT 没有 vbus_dwc3-supply，因此该函数的“optional regulator 不存在则返回成功”不是本机 5 V 输出证明。

保存的同机模块材料包含 ucsi_qti_glink，运行状态尚未本轮实测；其 [piano 源码](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/typec/ucsi/ucsi_qti_glink.c) 使用 PMIC GLINK owner **32779**、read/write/notify opcode **0x11/0x12/0x13** 和 48-byte UCSI buffer。此信息足以定位协议，但还没有 native UEFI GLINK client/ADSP service/role-control ABI 的实测。需要先确定可运行的 provider，取得 connector status、data role、power role、orientation、VBUS；控制后 re-query，不能把请求 ACK 当作 role/supply 已改变。

本机 repeater DT 有独立 host-param-override：例如 FD51=0F、FD54=01、FD56=03、FD57=05，而 inherited device/XBL 观察为 FD51=0A、FD54=03、FD57=03。[piano repeater driver](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/repeater/repeater-qti-pmic-eusb2.c) 在 Host 另设 FDE8/FDED bit6，Device 清除。当前 observer 只读权限不构成 PMIC 写能力，不能把 tuning 或 5 V 操作混进未验证原生 PmicDxe 路径。

[M31 driver](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/phy/phy-msm-m31-eusb2.c) 在 Host 遇到 active EUD 会经 SCM 禁用，后续恢复；这不能用普通 MMIO 猜测替代。QMP [lane selection](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/phy/phy-msm-ssusb-qmp.c) 由 PHY_LANE_A/B 产生 Type-C select=2/3，须先 power-up，再更新。DT 的 USB3_DP_COM_TYPEC_CTRL offset=0x10，所以地址 **0x088E8010**；不能只看 COM mode +0x00。

本机 WCD939x @0e 为 disabled、FSA4480 @42 为 ok，不能强制启用 WCD 路径。首验优先普通 USB2 boot keyboard，以减少 SS lane 变量，但它仍需要真实 OTG/DFP 与 VBUS。

## 可执行的阶段顺序与验收

1. **补平台观测。**在已有 Android/硬件工作环境使用已知 OTG 键盘只读记录当前 data/power role、orientation、供电、实际模块和时钟/PHY/repeater状态；另验证同一外设确实能枚举。随后 RAM 固件通过电脑启动时初始角色已改变，不能假设 Android 的 Host 状态保留。Host 窗口需明确 unplug PC→接 OTG 外设，日志由 RAM console 回收；同一物理端口不能同时提供现有 Device PC 诊断与 Host peripheral。
2. **独立 host-cap / No-Op profile。**内部测试时无电脑/外设连接，不发布 USB Bus，不执行 PORTSC power/reset。先停止 device engine 并确认 DSTS.DevCtrlHlt，恢复其 session 设置与 DMA；只在角色 owner 转交完成后切 GCTL.PRTCAPDIR=1。记录 GSNPSID(+0xC120)、VER_NUMBER(+0xC1A0)/VER_TYPE(+0xC1A4)、cap length、HCSPARAMS/HCCPARAMS、page size、ports、doorbell/runtime offsets；根据实际 DWC3 版本执行 role-switch reset，不能只由 ID=33313130 推断精确 revision。初始化受控 ring、DCBAA/ERST/scratchpad，按 hi→lo 写指针，发 **No-Op Command TRB type23**，要求 **Command Completion Event type33 / success code1 / 返回正确 command IOVA**。这只验证 xHC engine＋DMA，不能宣称外设已通。
3. **实施/测 PCI facade。**仅 USB0，测试 fake MMIO 范围/overflow、map方向、非 identity IOVA/offset、token预算、重入、错误取消、unmap/halt失败保留；解决 common-buffer coherence 后再让标准 XhciDxe Start。使用共享 foundation，不能另写 identity/bypass DMA。
4. **完成物理 Host profile。**获得已验证 role/source provider，确认实际角色及供电，落实 host PHY/repeater/clock/rate、必要版次 quirk；逐项保存并读回，失败先 halt，不把供电操作和 PCI 发布伪装成 success。只有准备成功才发布控制器 handle。
5. **键盘。**连接 XhciDxe→UsbBus→UsbKb，要求 PORTSC connect/speed、地址/描述符、interrupt-IN report、ReadKeyStroke 实际按键。再核对 disconnect/reconnect 和回收路径。HID LED/control OUT 不属于块存储写入，不应因只读介质目标禁用必要的 USB 初始化。
6. **只读 U 盘。**选择已知 BOT U 盘；加入真实 read-only mass driver变体，验证容量、GPT/MBR、文件读取与 SHA-256，持久化 WRITE 命令计数保持 0，再接 DiskIo/Partition/FAT。错误和热拔插先停止 DMA，再释放。
7. **实际启动。**从外接 U盘读取明确的 AArch64 EFI image，验证 Machine=0xAA64、文件 hash、LoadImage/StartImage 结果。用 RAM-only nonce/返回程序先证明 EFI transfer；然后结合平台 DT/ACPI、interrupt/SMMU 与外设 power policy 验证 OS 进入/早期日志。PE 已加载、看到菜单或 EBS 调用都不是 Linux/Windows 启动完成。

现 stock `UsbMassImpl.c:303–309` 即使 Media.ReadOnly 也进入 WRITE(10)/(16)。首验必须在 WriteBlocks 到 transport 之前硬返回 EFI_WRITE_PROTECTED，并发布真实 ReadOnly media；仅改属性、只选择只读 Open 模式或相信 FAT driver 不写不够。

## 清理、EBS 和资源边界

正常退出先停止 USB child consumers/async transfers，再 xHC halt；核对 USBSTS.HCHalted/CNR、doorbell/command/event 状态后才 unmap/free shared buffers、detach USB0并读回、恢复 PHY/session/role供电与 clocks。Halt/map/TLB sync/detach失败保留资源并走诊断 cold reset，不能回收仍可能被 DMA 访问的表或 pool。其他 SMMU streams、UFS 与当前 Device 基线保持可比较。

EBS 要使用统一、有证据的 halt-only 生命周期，不能依赖不同 driver notification 的执行顺序释放 SMMU。OS 后续必须有真实 xHC/PHY/Type-C/SMMU 重新接管方案；电源角色是否保留给 OS、是否会使外接 rootfs 掉电，要作为 handoff 行为验证，不可把 UEFI 外接文件读取成功扩展成 OS 运行时 USB 已支持。

## 下一段代码的建议范围

独立 `PianoUsbHostPci` core 与 fake-MMIO/DMA tests 已按后续授权实施，文件为 `bootprofiles/uefi-app/PianoUsbHostPci.c/.h`、`tools/test_usb_host_pci.c`。它输出真实 `EFI_PCI_IO_PROTOCOL` function table，但不 InstallProtocol、不改变控制器/PHY/供电，也未连接实际 backend。ABI 可由标准 XhciDxe 调用，硬件 Host 仍待接线与实测。

## 已实现 core 的 API 与 backend contract

`PianoUsbHostPciInit(Context, Backend, Limits)` 要求首次 Context 零初始化，默认限额为 16 MiB、64 allocation records / 64 mapping records；初始化不调用 hardware callbacks。PCI config 固定 class0C/03/30、BAR0=0xA600000/0x8000；只允许 BAR sizing probe/原地址恢复，禁止 relocation、修改 class、IO/DAC/ROM、pass-through BAR。PCI command 仅允许 memory/bus-master；MMIO read 需要 memory decode，write 需要 memory＋bus-master 与 Ready。支持 ordinary 8/16/32/64-bit read、32-bit/明确 high→low 的 64-bit write；sub-DWORD MMIO write 不做可能触发 W1C 的隐式 RMW。FIFO/fill、IO port/copy-memory 明确 unsupported。

Backend 明确提供：

- Ready：检查真实 USB0 host owner、role/clocks/SMMU readiness；无回调返回 NotReady。
- Read32/Write32：BAR-relative、有 ordering 的 MMIO；Write32负责 CPU ring publish 到 doorbell 的 release barrier。缺回调不是成功。
- SetBusMaster：enable 只允许 streaming/common 能力及 map/retire/allocate/free/quiesce 回调齐全；disable callback 成功必须已经实际 halt。任何 enable/disable失败标记 controller uncertain，保留原 attribute 状态；recover 即使 enable 部分成功、逻辑 BUS bit尚为0也会执行 disable。
- Allocate/Free：返回 tracked、page-aligned CPU storage、实际 EFI cache attributes 与 native ownership token。common 必须显式声明 COHERENT 或 UNCACHED；UC-only backend 返回 WB、混合 cache模式、RO/RP storage会报错并 quarantine。runtime/DAC/WC allocation request不虚构支持。PARTIAL_FREE 是独立能力；未声明则拒绝 partial free；声明后支持头/尾/中间页释放与 registry splitting，满足 xHC alignment helper 的用法。
- Map：明确 controller-read→ToDevice、controller-write→FromDevice、common→Bidirectional；streaming backend负责 owned bounce/cache，common不得复制型 bounce。要求完整请求长度、32-bit有效 IOVA、独立 native token、exclusive IOVA pages；tracked allocation不能借“external range”跨界。Core提供全局不复用的 opaque cookie，不能把 native token、foreign/stale cookie用于 Unmap。
- Retire：成功意味着该 DMA已退休、cache与TLB sync完成；失败保留native token。普通 streaming Unmap可 copy-back；任何 rollback/quarantine recovery使用 CopyBack=FALSE，不再写过期 caller。
- Quiesced/Flush/Stall：必须提供实际证明/操作；common Unmap及recover必须 Quiesced成功。Flush缺失返回 Unsupported；PollMem无Stall时只允许一次即时匹配/超时，正延时不能伪造等待。

allocation bytes 与 IOVA bytes分别受限额约束。部分 map、无效返回、页越界/alias、失败 retire/free均留账并冻结新增DMA资源。未知/重复 native token不能靠猜测 retire成功；`PianoUsbHostPciRecover()` 只在 halt确认后清理能证实释放的记录，其余保持quarantine直到外部诊断 cold reset。页/属性/预算校验失败不会让标准Driver得到成功或默认identity地址。

`PianoUsbHostPciWrite64HiLo()` 与 Mem.Write(Uint64)明确高→低，但**普通 DWORD write仍立即写原offset**；没有改Mu XhciSched，因此现有成对low→high调用还必须由后续受控Host变体修正，不能宣称此helper已自动解决stock driver顺序。

core是同步、boot-services范围；caller/发布层需在适当TPL串行调用，Busy拒绝backend重入到有副作用/硬件操作。DevicePath/handle发布、真实CBCap声明、SMMU/cache wrapper、role/source provider、consumer连接、EBS/quiet生命周期仍未实施。当前 shared allocator WB-only时应保持CBCap=0并收到Unsupported，不能为了让Start通过而声明未经证明的coherent/UC能力。

## core 主机验证

actual-source tests 覆盖 Xhci用到的PCI class/BAR/attributes/MMIO路径、full-range与overflow拒绝、hi→lo及单DWORD行为、callback reentry、明确CBCap/cache-policy门控、非identityIOVA/offset与方向、预算/cookie/foreign与stale token、partial page freeing、native NULL-success/alias/部分map、retire/free/enable/disable失败保留，以及halt recovery不copy-back。已通过 ASan、UBSan 与 leak detection；不访问任何真实MMIO或设备。

```sh
cc -std=gnu11 -fshort-wchar -Wall -Wextra -Werror -Wno-unused-parameter \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  -I upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include \
  tools/test_usb_host_pci.c -o /tmp/sunuefi-test-usb-host-pci-asan
ASAN_OPTIONS=detect_leaks=1 /tmp/sunuefi-test-usb-host-pci-asan
```

固定参考 [OnePlus 13 状态表](https://github.com/Project-Silicium/Mu-Silicium/blob/66e7bd1e7bcb757d4b28629bd6409d7209d3b242/Status.md) 的 UEFI Host/PD仍为不可用；其 Device/Mass Storage 与 Linux可启动状态不能当作 piano Host 移植成功模板。
