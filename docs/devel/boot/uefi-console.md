# UEFI 设置页面

SimpleInit 提供触摸尺寸的图形菜单；TianoCore 设置页面使用 UEFI 文本控制台。
在 3200×2136 屏幕上，默认 8×19 字形的 80×25 页面只占很小一块区域。

产品构建只为 GraphicsConsole 的字符绘制增加四倍缩放。该驱动内部使用
800×534 的逻辑画布，把文本块放大到物理屏幕；公开 GOP 的模式和帧缓冲仍是
3200×2136，SimpleInit、截图和操作系统使用原来的接口，不切换物理显示模式。
80×25 字符区放大后约为 2560×1900。

构建工作副本中的五组 HII 字符串加入 `zh-CN`，常用设置页面默认中文，
技术设备名和未翻译的文字仍用英文。中文来自 Noto Sans CJK SC 的 16×19
宽字符位图，通过标准 HII 字体包注册；构建记录源字体和生成文件摘要。
没有修改固定的上游检出目录。

实体按键通过标准 `SimpleTextIn` 协议接入。按键设备同时安装
`ConsoleInDevice` 标记，让标准控制台汇聚驱动能够找到它。进入设置页时，
音量键映射上下移动，短按电源映射回车，同时按音量两键映射返回。

源码位置：

* `uefi/components/product-support/Library/ConsoleScale/`：缩放和中文翻译。
* `tools/prepare_product_console.py`、`tools/prepare_product_ui.py`：在构建副本生成字形并接入 GraphicsConsole。
* `tools/prepare_gui_profile.py`：默认语言。
* `uefi/core/PianoKeys.c`：标准按键设备与映射。

合体 BOOT 的实机检查确认四倍字号、常用菜单中文、音量键移动、电源键确认
与双音量键返回均可使用。菜单、设置和 Shell 切换时保留同一个 Fastboot 服务。
Shell 首次连接曾发生 bulk IN 响应停滞，重新连接行为仍需修正。
磁吸键盘和触控板、触屏见后面两节。

## 官方磁吸键盘与触控板

固件在后台初始化键盘套里的 Nanosic WN8030 微控制器，进入菜单后大约
1.5 秒接入，这段时间和之后音量键、电源键照常可用。键盘以标准
`SimpleTextIn` 和 `SimpleTextInEx` 发布，并带 `ConsoleInDevice` 标记，
SimpleInit、固件设置和 Shell 都通过标准控制台读到它。顶排没有功能键，
右 Ctrl 加 `1` 到 `0`、`-`、`=` 输出 F1 到 F12，与 Linux 相同，因此
右 Ctrl + `=` 是 F12，可以进入固件设置。Caps Lock 灯跟随锁定状态，
按键不自动连发。实现已编译进产品镜像，尚未在平板上验证。

触控板以标准 `SimplePointer` 发布：单指移动光标，轻点一下是左键，
按下触控板是左键或右键。它不作为绝对坐标设备发布，因为菜单会把绝对
坐标当成触屏点按。SimpleInit 按“移动量 × `gui.mouse_scale` ÷ 分辨率”
换算光标，触控板报告的分辨率为 1，一个计数移动一个像素，横向划过整块
触控板约等于屏幕宽度；纵向计数按物理尺寸换算成与横向相同的比例。

初始化顺序与 Linux 7.3 的 `hid-nanosic-wn8030` 驱动一致：打开 QUP1
wrapper 与 SE6 的时钟，GPIO56/57 复用到 SE6；GPIO188 复位和 GPIO3 睡眠
（均低有效）先保持有效，经 RPMh 先打开 L11B 1.8 V，再打开 L14B 3.3 V；
释放复位，等待 500 ms 让 bootrom 启动，释放睡眠，确认 bootrom ID 为
`0xC8`、状态为 0，下载 RAM 固件（头部和每包 503 字节的代码，每次写入后
等待 3 ms），校验状态为 3 后启动固件，然后在 GPIO97 的上升沿读取 68 字节
报告。键盘接上时发送打开触控板和键盘供电的命令，此后每 12 秒重发键盘
供电请求。整个过程是由共享 pump 推进的状态机，只在等待事件和菜单刷新时
运行，每次最多执行一次 I²C 传输，不会阻塞菜单、设置、Shell 或 Fastboot。

