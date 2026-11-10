# UEFI 产品核心实现

本项目维护唯一的 UEFI 产品核心，编译生成产物 `artifacts/product/PianoUEFI-product.img`。合体 BOOT、`fastboot boot` 临时启动以及系统安装工具使用同一份核心，不再维护多套测试固件。

## 固件构建与组装

固件通过 `./build.sh uefi` 构建。其流程在 `build/firmware-workspace/` 独立工作区中进行，不改动 `upstream/` 中的上游子模块：

1. `tools/apply_firmware_patches.py` 应用 `patches/firmware/series.json` 中的补丁；
2. `tools/prepare_product_handoff.py`、`prepare_product_pump.py`、`prepare_product_ui.py`、`prepare_nv_runtime_guard.py` 注入晚期身份检查、事件 pump、菜单界面与变量运行保护；
3. `tools/build_simpleinit.sh --product-gui-pump` 编译 SimpleInit 图形菜单；
4. `tools/prepare_product.py` 以 `uefi/platforms/pianoProbePkg` 为基线，组装 `uefi/core/` 与 `uefi/components/` 的源文件，生成 `pianoProductPkg`；
5. `tools/build_stage0.sh product` 编译生成固件，`tools/build_integrity.py` 核对输入输出完整性，由 `tools/package_product.py` 封装生成产品镜像。

## 固件启动流程

产品核心从 `uefi/core/PianoProductCore.c` 的入口函数 `PianoProductCoreEntry` 开始执行：

```
PianoProductCoreEntry (uefi/core/PianoProductCore.c)
  │
  ├─► PianoProductAcquireSimpleInit: 获取并校验原厂传入的设备树 (FDT)
  ├─► PianoProductDisplayObserveStart: 探测并继承原厂 GOP 帧缓冲与显示状态
  ├─► PianoProductEarlyDxeRegister: 注册早期依赖
  ├─► PianoProductDisplayOwnerStart: 接管显示控制器与背光
  ├─► PianoBootPolicyStart: 初始化启动策略管理
  ├─► PianoGopDxeRegister / PianoLogoDxeRegister: 注册图形输出与启动画面
  ├─► PianoFvApplicationStart: 加载固件内部文件卷
  ├─► PianoRawLinuxRegister: 准备 Linux 加载支撑
  ├─► PianoUsbControllerServiceStart: 启动常驻后台的 USB 调试控制器
  ├─► PianoUfsProductStorageStart: 挂载只读 UFS 与独立存储分区
  ├─► PianoSimpleInitStart: 显示图形引导菜单与启动动画
  │
  ▼ 用户选择或倒计时超时
系统交接 (ExitBootServices) 或进入设置
```

## 原厂 UFS 分区只读保护

固件在 `uefi/core/PianoReadOnlyBlock.c` 中对原厂 UFS 分区实施代码级保护：

* **关键只读**：原厂 Android 相关的系统、配置与引导分区（如 `boot`、`init_boot`、`vendor_boot`、`super`、`userdata` 等）在 UEFI 运行期间一律只读，拒绝写操作；
* **允许写入**：仅针对 SunUEFI 独立创建的分区（`sunuefi_esp`、`sunuefi_root`）开放写入权限；
* **分区表校验**：固件仅对 GPT 结构、CRC 校验码和逻辑边界进行标准合规性核对，不与某次历史采集的整表快照比对。

## 安全退出 ExitBootServices

在启动 Linux 或执行重启前，UEFI 必须安全断开硬件并退出 ExitBootServices，防止发生外设总线冲突：

