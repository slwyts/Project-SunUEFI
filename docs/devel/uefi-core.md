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

UFS 整盘和原厂分区的标准 Block I/O 保持只读。独立的 `sunuefi_esp` 在原有 GPT
分区句柄上提供写入，EFI 程序能用标准文件接口保存启动状态，systemd-boot 也能
更新启动计数。写入使用本次 GPT 的实际范围；FAT 已标记为不干净时，只读挂载。
EFI 程序应在交接前刷新或关闭写入的文件。固件会在存储控制器停机前同步已交给
块设备的写入，包括卸载文件系统时产生的写入；只读启动不会为此发送刷盘命令。
固件变量文件另见[变量持久化](boot/uefi-console.md#persist-中的-uefi-变量)。

Fastboot 只对刷写 `sunuefi_esp` 和 `sunuefi_root` 启用写入。写前按 GPT 名称
定位并复核介质标识、GUID 和范围，再卸载目标文件系统。每段写入后读回比较，
最后同步设备缓存。Android sparse 镜像先检查整个结构，空洞保留原有数据。

GPT 检查结构、CRC、分区重叠和逻辑边界。查询、扫描和启动探测只读取磁盘。

## 安全退出 ExitBootServices

启动 Linux、软件重启或关机前，驻留核心按同一顺序关闭硬件，再退出 Boot Services。

1. 先关闭触屏的 GPI 通道和事件环，确认串行引擎停止，再释放 DMA 缓冲区及 SMMU 域，恢复寄存器、引脚和时钟。它占用前的无效 SMMU 路由也必须恢复，以免改变 USB／UFS 的退出校验。
2. 停止 DWC3 端点传输，清空事件环，释放 USB DMA 缓冲区；USB 口处于主机模式时，先断开 USB 设备驱动，停止并复位 xHCI。
3. 完成 UFS 已排队的传输，同步缓存，安全断开存储通道。USB 和 UFS 各自在停止 DMA 后解绑地址映射并释放页表。
4. 关闭物理按键和磁吸键盘输入协议，让键盘 MCU 回到复位，恢复 L11B/L14B 的 RPMh 投票、引脚和 SE6 寄存器，释放 QUP1 时钟引用，最后释放显示时钟。

任何一步无法确认外设已经停止时，固件保留可能仍被 DMA 使用的内存，拒绝继续。整机重启和关机在 `ExitBootServices` 成功后调用 `ResetSystem`。具体实现见[触屏实现](boot/uefi-console.md#触屏)与[电源动作](boot/uefi-console.md#电源动作与退出)。

普通重启还会在退出后检查 Gunyah 看门狗。如果设备树指定通过 SMC 调用固件，且虚拟机管理程序的标识和计时器都匹配，
固件先设置八秒超时，再立即调用 PSCI 重启；即使 PSCI 没有响应，计时也不依赖 UEFI 的中断。
不支持该接口时仍走 PSCI。关机、进入 Recovery、Fastboot 或 EDL，以及启动操作系统，都不设置这个后备计时器。

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
* `reboot` / `continue`：保存设置，同步存储，完成安全退出后重启。

刷写前只卸载目标文件系统。后续每段写入都会重新确认目标未被挂载、介质与分区未变，写完读回校验，最后同步才报告成功。产品 UFS 每条读写命令最多传输 1 MiB，分成至多四个 256 KiB 的 DMA 段；Fastboot 单次 `fetch` 仍最多返回 256 KiB。

产品的 USB 批量数据用 64 KiB DMA 缓冲，命令仍只占一个 USB 包。USB 事件优先处理，键盘和触屏在批次间继续读取；下载、刷写或擦除时，SimpleInit 的自动启动倒计时暂停。电脑中止读取时，固件等待旧回复结束，确认端点停止后才接受新命令。命令用法见 [UEFI Fastboot](../user/fastboot.md)。

Type-C 重插后，固件确认新方向，等当前命令结束并停稳原连接的 DMA，
再切换 USB3 通道。高速通道初始化失败时会尝试恢复原设置，保留 USB2 连接。

错误提示页仅在 BootServices、USB 设备及其 SMMU 资源仍有效时运行诊断循环。
循环绕过启动策略、输入和 USB 角色切换，只处理现有 USB 设备；输入退出失败
仍可保留健康的 USB 会话。Fastboot 提供状态、日志、截图和快照上传。固件确认
资源仍可安全退出时才接受重启。具体重启、清理失败和连接
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
没有匹配的文件系统时，调用同一路径的标准 `EFI_LOAD_FILE_PROTOCOL`。
从 SimpleInit 选择 EFI 文件（包括自动发现的 Windows Boot Manager）时，菜单
交回所选设备路径及启动参数，由核心按同样的方式装载文件。选文件只对本次
启动有效，不创建 `Boot####`，不消费 `BootNext`，也不改写 `BootOrder`。
运行的 EFI 工具正常返回后会重新显示菜单。
电脑也可通过 [UEFI Fastboot](../user/fastboot.md#启动与退出) 提交一次性文件路径。
固件发送完 USB 回执后，交给同一套 EFI 加载和设备退出流程，不需要改 ESP 上的菜单配置。
固件不限定分区标签、UFS LUN 或 systemd-boot 文件名；不自动创建名为 Linux 的
固件内部替代项。

产品 ESP 默认安装 systemd-boot。没有注册普通启动项时使用标准后备文件。
systemd-boot 默认使用分开的内核、initramfs 和设备树，也能发现版本化 UKI。
SimpleInit 默认选择“启动 Linux”，无操作五秒后按启动顺序寻找系统，跳过 SimpleInit 自身；
随后 systemd-boot 菜单停留三秒，可选择旧内核。
自动启动找不到可用项时返回菜单。系统引导程序返回错误后，只有应用已卸载、
固件服务仍可用且没有新的退出请求，才继续尝试顺序中的下一项。
手动指定的启动项失败，或尝试完仍无法启动时，只要引导程序已退出、硬件仍由固件管理，
错误页就保留完整的 Fastboot。可从电脑选择另一个 EFI 文件或重启，也可按音量键回菜单；
错误页不会自行倒计时重试。
运行系统引导程序前开启五分钟的 UEFI 软件看门狗。超时时，如果存储和 USB 仍由固件完整管理，
下一次等待 EFI 事件会收到中止通知，期间 Fastboot 仍能处理命令。引导程序返回后，固件卸载它，
再尝试下一个启动项或显示错误。已超时的程序不能继续接管硬件。
已超时的程序若仍占用 CPU，但中断和原有 Fastboot 连接都在，电脑仍能用 `getvar product`
或 `oem status` 确认固件是否仍控制 USB、设备交接是否已开始；`reboot` 可先接受请求，
等该程序交还控制权后再安全重启。日志导出、刷写等操作要等它退出。
如果程序不再返回或驱动已经开始退出，可以长按电源重启，再按音量上进入 Android。
这只处理退出 UEFI 之前的超时；操作系统接管后的死机不由此计时器处理。
完整文件布局见[启动架构](architecture.md#存储布局)。

固件通过 `EFI_DT_FIXUP_PROTOCOL` 为装载的设备树应用同次启动的内存、面板与 Gunyah 修正。Linux EFI stub 填写 EFI 内存图和 initrd 信息。每次启动都从 ABL 传入的最终设备树读取实际 RAM：原厂固件、启动时仍占用的数据、固定保留区和 CMA 不进入通用分配池。完成底层设备初始化后，设备树与 SMEM 都确认为可用的空闲页才交给标准 EFI 分配器，供 Linux 或 Windows 加载内核和启动映像。仅由设备树报告的扩容内存仍在 EFI 内存图中描述，固件不提前分配。设备树要求的新保留区不能覆盖正在使用的页；退出前复核实际 EFI/GCD 内存描述符、页表映射及缓存属性。

`PianoProductMemoryApplyDtb()` 使用带 CRC 与同次启动标识的早期采集结果，替换 Linux DTB 的 `/memory/reg`、DDR 容量／厂商／类型和 `/ddr-regions`，移除多余的 memory 节点。DTB 在内存中经标准协议修正，不按内存容量制作镜像，也不在安装时改写 ESP。Linux 的硬件节点、CMA 请求与保留区保持原样；交接前的检查会拒绝不兼容的保留区，而不是把 EFI 空闲页误当作物理总内存。

Windows 和 Linux 还可从 SMBIOS 读取板载内存。类型 16、17 描述本次启动识别的容量，
类型 19 分段描述物理地址，不把中间的空洞算作 RAM。仅改大容量字段不算扩容；
如果本次 ABL 已经报告旧 DDR 表以外的 RAM，则保留新增地址段。原厂记录不一致时，
不发布超过已确认容量的控制器地址表，也不把可分配页向上取整当作装机容量。
在 Linux 用 `sudo dmidecode -t 16,17,19` 查看；系统实际可用容量会扣除硬件保留区。

同一份 SMBIOS 将设备标为小米 Pad 8 Pro、主板 `piano`，机型为平板。类型 0 描述正在运行的
UEFI，不是原厂 XBL。类型 1 只在 ABL 的设备树提供标准 `serial-number` 时填写序列号；
没有原厂系统 UUID 或零售 SKU 时留空，不从 SoC 编号、内存容量或地区型号推算。

启动镜像由构建器生成，写入 FAT 后读回核对。选择 UKI 时，用 `ukify` 打包
并核对 `.linux`、`.initrd`、`.dtb` 和 `.cmdline` 各段。内核没有内建命令行或
后备值。安装后的内核更新由发行版软件包维护，不需要重新刷写整个 ESP。

`ExitBootServices` 使用当前最内层启动应用的句柄，并核对它到所选 EFI
应用的父链、外层应用的加载身份和来源设备。父链最多 32 层，子加载器可以打开
其他卷，但不能借用另一条启动链的镜像身份。systemd-stub 直接调用内核时
会复用 UKI 句柄，外层 systemd-boot 的身份保持不变。
引导程序安装了设备树时，固件检查这棵树与本次启动的 RAM 和保留区是否一致。
未安装设备树时，使用标准 ACPI 2.0 配置表，检查 RSDP 和 XSDT 是否位于可读
内存中，长度和校验和是否正确。损坏的设备树不会因为另有 ACPI 表而被接受。
两种方式共用 EFI 内存图检查和 USB、UFS 的退出流程，不按引导文件名识别系统。
启动服务期间，EFI 加载器可通过标准 `EFI_MEMORY_ATTRIBUTE_PROTOCOL` 查询、设置和清除
已分配页的访问权限。ArmCpuDxe 读取和更新实际页表；例如，给一页数据临时设置只读，
再清除只读位，不会清除它原有的禁止执行属性。
SM8750 没有启用 Mu 的处理器勘误补丁。启动 Windows 时，固件不会为这些补丁
替换页分配和退出启动服务的函数，也不修改微软加载器的代码或内存保护属性。
外层应用的父句柄和来源设备在整个启动过程中保持不变。开始关闭资源前的拒绝可以正常返回；
进入关闭流程后，除内存图过期外的错误都会停止执行。首次调用通过身份与
内存检查后即关闭资源，即使传入的 MapKey 已过期，也不把关闭动作留到下一次
调用。Linux 刷新内存图后重试，不再分配、访问文件或重复关闭硬件。

`PianoRawLinuxBoot.c` 仅保留已确认 USB 下载的诊断启动，不读取 ESP，也不作为
UKI 失败后的后备。它与产品 UKI 共用资源退出检查；UEFI Fastboot 的单次
下载上限仍为 64 MiB。

系统引导程序可通过标准 `EFI_RNG_PROTOCOL` 取得启动所需的随机数据。固件使用
EDK2 的 RNG 驱动：默认取自 CPU 的 RNDR，也支持显式请求 Arm 固件的原始随机源。

在操作系统接管前，标准 `GetTime()` 读取 PMK8550 的秒计数，加上原厂 `persist/time/ats_2`
的偏移，返回 UTC。它不写时钟或闹钟；退出启动服务后，SPMI 由操作系统管理，固件不再
提供时间服务，并在 `EFI_RT_PROPERTIES_TABLE` 中如实声明。Linux 继续通过 PMIC RTC 和
NVMEM 读取相同的时间，见 [RTC 与原厂时间](hardware/sleep.md#rtc-与原厂时间)。

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