传输使用 SE6 的 GENI I²C FIFO 接口，1 MHz，由 CPU 轮询，不使用 DMA，
所以不新增 SMMU 上下文。SE6 必须已经由引导程序载入 I²C 协议固件并开放
FIFO 接口；如果它只开放 GSI 接口，固件只读取状态、不改动它，键盘在 UEFI
中不可用，Linux 照常使用。MCU 的 RAM 固件来自固定版本的
`upstream/piano-firmware-current` 中的 `nanosic/MCU_Upgrade.bin`，也就是
Linux 根系统使用的同一文件；`tools/prepare_product.py` 按该仓库的
`firmware/SHA256SUMS` 核对后生成头文件嵌入固件，生成文件不提交。

任何一步失败，固件立即释放已经占用的资源，不发布输入设备，只保留音量键
和电源键。RAM 日志中的 `PIANO_POGO_UNAVAILABLE` 写明失败步骤和原因，
例如 `se6-gsi-only`、`boot-id`、`verify` 或 `transport`；`PIANO_POGO_SE6`
记录 SE6 的协议、FIFO 设置和时钟频率，`PIANO_POGO_READY` 和
`PIANO_POGO_COVER` 记录接入和键盘连接。没有键盘时 MCU 照常启动，只是没有
按键输入。

进入 Linux 或重启前，固件卸载输入协议，按 Linux 关机顺序让 MCU 进入睡眠
和复位，等待 10 ms，恢复 L14B、L11B 的原 RPMh 投票，恢复引脚和 GPIO97
的中断配置，恢复 SE6 寄存器，最后释放本固件持有的时钟引用。Linux 驱动
探测时会重新上电、复位并下载固件，看到的状态与其他启动方式相同。这条
路径不使用 DMA，也没有 SMMU 上下文，某一项没能恢复时不阻止启动：日志里的
`PIANO_POGO_RELEASE` 列出没有恢复的项目。输入协议、引脚、电源或时钟未能安全退出时拒绝交接，保留资源并显示恢复操作。

源码位置：

* `uefi/components/pogo-product/`：资源占用与释放、MCU 下载格式、启动状态机和输入协议。
* `uefi/core/PianoGeniI2cPio.c`：GENI I²C FIFO 传输，所有等待都有时限。
* `uefi/core/PianoPogoReport.c`、`uefi/core/PianoPogoInput.c`：报告解析、按键映射和触控板指针。

## 触屏

屏幕的 NT36532E 是触控与显示一体的芯片，没有内置触控固件。固件在后台
给它下载触控固件，完成后触屏以标准 `AbsolutePointer` 发布，坐标范围与 3200×2136 横屏画布相同，SimpleInit 直接用它点按。
只跟踪一个触点。SimpleInit 把每个新坐标都当成一次点按，所以每次触摸只在
抬起时报告一次，位置取按下点；移动超过一个传感器间距的触摸不算点按，
菜单里不能拖动或滚动，多指手势也不可用。实现已编译进产品镜像，尚未在平板上验证。

初始化严格按 Linux 7.3 `nt36xxx` 驱动在“没有可跟随的 DRM 面板”时的探测
顺序进行，这正是屏幕已由引导程序点亮时的情形：写 `0x7FFF80 = 0x5A`
做引擎复位，等 10 ms；依次用三组芯片 ID 寄存器读取并匹配 NT36532，每组
最多 5 次，每次先做 bootloader 复位；读 `ENB_CASC` 判断单芯片或级联；
然后执行 `nvt_download_firmware_hw_crc`：bootloader 复位，写 ILM/DLM 的
目标地址、长度和 CRC，打开自动复制，按 63 KiB 分块写入全部分区，等待
自动复制完成，打开固件 CRC 校验，置 `BOOT_RDY`，确认复位状态在
`0xA0`–`0xAF`，失败时整体重试，共 3 次；最后读取帧长度和固件信息。
与 Linux 驱动一样，不操作任何复位 GPIO、稳压器或面板命令。