1. **USB 控制器断开**：停止 DWC3 端点传输，清空事件环，释放 USB DMA 缓冲区；USB 口处于主机模式时，先断开 USB 设备驱动，停止并复位 xHCI；
2. **UFS 存储安全退出**：完成已排队的传输，同步缓存，安全断开 UFS 控制器通道；
3. **时钟与电源释放**：释放显示时钟；磁吸键盘的 MCU 回到复位，恢复 L11B/L14B 的 RPMh 投票、引脚和 SE6 寄存器；触屏恢复引脚和中断配置；再释放 QUP1 时钟引用。键盘和触屏不使用 DMA，但输入协议、引脚、电源或时钟没能恢复时同样拒绝交接；
4. **SMMU 保护解除**：安全解绑 DMA 地址映射，释放页表；
5. **安全拒绝交接**：使用 DMA 的设备（USB、UFS、显示）在任何一步无法确认已干净停止时，固件拒绝交接并保持当前状态，严禁强行释放可能仍在被硬件 DMA 访问的物理内存。

## 常驻 USB Fastboot 服务

固件运行期间在后台常驻 USB Fastboot 服务（序列号固定为 `SunUEFI-piano`），在固件菜单、设置和 Shell 界面中均保持在线。它支持以下调试操作：

* `getvar product` / `version` / `storage-policy`：查询固件状态；
* `oem status` / `oem ramlog`：导出内存诊断日志（最多 256 KiB）；
* `oem screenshot`：读取帧缓冲截图（24 位 BMP）；
* `fetch`：只读读取分区内容（单次上限 64 KiB）；
* `oem boot-stable`：直接从 ESP 启动 Linux；
* `reboot` / `continue`：安全清理外设后重启回系统。

该服务明确拒绝 `flash`、`erase`、`flashing` 等改写分区的危险操作，详见 [UEFI Fastboot](../user/fastboot.md)。

## SimpleInit 图形菜单与启动画面

固件整合了定制的 SimpleInit 图形菜单，针对 3200×2136 分辨率的平板屏幕进行四倍字体放大：

