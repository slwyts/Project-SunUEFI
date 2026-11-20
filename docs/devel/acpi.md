# ACPI 表

产品固件给 Windows 提供一组最小 ACPI 表：FACP、MADT、GTDT 和 DSDT。目的只是让
Windows 的 HAL 和自带驱动能枚举处理器、GIC、UFS 控制器与 xHCI 控制器，不代表
平板的其余硬件在 Windows 下可用。

表在 `uefi/platforms/pianoProbePkg/AcpiTables/`，由产品组装脚本复制到
`pianoProductPkg` 后编译。stage0 平台不启动操作系统，不生成 ACPI。

## 表和对象

| 表 | 内容 | 来源 |
| --- | --- | --- |
| FACP | PSCI 启动、`HW_REDUCED_ACPI`、无 PM／GPE 寄存器，DSDT 指针由 `AcpiTableDxe` 回填 | ACPI 6.0，平台自身 |
| MADT | 8 个 `GICC`，1 个 `GICD`，1 个 `GICR` | 板级 DTB |
| GTDT | ARM 通用定时器，GSIV 29／30／27／26，计数器基址 0 | 板级 DTB 的 `arm,armv8-timer` |
| DSDT | 8 个 `ACPI0007`、`UFS0`、`XHC0` | 板级 DTB |

寄存器与中断取值来自 `vendor/piano/dtb/piano.dtb`，由
`tools/compose_piano_dtb.py` 读取。UEFI 代码里的 `0x1D84000` 和 `0xA600000`
与 DTB 一致。

### 设备树节点到 ACPI 对象

| 设备树节点 | ACPI 对象 | 标识 | 资源 |
| --- | --- | --- | --- |
| `/cpus/cpu@0` … `cpu@500`、`cpu@10000`、`cpu@10100` | `\_SB.C000` … `\_SB.C007` | `_HID "ACPI0007"`，`_UID` 0…7 | MADT `GICC`，MPIDR 0、0x100、0x200、0x300、0x400、0x500、0x10000、0x10100 |
| `/soc/interrupt-controller@16000000` | 不建 DSDT 对象 | — | `GICD` 0x16000000，长度 0x10000；`GICR` 0x16080000，长度 0x200000，间距 0x40000；维护中断 GSIV 25 |
| `/soc/timer` | 不建 DSDT 对象 | — | GTDT：安全 EL1 29、非安全 EL1 30、虚拟 27、非安全 EL2 26；DT 标为低电平，但 Qualcomm 自己的 ACPI 把通用定时器中断写成边沿／高有效，这里跟后者 |
| `/soc/ufshc@1d84000` | `\_SB.UFS0` | `_HID "QCOM24A5"`，`_UID` 0，`_CCA` 1 | `Memory32Fixed` 0x1D84000／0x1C000，`Interrupt` GSIV 297（SPI 265） |
| `/soc/ssusb@a600000`、`dwc3@a600000` | `\_SB.XHC0` | `_HID "PNP0D10"`（Microsoft 的通用 xHCI 标识），`_UID` 0，`_CCA` 1 | `Memory32Fixed` 0xA600000／0x100000，`Interrupt` GSIV 165（SPI 133），共享 |

`GICC` 的 GICR 基址逐个写出，因为 Qualcomm 的重分发器间距是 0x40000，不是
架构默认的 0x20000。设备树里的 `arm,armv8-timer` 没有寄存器窗口，计数器通过
系统寄存器访问，所以 GTDT 的两个计数器基址都是 0。

`QCOM24A5` 取自 `sunflower2333/mu_wide_platforms` 的 `Platforms/PakalaPkg/ACPI`
（DSDT 头写 SM8650）——同一 UFS IP，基址同为 0x1D84000，中断同为 0x129；
Xiaomi nabu 和 OnePlus aston 的原厂 DSDT 也是这个名字。Piano 自己的 DSDT 没有
公开（`mu_wide_platforms` 的 `Platforms/` 和分支里都没有 piano），所以这是目前
能拿到的最近命名。`PNP0D10` 是 Microsoft 文档规定的标准 xHCI `_CID`；这次把它
直接当 `_HID` 用，交给 Windows 自带的 xHCI 驱动，不冒充 Qualcomm 的 URS 结构。

### 设备树没有、这次也不补的内容

- `_PR0`／`_PR3` 和 PowerResource：Windows 不会主动打开这些设备的时钟和电源，
  交接时固件保持它们已初始化。
- `_DSD`、GPIO 复位和 SMMU 流 ID：没有 SM8750 的 Windows SMMU 驱动，DSDT 不
  伪造这些关系。
- UFS 的 ICE／MCQ 窗口：DTB 里还有 `0x1D88000`、`0x1DA4000` 和 `0x1DA5000`
  三段。0x1C000 的长度覆盖前两段（HCI 和紧邻窗口）；后两段是独立 IP，等实测
  需要再补。
- `_UPC`／`_PLD` 和 USB 口子设备：只描述控制器，USB 口枚举留到 WinPE 实测后补。
- 处理器容器（`ACPI0010`）和 PPTT：本次用 8 个平铺的 `ACPI0007`，拓扑信息
  由 MADT 的 MPIDR 提供。

## Windows 交接

`PianoLateHandoff` 在 ExitBootServices 时读取调用者的
`EFI_LOADED_IMAGE_PROTOCOL.FilePath`。路径里出现 `winload.efi`、
`bootmgfw.efi` 或 `bootmgr.efi` 时按 Windows 处理：只检查 TPL、内存稳定性和
owners 配置未变，不调用 `PianoProductOwnersRetire`，设备保持已初始化状态交给
Windows。日志出现 `PIANO_LATE_HANDOFF_TARGET target=windows keep=1`。

其他镜像沿用原来的拆除顺序：触屏、DWC3／xHCI、UFS、输入、时钟。拆除失败仍然
保留内存并停在 `CpuDeadLoop`，不继续交接。

怎么把这一版放到平板上、启动自备的 Windows 安装介质，见
[启动 Windows 安装介质](windows-boot.md)。

## 这次不做的子系统

| 子系统 | 原因 |
| --- | --- |
| GPU | SM8750 没有公开的 Windows WDDM 驱动；X 系列驱动按 SoC 和 INF 硬件 ID 绑定，不能跨 SoC 使用 |
| Wi-Fi／蓝牙 | WCN 模组需要厂商签名驱动、NVM 校准数据和固件下载，没有 SM8750 的公开驱动包 |
| 音频 | WCD9395、WSA884x 和 AudioReach DSP 需要厂商驱动与 DSP 拓扑 |
| 摄像头 | CAMSS／TFE 需要相机与传感器驱动，以及 `_PLD`／`_DSD` 描述 |
| 传感器 | 板载 MCU 走私有 I2C／SPI 协议，没有标准 HID 描述符 |
| 充电／PMIC | 电池方法要落到 SPMI／PMIC 寄存器，Windows 侧没有 SPMI 驱动 |
| 触摸 | NT36536 走 SPI，Windows 没有 HID-over-SPI 的 inbox 驱动 |
| 磁吸键盘 | GENI I2C SE6 上是私有 MCU 协议，不暴露标准 HID 描述符 |
