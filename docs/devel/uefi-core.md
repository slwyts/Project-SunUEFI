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

1. **USB 控制器断开**：停止 DWC3 端点传输，清空事件环，释放 USB DMA 缓冲区；
2. **UFS 存储安全退出**：完成已排队的传输，同步缓存，安全断开 UFS 控制器通道；
3. **时钟与电源释放**：释放显示时钟、SE6 串行引擎等临时占用的外设时钟资源；
4. **SMMU 保护解除**：安全解绑 DMA 地址映射，释放页表；
5. **安全拒绝交接**：若在任何一步无法确认设备已干净停止，固件将拒绝交接并保持当前状态，严禁强行释放可能仍在被硬件 DMA 访问的物理内存。

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

* **交互操作**：支持实体按键操作，音量＋/－键移动焦点，电源键确认，双击音量键返回；
* **菜单选项**：启动 Linux、进入 Android、固件设置（Setup）、命令行（Shell）；
* **进度显示**：启动画面实时显示底层硬件初始化的分阶段进度，避免长时间黑屏。

## ESP 启动文件与系统交接

启动 Linux 时，固件在唯一的 `sunuefi_esp` 分区上加载 `/EFI/BOOT/BOOTAA64.EFI`，并保留真实的 ESP 设备句柄。systemd-boot 读取 `loader.conf` 和启动项。标准 EFI 启动项分别用 `linux`、`initrd`、`devicetree` 指定带 EFI stub 的 ARM64 内核、初始文件系统和设备树，由 systemd-boot 直接装载。完整文件布局见[启动架构](architecture.md#存储布局)。

固件通过 `EFI_DT_FIXUP_PROTOCOL` 为装载的设备树应用同次启动的面板与 Gunyah 修正，并检查保留区。Linux EFI stub 负责填写 EFI 内存图和 initrd 信息。SEC 从本次 ABL 传入的 Factory DT 读取完整 RAM，结合 SMEM、原生固件区域和冷启动对象生成内存表；新增普通 RAM 保持为 `EfiLoaderData`，不进入固件分配池。退出前再次核对真实 EFI/GCD 描述符及页表的恒等映射和缓存属性，再交给 Linux 回收。

现有 Debian 镜像仍可使用 `/EFI/Piano/LinuxBoot.efi`。该应用通过 Runtime 协议的 APP 入口同步调用驻留核心。核心确认调用者是当前 systemd-boot 的真实子应用后，沿用 `PianoRawLinuxBoot.c` 的文件加载和内核交接：读取 `/EFI/Piano/stable/boot.img`，复制内核、设备树和 initramfs，保留运行时面板与 Gunyah 修正，再关闭 USB、存储、输入、时钟和 SMMU。

ESP 中的 Linux `boot.img` 总容器上限为 128 MiB，打包入口 `./build.sh package` 和 `./build.sh esp` 会核对该上限。initramfs 的压缩后上限仍为 16 MiB，内核和设备树仍须满足各自的交接窗口。UEFI Fastboot 的单次 USB 下载上限保持 64 MiB，超过此大小的 Linux 容器需从 ESP 启动。

两种路径的 `ExitBootServices` 都使用当前最内层启动应用的真实句柄和已保存的加载身份。标准 EFI 路径还检查该应用到当前 systemd-boot 的父链及 ESP 身份。开始关闭资源前的拒绝允许启动管理器重试其他内核；进入关闭流程后出错便停止，不能返回仍可能持有文件句柄的 systemd-boot。MapKey 变化后的重试不再进行文件操作、分配或重复关闭硬件。

## UEFI 核心代码结构

| 目录 | 职责 |
| --- | --- |
| `uefi/core/` | 产品核心逻辑：入口控制、硬件状态观察、只读块设备、USB 服务、按键处理与启动策略 |
| `uefi/components/os-boot/` | 操作系统启动文件解析、内存租用与启动会话控制 |
| `uefi/components/linux-loader/` | Linux 内核与设备树装载器 |
| `uefi/components/product-handoff/` | 固件交接、晚期内存验证与退出钩子 |
| `uefi/components/product-support/` | GOP 显示适配、内存日志打印与 BDS 支持库 |
| `uefi/components/simple-init/` | SimpleInit 菜单与图形渲染组件 |
| `uefi/handoff/bootselect/` | 合体 BOOT 开机前置选择器与按键捕获 |
| `uefi/handoff/bootshim/` | 引导垫片与早期内存环境搭建 |
| `uefi/handoff/early-memory/` | 冷启动对象扫描与早期内存描述符解析 |
| `uefi/platforms/pianoPkg/` | 基础平台描述与组件基准引用 |
| `uefi/platforms/pianoProbePkg/` | 诊断平台与产品固件组装模板 |
