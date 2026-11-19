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

驻留产品核心校验原厂传入的设备树，接管显示，启动后台 USB 服务，发布 UFS
块设备并恢复保存的设置。然后进入固件启动顺序、一次性启动选择或固件设置。
启动操作系统和整个固件重启都回到驻留核心，由它完成对应的资源退出。

核心从同一次启动载荷检查取得设备树和启动目标。临时启动只在设备树声明且
完整映射的 initrd 范围内查找 SimpleInit；合体 BOOT 使用选择器传来的载荷范围。
两条路径都核对固件内置的应用摘要和 ARM64 EFI 格式。真正加载 SimpleInit 前
仍独立复查交接记录和应用，不沿用初始化时的验证结果。

## UFS 写入

标准 Block I/O 以只读方式发布原厂 UFS。固件变量文件的写入范围见
[变量持久化](boot/uefi-console.md#persist-中的-uefi-变量)。

Fastboot 只对刷写 `sunuefi_esp` 和 `sunuefi_root` 启用写入。写前按 GPT 名称
定位并复核介质标识、GUID 和范围，再卸载目标文件系统。每段写入后读回比较，
最后同步设备缓存。Android sparse 镜像先检查整个结构，空洞保留原有数据。

GPT 检查结构、CRC、分区重叠和逻辑边界。查询、扫描和启动探测只读取磁盘。

## 安全退出 ExitBootServices

在启动 Linux 前，UEFI 必须安全断开硬件并退出 ExitBootServices，防止发生外设总线冲突：

1. 先关闭触屏的 GPI 通道和事件环，确认串行引擎停止，再释放 DMA 缓冲区及 SMMU 域，恢复寄存器、引脚和时钟。它占用前的无效 SMMU 路由也必须恢复，以免改变 USB／UFS 的退出校验。
2. 停止 DWC3 端点传输，清空事件环，释放 USB DMA 缓冲区；USB 口处于主机模式时，先断开 USB 设备驱动，停止并复位 xHCI。
3. 完成 UFS 已排队的传输，同步缓存，安全断开存储通道。USB 和 UFS 各自在停止 DMA 后解绑地址映射并释放页表。
4. 关闭物理按键和磁吸键盘输入协议，让键盘 MCU 回到复位，恢复 L11B/L14B 的 RPMh 投票、引脚和 SE6 寄存器，释放 QUP1 时钟引用，最后释放显示时钟。

任何一步无法确认外设已经停止时，固件保留可能仍被 DMA 使用的内存，拒绝交接系统。整机重启不经过这条路径：保存设置、完成存储写入并停止存储队列后直接调用 `ResetSystem`，外设 DMA 缓冲区保留到复位。具体实现见[触屏实现](boot/uefi-console.md#触屏)与[电源动作](boot/uefi-console.md#电源动作与退出)。

DWC3 停机时先结束活动端点传输，再清除 `RUN_STOP`。等待 `DEVCTRLHLT` 时持续
确认事件缓冲区中的事件，不再处理新请求或提交 TRB；USB2 PHY 在整个停机过程中
保持唤醒。只有收到控制器停止确认后，才归还 DMA 缓冲区并释放 SMMU 与时钟。

## 常驻 USB Fastboot 服务

固件运行期间在后台常驻 USB Fastboot 服务（序列号固定为 `SunUEFI-piano`），在固件菜单、设置和 Shell 界面中均保持在线。它支持以下调试操作：

* `oem help`：列出当前模式可用的命令；
* `getvar product` / `version` / `storage-policy`：查询固件状态；
* `oem status` / `oem ramlog`：导出内存诊断日志（最多 256 KiB）；
* `oem screenshot`：读取帧缓冲截图（24 位 BMP）；
* `fetch`：按后端提供的上限只读读取分区内容（产品每次最多 256 KiB）；
* `flash` / `erase`：按准确的 GPT 名称写入或清零指定分区；
* `oem boot-stable`：按固件的启动顺序尝试已安装的系统；
* `reboot` / `continue`：保存设置、同步存储后执行整机复位；不经过系统交接检查。

刷写前只卸载目标文件系统。后续每段写入都会重新确认目标未被挂载、介质与分区未变，写完读回校验，最后同步才报告成功。产品 UFS 每条读写命令最多传输 1 MiB，分成至多四个 256 KiB 的 DMA 段；Fastboot 单次 `fetch` 仍最多返回 256 KiB。

产品的 USB 批量数据用 64 KiB DMA 缓冲，命令仍只占一个 USB 包。USB 事件优先处理，键盘和触屏在批次间继续读取；下载、刷写或擦除时，SimpleInit 的自动启动倒计时暂停。电脑中止读取时，固件等待旧回复结束，确认端点停止后才接受新命令。命令用法见 [UEFI Fastboot](../user/fastboot.md)。

Type-C 重插后，固件确认新方向，等当前命令结束并停稳原连接的 DMA，
再切换 USB3 通道。高速通道初始化失败时会尝试恢复原设置，保留 USB2 连接。

错误提示页仅在 BootServices、USB 设备及其 SMMU 资源仍有效时运行诊断循环。
循环绕过启动策略、输入和 USB 角色切换，只处理现有 USB 设备；输入退出失败
不应停止仍健康的 USB。Fastboot 保留状态、日志、截图和快照上传，重启是否
允许取决于设置保存与存储写入状态，外设寄存器恢复失败不阻止整机复位。具体重启、清理失败和连接
保留条件见[电源动作与退出](boot/uefi-console.md#电源动作与退出)。状态查询只
输出保存的镜像、设备树、内存和退出检查结果，不重新探测失败的硬件。

## SimpleInit 图形菜单与启动画面

固件整合了定制的 SimpleInit 图形菜单，针对 3200×2136 分辨率的平板屏幕进行四倍字体放大：

* **交互操作**：音量＋/－键移动焦点，电源键确认，同时按两侧音量键返回。其他输入的使用限制见[已知问题](../user/known-issues.md#uefi-菜单与-usb)，后台初始化和标准协议见 [UEFI 设置页面](boot/uefi-console.md#官方磁吸键盘与触控板)；
* **菜单选项**：默认选择“启动 Linux”；“从文件启动”可选择 EFI 应用，另有固件设置、UEFI Shell、文件管理器、工具、UEFI 启动项及电源操作；
* **进度显示**：启动画面实时显示底层硬件初始化的分阶段进度，避免长时间黑屏。

## ESP 启动文件与系统交接

控制台初始化后先启动驻留核心，完成设备初始化、变量恢复和安全退出协议安装，
再选择系统。固件读取标准 `Boot####`：先消费一次性 `BootNext`，
再按 `BootOrder` 尝试启用的启动项。SimpleInit 也是普通启动项，通过固定的固件卷
GUID 定位，没有用户排序时排在首位；已有排序不会因固件启动而重排。
启动时按 Esc 打开本次启动选择，选定或取消都不改 `BootOrder`，F12 进入固件设置。
设备路径由上游 UEFI BootManager 库展开，
支持 GPT 短路径、文件路径和标准 `EFI/BOOT/BOOTAA64.EFI` 后备入口。
固件不限定分区标签、UFS LUN 或 systemd-boot 文件名；不自动创建名为 Linux 的
固件内部替代项。

产品 ESP 默认安装 systemd-boot。没有注册普通启动项时使用标准后备文件。
systemd-boot 默认使用分开的内核、initramfs 和设备树，也能发现版本化 UKI。
SimpleInit 默认选择“启动 Linux”，无操作五秒后按启动顺序寻找系统，跳过 SimpleInit 自身；
随后 systemd-boot 菜单停留三秒，可选择旧内核。
自动启动找不到可用项时返回菜单。系统引导程序返回错误后，只有应用已卸载、
固件服务仍可用且没有新的退出请求，才继续尝试顺序中的下一项。
手动指定的启动项失败，或尝试完仍无法启动时，显示错误并保留故障恢复入口。
完整文件布局见[启动架构](architecture.md#存储布局)。

固件通过 `EFI_DT_FIXUP_PROTOCOL` 为装载的设备树应用同次启动的内存、面板与 Gunyah 修正，并检查保留区。Linux EFI stub 负责填写 EFI 内存图和 initrd 信息。SEC 从本次 ABL 传入的最终设备树读取完整 RAM，结合 SMEM、原生固件区域和冷启动对象生成内存表；新增普通 RAM 保持为 `EfiLoaderData`，不进入固件分配池。退出前再次核对真实 EFI/GCD 描述符及页表的恒等映射和缓存属性，再交给 Linux 回收。

`PianoProductMemoryApplyDtb()` 使用带 CRC 与同次启动标识的早期采集结果，替换 Linux DTB 的 `/memory/reg`、DDR 容量／厂商／类型和 `/ddr-regions`，移除多余的 memory 节点。DTB 在内存中经标准协议修正，不按内存容量制作镜像，也不在安装时改写 ESP。Linux 的硬件节点、CMA 请求与保留区保持原样；交接前的检查会拒绝不兼容的保留区，而不是把 EFI 空闲页误当作物理总内存。

启动镜像由构建器生成，写入 FAT 后读回核对。选择 UKI 时，用 `ukify` 打包
并核对 `.linux`、`.initrd`、`.dtb` 和 `.cmdline` 各段。内核没有内建命令行或
后备值。安装后的内核更新由发行版软件包维护，不需要重新刷写整个 ESP。

`ExitBootServices` 使用当前最内层启动应用的句柄，并核对它到所选 EFI
应用的父链、外层应用的加载身份和来源卷。父链最多 32 层，子加载器可以打开
其他卷，但不能借用另一条启动链的镜像身份。systemd-stub 直接调用内核时
会复用 UKI 句柄，外层 systemd-boot 的身份保持不变。
父句柄和 ESP 来源保持不变，不接受任意替换地址。开始关闭资源前的拒绝可以正常返回；
进入关闭流程后，除内存图过期外的错误都会停止执行。首次调用通过身份与
内存检查后即关闭资源，即使传入的 MapKey 已过期，也不把关闭动作留到下一次
调用。Linux 刷新内存图后重试，不再分配、访问文件或重复关闭硬件。

`PianoRawLinuxBoot.c` 仅保留已确认 USB 下载的诊断启动，不读取 ESP，也不作为
UKI 失败后的后备。它与产品 UKI 共用资源退出检查；UEFI Fastboot 的单次
下载上限仍为 64 MiB。

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