触控固件按 GPIO100（触控 lcd-id）选择：0 用
`novatek/novatek_nt36532_piano_fw_csot.bin`，1 用
`novatek/novatek_nt36532_piano_fw_boe.bin`，与 Linux 设备树的
`firmware-name` 顺序相同。两份文件来自固定版本的
`upstream/piano-firmware-current`，`tools/prepare_product.py` 按
`firmware/SHA256SUMS` 核对后嵌入，`PIANO_TOUCH_FIRMWARE` 日志记录所选
文件的 SHA-256。

传输使用 QUP1 SE2 的 GENI SPI FIFO 接口，模式 0，最高 19.2 MHz，由 CPU
轮询，不使用 DMA。SE2 只开放 GSI 接口时，固件只读取状态，触屏在 UEFI
中不可用，日志写 `se2-gsi-only`。运行时在 GPIO162 的上升沿读取一帧原始
矩阵（最多每秒约 60 帧），算法移植自 `piano-touch-view`：锁定非笔的
主帧类型，取 32 帧中值作基线，去除行列共模噪声，在 3×3 邻域找超过阈值
200 的峰并求加权质心，再映射到屏幕坐标（Y 轴翻转）；静止时基线缓慢跟随
漂移。pump 只在等待事件和菜单刷新时推进触屏：下载期间每次写一个分块（19.2 MHz
下约 30 ms），之后每次最多读一帧。

失败时固件释放已占用的资源，不发布触屏，`PIANO_TOUCH_UNAVAILABLE` 写明
步骤和原因，例如 `se2-gsi-only`、`chip-id`、`firmware-start` 或
`transport`；`PIANO_TOUCH_SE2`、`PIANO_TOUCH_CHIP_ID`、
`PIANO_TOUCH_DOWNLOAD`、`PIANO_TOUCH_READY` 记录各阶段结果。连续出现看门狗
帧时按 Linux 的做法重新下载。

进入 Linux 或重启前，固件卸载触屏协议，恢复 SPI 与中断引脚、GPIO162 的
中断配置和 SE2 寄存器，释放时钟引用；不向芯片发送命令，Linux 驱动关机时
同样不发。Linux 探测时会再次复位并下载固件。这条路径不使用 DMA，某一项
没能恢复时在 `PIANO_TOUCH_RELEASE` 中列出原因并拒绝交接，不把未关闭的输入协议或时钟交给系统。

源码位置：

* `uefi/components/touch-product/PianoTouchService.c`：资源占用与释放、探测与下载状态机、`AbsolutePointer`。
* `uefi/components/touch-product/PianoGeniSpiPio.c`：GENI SPI FIFO 传输。
* `uefi/components/touch-product/PianoTouchNvt.c`：固件分区解析与芯片表。
* `uefi/components/touch-product/PianoTouchMatrix.c`：原始矩阵到触点。

## 启动项与横屏

SimpleInit 默认旋转改为 0°，使用 GOP 的 3200×2136 横屏画布。界面旋转由
SimpleInit/LVGL 处理，物理显示模式保持原样；实机已确认横屏显示。