* **交互操作**：音量＋/－键移动焦点，电源键确认，同时按两侧音量键返回。其他输入的使用限制见[已知问题](../user/known-issues.md#uefi-菜单与-usb)，后台初始化和标准协议见 [UEFI 设置页面](boot/uefi-console.md#官方磁吸键盘与触控板)；
* **菜单选项**：默认选择“启动 Linux”；“从文件启动”可选择 EFI 应用，另有固件设置、UEFI Shell、文件管理器、工具、UEFI 启动项及电源操作；
* **进度显示**：启动画面实时显示底层硬件初始化的分阶段进度，避免长时间黑屏。

## ESP 启动文件与系统交接

启动 Linux 时，固件在唯一的 `sunuefi_esp` 分区上加载 `/EFI/BOOT/BOOTAA64.EFI`，并保留真实的 ESP 设备句柄。systemd-boot 读取 `loader.conf` 和启动项。标准 EFI 启动项分别用 `linux`、`initrd`、`devicetree` 指定带 EFI stub 的 ARM64 内核、初始文件系统和设备树，由 systemd-boot 直接装载。完整文件布局见[启动架构](architecture.md#存储布局)。

固件通过 `EFI_DT_FIXUP_PROTOCOL` 为装载的设备树应用同次启动的内存、面板与 Gunyah 修正，并检查保留区。Linux EFI stub 负责填写 EFI 内存图和 initrd 信息。SEC 从本次 ABL 传入的最终设备树读取完整 RAM，结合 SMEM、原生固件区域和冷启动对象生成内存表；新增普通 RAM 保持为 `EfiLoaderData`，不进入固件分配池。退出前再次核对真实 EFI/GCD 描述符及页表的恒等映射和缓存属性，再交给 Linux 回收。

`PianoProductMemoryApplyDtb()` 使用带 CRC 与同次启动标识的早期采集结果，替换 Linux DTB 的 `/memory/reg`、DDR 容量／厂商／类型和 `/ddr-regions`，移除多余的 memory 节点。标准 EFI 和旧 `boot.img` 共用此函数，不按内存容量制作镜像，也不在安装时改写 ESP。Linux 的硬件节点、CMA 请求与保留区保持原样；交接前的检查会拒绝不兼容的保留区，而不是把 EFI 空闲页误当作物理总内存。

现有 Debian 镜像仍可使用 `/EFI/Piano/LinuxBoot.efi`。该应用通过 Runtime 协议的 APP 入口同步调用驻留核心。核心确认调用者是当前 systemd-boot 的真实子应用后，沿用 `PianoRawLinuxBoot.c` 的文件加载和内核交接：读取 `/EFI/Piano/stable/boot.img`，复制内核、设备树和 initramfs，保留运行时面板与 Gunyah 修正，再关闭 USB、存储、输入、时钟和 SMMU。

已有 ESP 仅包含 `boot.img` 时，固件在未找到且从未执行 `BOOTAA64.EFI` 的情况下直接使用旧加载器，用户无需重装系统。EFI 引导器已经运行后发生的错误不会静默切换加载路径。准备内核、核对交接身份和退出固件服务的失败分别记录阶段；内存采集与设备树保留区错误显示对应原因及回到 Android 的操作。

ESP 中的 Linux `boot.img` 总容器上限为 128 MiB，打包入口 `./build.sh package` 和 `./build.sh esp` 会核对该上限。initramfs 的压缩后上限仍为 16 MiB，内核和设备树仍须满足各自的交接窗口。UEFI Fastboot 的单次 USB 下载上限保持 64 MiB，超过此大小的 Linux 容器需从 ESP 启动。

两种路径的 `ExitBootServices` 都使用当前最内层启动应用的真实句柄和已保存的加载身份。标准 EFI 路径还检查该应用到当前 systemd-boot 的父链及 ESP 身份。开始关闭资源前的拒绝允许启动管理器重试其他内核；进入关闭流程后，除内存图过期外的错误都会停止执行。首次调用通过身份与内存检查后即关闭资源，即使传入的 MapKey 已过期，也不把关闭动作留到下一次调用。原生内存图检查仍拒绝过期的 key；Linux 刷新一次内存图即可重试，不再进行文件操作、分配或重复关闭硬件。

固件的四个 EFI 时间调用返回 `EFI_UNSUPPORTED`，并通过 `EFI_RT_PROPERTIES_TABLE` 报告；不发布占位 EFI RTC。Linux 使用 PMIC RTC，UEFI 只读原厂 `persist/time/ats_2`，通过保留内存中的标准 NVMEM 单元传入日历偏移，见 [RTC 与原厂时间](hardware/sleep.md#rtc-与原厂时间)。

## UEFI 核心代码结构

| 目录 | 职责 |
| --- | --- |
| `uefi/core/` | 产品核心逻辑：入口控制、硬件状态观察、只读块设备、USB 服务、按键处理与启动策略 |
| `uefi/components/os-boot/` | 操作系统启动文件解析、内存租用与启动会话控制 |
| `uefi/components/linux-loader/` | Linux 内核与设备树装载器 |
| `uefi/components/product-handoff/` | 固件交接、晚期内存验证与退出钩子 |
| `uefi/components/pogo-product/` | 官方磁吸键盘与触控板：QUP1 SE6 I²C、MCU 固件下载、标准输入协议与退出前释放 |
| `uefi/components/touch-product/` | 触屏：QUP1 SE2 SPI、触控固件下载、原始矩阵到触点、`AbsolutePointer` 与退出前释放 |
| `uefi/components/product-support/` | GOP 显示适配、内存日志打印与 BDS 支持库 |
| `uefi/components/usb-host/` | USB 主机模式：Type-C 状态读取、xHCI 与 DMA |
| `uefi/components/simple-init/` | SimpleInit 菜单与图形渲染组件 |
| `uefi/handoff/bootselect/` | 合体 BOOT 开机前置选择器与按键捕获 |
| `uefi/handoff/bootshim/` | 引导垫片与早期内存环境搭建 |
| `uefi/handoff/early-memory/` | 冷启动对象扫描与早期内存描述符解析 |
| `uefi/platforms/pianoPkg/` | 基础平台描述与组件基准引用 |
| `uefi/platforms/pianoProbePkg/` | 诊断平台与产品固件组装模板 |