标准 UEFI 启动管理器读取 `Boot####` 和 `BootOrder`。ESP 中的 `boot.img`
是 Android 格式的 Linux 启动包，不能直接当作 EFI 应用交给标准启动管理器。
产品现在先查找 ESP 和启动文件，再登记一个指向固件内
`PianoLinuxBoot.efi` 的 Linux 启动项。该应用向驻留核心请求启动，
随后进入 ESP 中的 systemd-boot。systemd-boot 的 Debian 条目加载 ESP 中
同一份 `LinuxBoot.efi`，通过 APP 接口同步进入核心的文件加载与安全交接，
不再排队或递归启动管理器。具体内存与退出身份见 [UEFI 核心](../uefi-core.md#esp-启动文件与系统交接)。

入口在启动时发现并登记，普通非易失变量通过下文的文件后端保存。
标准 EFI 启动项可以用 `linux`、`initrd`、`devicetree` 直接装载 Linux EFI stub。
固件提供完整内存描述、设备树修正与安全退出；Android 格式的启动包仍须使用
原有启动应用，不能仅改名为 `.efi`。两条路径共用同一份 UEFI 核心。

## 内置菜单与自动保存

[`config/simpleinit.static.uefi.cfg`](../../../config/simpleinit.static.uefi.cfg)
原样内置在 SimpleInit 的 rootfs 中，先作为静态配置加载，再加载
`persist/sunuefi/simpleinit.uefi.cfg` 中的用户设置。首次使用不需要复制
任何文件：目录由固件创建，修改后的设置由 SimpleInit 自动保存。
静态配置里的默认值不会整份复制到用户文件。

默认中文、横屏、DPI 600，开启动画，不自动倒计时。“启动 Linux”排在
第一项并默认选中；Windows 沿用扫描 `EFI/Microsoft/Boot/bootmgfw.efi`
的方式发现。菜单保留从文件启动、固件设置、UEFI Shell、文件管理器和
通用工具。菜单内的 Linux、设置、Shell 与重启动作交给同一个驻留核心处理，
不会为每个页面重新创建 USB 或存储服务。

菜单顺序使用 `boot.configs.<入口>.order` 整数。
`tools/prepare_simpleinit.py` 负责配置与保存路径，
`tools/prepare_product_ui.py` 负责动作分发和上游 EFI 文件扫描适配。
文件系统 URL 使用完整设备路径，查找对应 SimpleFileSystem，不把父磁盘
误当成分区文件系统。

## persist 中的 UEFI 变量

SimpleInit 设置与 UEFI 变量分开保存：

```text
persist/sunuefi/
├── simpleinit.uefi.cfg
├── nvram-a.bin
└── nvram-b.bin
```

变量文件包含普通非易失变量的名字、GUID、属性和数据。写入使用递增序号、
CRC32 和两个交替副本；刷新后独立读回比较，再使用新的序号。启动时选择
完整且序号最大的副本，通过标准 `SetVariable()` 恢复，不替换已经初始化
的变量缓存。系统启动交接前完成保存并禁止新的非易失变量修改。退出 Boot
Services 后不再访问 UFS，NV 写入返回不支持；带认证属性的 Secure Boot
变量不通过这个普通文件后端持久化。

ext4 驱动使用固定 lwext4 源码提供标准 SimpleFileSystem，只允许
`persist/sunuefi/` 中的文件写入，其他路径只读。父磁盘仍由
`PianoReadOnlyBlock` 保持代码级只读，私有写入只接受 GPT 分区范围。
文件系统先停止日志、卸载并同步，再停止 USB、UFS、SMMU 和时钟。
存在未关闭文件或无法确认 DMA 已停止时拒绝系统启动交接，不强行释放。

lwext4 的固定来源在 [`config/uefi-ext4.json`](../../../config/uefi-ext4.json)，
不编译分区或格式化工具。完整原始 `persist` 备份在
[`vendor/piano/backups/`](../../../vendor/piano/backups/)，不参与固件构建。

实现位于 `uefi/components/ext4/` 和 `uefi/components/nv/`，由
`tools/firmware/prepare_ext4.py`、`tools/prepare_nv_runtime_guard.py` 接入构建。
实机已完成保存、重启恢复和启动现有 Linux；退出后只读导出的分区副本通过
`e2fsck -fn`，两个变量副本的 CRC32 均一致。

上游 `SecureBootHandlerLib` 的文件系统扫描曾遗漏关闭根目录，导致 ext4
拒绝卸载。修正在 `patches/firmware/series.json` 中登记的补丁里：每个扫描
句柄都配对关闭，通知缓冲只释放一次，不绕过文件系统的打开文件检查。

## 电源动作与退出

“电源与重启”提供关机、普通重启、Recovery、Bootloader 和 EDL（9008）。
动作只在菜单中登记，界面退出后由驻留核心保存设置、卸载文件系统、停止
USB 与 UFS，再通过标准 `ExitBootServices` 退出，最后调用运行时复位。
该复位路径不需要也不授予 EFI／UKI 的完整 DDR 访问权限。

Recovery／Bootloader 使用原厂设备树的 PMK8550 SDAM 重启原因字段；
EDL 使用 TCSR／SCM 下载模式，代码位于 `uefi/components/reset/`。
普通重启已完成实机检查；Recovery、Bootloader 和关机尚未执行实机检查，
EDL 未在设备上执行。

启动和退出失败时，屏幕说明失败步骤及返回 Android 的方法，状态码作为
补充。回到 Android 的路径是长按电源重启，在早期选择器按音量上选 Android。

USB 初始化与 Linux 加载结果会保存在固件内存中，并在菜单运行时每五秒输出摘要，
避免早期日志被 UFS 文件读取记录覆盖。摘要只重发已保存的状态，不重新访问 USB
寄存器。读取失败时可区分 ESP 文件读取、镜像解析和后续启动交接阶段。

## 中文默认语言与 FAT 挂载

固定 Mu_Basecore 的 `FatPkg/EnhancedFatDxe/UnicodeCollation.c` 在启动驱动前
使用 `PlatformLang` 与模块的默认语言匹配 UnicodeCollation2。
固件提供的 EnglishDxe 只声明 `en`，全局默认 `zh-CN` 时两种语言均无法匹配，
FAT 驱动返回 Unsupported，不发布 SimpleFileSystem。

产品 DSC 只为 `Fat.inf` 设置 `PcdUefiVariableDefaultPlatformLang="en-US"`，
提供文件名比较服务的回退语言；中文界面与全局 `PlatformLang` 保持不变。
菜单已通过此回退挂载 ESP 并启动现有 Linux。

## USB 复位与供电

之前正常 BOOT 进入 UEFI 时，ABL 已关闭 PHY，DWC31 2.00a 的 `DCTL.CSFTRST` 持续置位，200 ms 后超时；只开控制器时钟或清除 SUSPHY／ENBLSLPM 不能恢复。

`PianoUsbPhy.c` 按同机 `mu_wide_platforms` 的 UsbConfigDxe 和原厂 XBL 表初始化 M31／QMP PHY，并通过原厂 RPMh 协议申请 `ldod2`、`ldog3`、`ldof1`、`ldob5`、`ldob15` 供电。中继器原先虽有使能位，但 `FD08=00`，缺少供电；补齐后正常 BOOT 可枚举，`CSFTRST` 在 20 ms 内完成。

PHY 是 8 位寄存器，32 位读取会复制字节，例如写入 `1` 返回 `0x01010101`。退出时按有效位校验，不能把这种正常读回误报为失败。设备核心先停止 DMA、释放缓冲并撤销 SMMU，再停 PHY、恢复 RPMh 原投票和归还时钟；任何一步无法确认完成仍拒绝启动交接。

合体 BOOT 已完成身份、日志、3200×2136 截图读取和 Fastboot 普通重启，
重启后重新枚举并启动原有 Linux。Linux 保留日志记录 PHY 停止、15 项供电
恢复及 UFS／USB 资源退出成功。

正常文件读取不再逐块写入默认 RAM 日志：`BLOCKIO_READ` 的成功提交、完成和
协议内容改为 `DEBUG_VERBOSE`，需要逐次 DMA 记录时可在调试构建中开启。
分配、映射、退出和错误记录继续保留，失败请求与响应仍会输出。
这样可以避免读取 Linux 文件时覆盖前面的 USB 初始化现场。

## USB 主机

在 SimpleInit、固件设置或 Shell 里插上 OTG 转接头连接的键盘、鼠标、Hub 或
U 盘，固件把平板的 USB 口从设备模式切换为主机模式，拔下后切回 UEFI
Fastboot。实现已编译进产品镜像，尚未在平板上验证。

角色由 PMIC 的 Type-C 状态决定。固件通过 SPMI 只读通道读取 PMIH010X
充电模块的 Type-C 寄存器，布局取自本机原厂 UEFI 的 PmicDxe：类型 `0x02`、
子类型 `0x7f`/`0xa8`/`0xba` 的充电外设之后 `+0x600` 是 Type-C 块，`+0x08`
的 Rd 位表示接入的是 USB 设备，`+0x0B` 的位 0、位 1 是接入和插头方向。
只有 PMIC 型号为 `0x47` 或 `0x56` 时才这样读取，其他型号保持设备模式。
连续两次、相隔 250 ms 读到“接入 USB 设备”才切到主机，切回时同样需要两次。
切换只在界面通过 `WaitForEvent` 等待输入时进行；开机倒计时和读取启动文件
不经过这个等待，不会切换，Shell 命令只有在等待按键时才可能切换。PMIC 同时
报告对方的 Rp（电脑或充电器在供电）时，始终保持设备模式。

切到主机时，常驻 USB 服务先按原有流程停止 Fastboot，释放 9 个 DMA 缓冲区并
确认控制器停下；时钟、PHY 和 USB0 的 SMMU 页表继续由同一个 USB 服务持有。
随后按原厂 UsbConfigDxe 的 `Usb30InitHost` 写 DWC3 全局寄存器（另按 Linux
对本机设备树的处理设置 `GCTL.DSBLCLKGTNG` 和 `GUCTL.HSTINAUTORETRY`），把 xHCI
寄存器窗口（`0x0A600000` 起 32 KiB）通过 `PianoUsbHostPci` 交给标准
XhciDxe，再由 UsbBus、UsbKb、UsbMouse、UsbMassStorage 接管设备。DMA 只经过
USB0 的 SMMU 页表：XhciDxe 的环和上下文放在改成非缓存（`EFI_MEMORY_WC`）的
页上，数据传输经由每次主机会话只映射一次的 1 MiB 中转区复制，并显式维护缓存。

U 盘在 UEFI 里只读。固件补丁让 UsbMassStorageDxe 的 `WriteBlocks` 直接返回
写保护，并把介质报告为只读，不会向 U 盘发送 WRITE 命令。

固件不控制 VBUS。原厂 PmicDxe 的 VBUS 开关函数是空函数，原厂 UEFI 也不写
OTG 命令寄存器，所以这里不新增 PMIC 写入。PMIC 是否会自行给不带电源的设备
供电还没有实机结论；每次 Type-C 状态变化时，日志 `SUNUEFI_USB_TYPEC_POWER`
记录 DCDC 和 USBIN 的只读状态，`SUNUEFI_USB_TYPEC_MODE` 记录 Type-C 端口模式
配置，用来判断有没有输出 5 V、端口是否只作受电方。

不做 USB PD 协商，也不交换数据角色：带电源的 USB-C 扩展坞给平板供电时，
平板是受电方和设备，扩展坞上的设备不会出现。QMP PHY 的通道方向沿用开机时
的设置，插头方向不同时 USB 3 设备会以 USB 2 速度工作。

启动 Linux、重启或关机前，USB 服务先退出主机模式：断开 XhciDxe 及其上层驱动，
确认 PCI I/O 层没有剩余映射和分配，停止并复位 xHCI，撤销中转区映射，把 DWC3
寄存器恢复为设备模式的值，然后照常关闭 SMMU、PHY 和时钟。Linux 看到的控制器
状态与没插过 USB 设备时相同。这一步在任何退出动作开始前进行：还有文件或驱动
占用 U 盘时，屏幕提示拔下 USB 设备后重试并回到菜单，拔下后可以直接再次启动。
无法确认 xHCI 已停下、DMA 可能仍在进行时拒绝交接，屏幕显示“USB 设备仍在使用，
未能安全断开”。

日志里 `SUNUEFI_USB_TYPEC`、`SUNUEFI_USB_ROLE`、`SUNUEFI_USB_HOST_*` 记录
检测结果、切换过程和 xHCI 能力寄存器。主机模式下 UEFI Fastboot 不在线，
拔下 USB 设备、接回电脑后用 `fastboot -s SunUEFI-piano oem ramlog` 取回。

源码位置：

* `uefi/components/usb-host/PianoUsbTypec.c`：Type-C 状态读取。
* `uefi/components/usb-host/PianoUsbHost.c`：主机模式寄存器、DMA 和退出。
* `uefi/core/PianoUsbController.c`：设备与主机角色切换。
* `uefi/core/PianoUsbHostPci.c`：PCI I/O 层，控制器运行中释放的共享缓冲区保留到 xHCI 停下后再释放。
* `patches/firmware/mu-basecore/cfb9372/`：U 盘只读。
