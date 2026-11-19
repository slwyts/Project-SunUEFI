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

SimpleInit 的键盘、相对指针和触屏读取器始终注册，即使启动时尚无该设备。
每次读取先枚举标准协议，删除已经拔出的句柄；在 `TPL_CALLBACK` 内重新查找
协议并读取，防止 USB 拔出在查询和调用之间释放接口。不再留下指向已退出
SimpleInit 的协议安装通知。USB 键盘沿用 UsbKbDxe，固件设置和 Shell 沿用
标准控制台，不另外接入 HID。

键盘优先读取 `SimpleTextInEx`，没有该协议时读取 `SimpleTextIn`。Tab 和
Shift+Tab 切换焦点，方向键在编辑状态保留各自方向，Home、End、Delete
传给控件；每个键后向 LVGL 发送一次释放，连续相同按键不会合并。音量键
仍可代替左右调节，电源键确认；输入框编辑时空格保留为空格。

## 官方磁吸键盘与触控板

固件在后台初始化键盘套里的 Nanosic WN8030 微控制器，音量键和电源键可立即
使用。启动选项最多等待键盘初始化 3.5 秒；如果键盘就绪，则保留一秒供用户按键。
确认键盘未连接后按原有窗口启动。平板实测键盘 MCU 在开机时约 1.57 秒就绪。键盘以标准
`SimpleTextIn` 和 `SimpleTextInEx` 发布，并带 `ConsoleInDevice` 标记，
SimpleInit、固件设置和 Shell 都通过标准控制台读到它。顶排没有功能键，
右 Ctrl 加 `1` 到 `0`、`-`、`=` 输出 F1 到 F12，与 Linux 相同，因此
右 Ctrl + `=` 是 F12，可以进入固件设置。Caps Lock 灯跟随锁定状态，
按键不自动连发。实机已确认 MCU 固件校验、键盘套连接识别、运行命令和
固件设置中的物理按键操作；触控板移动、点击及拔插后的输入恢复还需上机检查。

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
报告。固件启动后先查询 MCU 版本并发送键盘上电命令，不等待连接报告才上电。
键盘接上时发送打开触控板和键盘供电的命令，此后每 12 秒重发键盘
供电请求。初始化由共享事件循环推进；开机时按下一包的到期时间等待，不额外
凑整到毫秒。每次只推进一步，USB 和触控仍能在键盘传输之间处理事件。

帧头按原厂解析：屏蔽首字节最高位后必须为 `0x57`，第三字节只接受
`0x39`、`0x4a`、`0x5b`、`0x6c`。一帧可以连续包含多个报告；ID 为零表示
报告结束，后面未使用的缓冲区不要求清零，与 Linux 驱动的停止条件一致。
报告大小和触点范围继续逐项检查，截断
或不支持的报告不会提交半帧按键。`PIANO_POGO_INPUT` 汇总接受、拒绝、连接
状态和命令结果，不记录认证材料。

连接状态和供电状态分别来自状态字节的 bit 0、bit 1。物理断开时立即清空按键、
释放触控板按钮，不等供电位一起归零；接上但尚未供电时也会发起上电请求。

帧头、连接状态、MCU 版本请求和上电命令对照原厂 `nanosic_driver.ko` 的
`Nanosic_i2c_parse`、`Nanosic_i2c_specified_packets_detect`、
`Nanosic_803_firmware_version_read` 与 `keyboard_resume_work`：版本请求发往
`0x18` 路由，供电命令为 `0x25 = 1`。参考模块取自
`OS3.0.309.0.WPYCNXM` 官方 OTA，提取时核对每项操作及完整 `vendor_dlkm`
分区摘要；运行固件仍使用清单中的 `OS4.0.6.0.XPYCNXM` MCU 文件，没有换回
旧固件。版本查询不属于键盘认证，也不能证明物理输入已经正常。

传输使用 SE6 的 GENI I²C FIFO 接口，1 MHz，由 CPU 轮询，不使用 DMA，
所以不新增 SMMU 上下文。SE6 必须已经由引导程序载入 I²C 协议固件并开放
FIFO 接口；如果它只开放 GSI 接口，固件只读取状态、不改动它，键盘在 UEFI
中不可用，Linux 照常使用。MCU 的 RAM 固件来自仓库内的
`vendor/firmware/piano-keyboard/MCU_Upgrade.bin`，也就是 Linux 根系统使用
的同一文件；`tools/prepare_product.py` 按该组件的清单核对后生成头文件嵌入
固件，生成文件不提交。

任何一步失败，固件立即释放已经占用的资源，不发布输入设备，只保留音量键
和电源键。RAM 日志中的 `PIANO_POGO_UNAVAILABLE` 写明失败步骤和原因，
例如 `se6-gsi-only`、`boot-id`、`verify` 或 `transport`；`PIANO_POGO_SE6`
记录 SE6 的协议、FIFO 设置和时钟频率，`PIANO_POGO_READY` 和
`PIANO_POGO_COVER` 记录接入和键盘连接。没有键盘时 MCU 照常启动，只是没有
按键输入。

进入 Linux 前，固件卸载输入协议，按 Linux 关机顺序让 MCU 进入睡眠
和复位，等待 10 ms，恢复 L14B、L11B 的原 RPMh 投票，恢复引脚和 GPIO97
的中断配置，恢复 SE6 寄存器，最后释放本固件持有的时钟引用。Linux 驱动
探测时会重新上电、复位并下载固件，看到的状态与其他启动方式相同。这条
路径不使用 DMA，也没有 SMMU 上下文。SE6 的主、次序列器均空闲后才恢复寄存器；次序列器仍在运行时不尝试取消它。SE6 停止或寄存器恢复失败时，不再关闭板级电源、引脚和时钟，保留它们直到重新启动。`PIANO_POGO_RELEASE` 列出没有恢复的项目；输入协议、引脚、电源或时钟未能安全退出时拒绝交接，保留资源并显示恢复操作。

源码位置：

* `uefi/components/pogo-product/`：资源占用与释放、MCU 下载格式、启动状态机和输入协议。
* `uefi/core/PianoGeniI2cPio.c`：GENI I²C FIFO 传输，所有等待都有时限。
* `uefi/core/PianoPogoReport.c`、`uefi/core/PianoPogoInput.c`：报告解析、按键映射和触控板指针。

## 触屏

屏幕的 NT36532E 是触控与显示一体的芯片，没有内置触控固件。固件在后台
给它下载触控固件，完成后触屏以标准 `AbsolutePointer` 发布，坐标范围与 3200×2136 横屏画布相同，SimpleInit 直接用它点按。
只跟踪一个触点，按标准协议分别报告按下、移动和抬起；抬起保留最后坐标，
不修改坐标制造新事件。SimpleInit 同时检查坐标和按钮状态。硬件 SPI 与
GPI DMA 传输已接入源码，GPI 初始化和固件解析已经上机检查；SPI 传输、
芯片识别、整帧数据与菜单点按尚未跑通。

初始化严格按 Linux 7.3 `nt36xxx` 驱动在“没有可跟随的 DRM 面板”时的探测
顺序进行，这正是屏幕已由引导程序点亮时的情形：写 `0x7FFF80 = 0x5A`
做引擎复位，等 10 ms；依次用三组芯片 ID 寄存器读取并匹配 NT36532，每组
最多 5 次，每次先做 bootloader 复位；读 `ENB_CASC` 判断单芯片或级联；
然后执行 `nvt_download_firmware_hw_crc`：bootloader 复位，写 ILM/DLM 的
目标地址、长度和 CRC，打开自动复制，按 512 字节分块写入全部分区，等待
自动复制完成，打开固件 CRC 校验，置 `BOOT_RDY`，确认复位状态在
`0xA0`–`0xAF`，失败时整体重试，共 3 次；最后读取帧长度和固件信息，等待校准
状态 `0xA3` 后，按原厂 `DATA_MODE_53` 发送 active 扫描命令并等待确认。
与 Linux 驱动一样，不操作任何复位 GPIO、稳压器或面板命令。

触控固件按 GPIO100（触控 lcd-id）选择：0 用
`novatek/novatek_nt36532_piano_fw_csot.bin`，1 用
`novatek/novatek_nt36532_piano_fw_boe.bin`，与 Linux 设备树的
`firmware-name` 顺序相同。两份文件来自仓库内的
`vendor/firmware/piano-touch`，`tools/prepare_product.py` 按该组件的清单
核对后嵌入，`PIANO_TOUCH_FIRMWARE` 日志记录所选文件的 SHA-256。

SE2 只开放 GSI 接口，传输使用 GPI1 的 AP 通道，不访问曾引发异常的
`SE+0x2008` 安全配置窗口。GPIO40/41/42/43 复用为模式 0、8 位的硬件 SPI，
频率不超过原厂设备树给出的 19.2 MHz。运行时通过 GPIO162 的上升沿锁存状态
读取一帧原始矩阵；没有锁存事件时低频轮询。一次片选读取完整帧，仍须通过
整帧校验，校验失败的帧不产生点按。
矩阵的头部和全部采样值都必须位于校验覆盖范围内；固件分区按实际下载的
`size + 1` 字节检查源文件和 24 位 SRAM 地址范围，CRC 的四字节读取也单独
检查边界，拒绝截断分区和地址回绕。

GPI 配置来自原厂最终设备树和 `GpiDxe`，描述符格式、SPI 命令及启停顺序对照
Linux `drivers/dma/qcom/gpi.c` 与 `drivers/spi/spi-geni-qcom.c`。全双工 GO
描述符按原厂 `spi-msm-geni` 填写实际接收字数；8 位传输中字数等于字节数。
错误记录寄存器只在全局错误中断置位时表示当前故障，空闲时的非零残留值不能
独立阻止初始化。确认通道空闲并接入本固件的 SMMU 域后，按原厂初始化顺序清理
旧记录；运行中的全局和通用错误中断仍立即停止传输。
通道配置的状态、执行环境编号和通道编号由硬件只读提供，配置与恢复读回时只比较
可写字段；这些位非零不表示写入失败。事件环及收发环的地址、长度与协议字段仍逐项核对。
通道和事件环的配置只在分配期间有效，释放后不再回写这些寄存器；
退出时恢复共享中断设置和串行引擎配置，不把已释放的环重新描述给硬件。
先用已有的
异常保护读取 AP 通道状态，只有通道和事件环未占用时才建立自己的 SMMU
映射。GPI 通过 SID `0xB6` 读取描述符环和写入事件，GENI 串行引擎通过
SID `0xA3` 访问数据缓冲区；原厂 `spi-msm-geni` 也分别使用 GPI 设备和
QUP wrapper 的 DMA 映射。XBL `NAMEDNODE_QUP1` 的输入 `0x03000001`、
`0x03000004` 分别对应这两个 SID。固件将两条路径接入同一受控域，共用页表，
退出前核对并解除两条映射，只恢复它们原先未启用的路由槽位。页表和五份 DMA
缓冲区复用共享内存管理代码，不假定设备地址等于物理地址。
完成事件必须同时匹配通道、描述符地址、字节数和成功状态；全双工
读取须收到发送、接收两条通道的完成事件，不能只凭接收环读指针前移就返回数据。
GPI 全局或通用错误中断置位时，本次传输失败，并记录错误信息，不重新提交描述符。
停止失败则保留内存和时钟并拒绝启动系统。

算法移植自 `piano-touch-view`：锁定非笔的
主帧类型，取 32 帧中值作基线，去除行列共模噪声，在 3×3 邻域找超过阈值
200 的峰并求加权质心，再映射到屏幕坐标（Y 轴翻转）；静止时基线缓慢跟随
漂移。pump 只在等待事件和菜单刷新时推进触屏：下载期间每次写一个分块，之后每次最多读一帧。
固件先处理已排队的 USB 事件，再在传输或分区写入的批次之间读取磁吸键盘、
触控板和触屏。界面仍可操作；切换到其他引导界面、启动系统或重启，要等当前
写盘和回复结束。仅向内存下载、还未写盘时，用户可以手动重启。

失败时固件释放已占用的资源，不发布触屏，`PIANO_TOUCH_UNAVAILABLE` 写明
步骤和原因，例如 `chip-id`、`firmware-start` 或
`transport`；`PIANO_TOUCH_SE2`、`PIANO_TOUCH_CHIP_ID`、
`PIANO_TOUCH_DOWNLOAD`、`PIANO_TOUCH_READY` 记录各阶段结果。连续出现看门狗
帧时按 Linux 的做法重新下载。
`oem ramlog` 重新输出保存的失败步骤、原因和 GPI 传输状态，即使初始化日志已被后续输出覆盖，也能定位失败位置。

进入 Linux 前，触屏先于 USB 和 UFS 退出：卸载触屏协议，停止并释放
GPI 通道和事件环，确认 SE2 空闲，复核 SMMU 路由仍属于本驱动后恢复寄存器、
释放 DMA 映射和缓冲区，解绑
SMMU 域，并恢复它占用前的无效路由条目。其他设备的路由必须保持不变，
解绑前重新读取并确认路由、页表和其他设备的上下文未变，以便 USB 和 UFS
随后按各自的启动状态关闭。最后恢复引脚、GPIO162 中断配置和
时钟引用。不向触控芯片发送关机命令，Linux 探测时会再次复位并下载固件。
没有恢复的项目会写入 `PIANO_TOUCH_RELEASE`，并阻止控制权交给系统。

源码位置：

* `uefi/components/touch-product/PianoTouchService.c`：资源占用与释放、探测与下载状态机、`AbsolutePointer`。
* `uefi/components/touch-product/PianoGeniSpiGpi.c`：硬件 SPI、GPI DMA 描述符与完成检查、安全停止。
* `uefi/components/touch-product/PianoTouchNvt.c`：固件分区解析与芯片表。
* `uefi/components/touch-product/PianoTouchMatrix.c`：原始矩阵到触点。

## 启动项与横屏

SimpleInit 默认旋转改为 0°，使用 GOP 的 3200×2136 横屏画布。界面旋转由
SimpleInit/LVGL 处理，物理显示模式保持原样；实机已确认横屏显示。

产品驻留核心在 BDS 的控制台初始化完成后启动，先恢复变量和安装安全退出协议。
普通自动启动先消费 `BootNext`，再按 `BootOrder` 读取 `Boot####` 的设备路径。
从 SimpleInit 选“启动 Linux”时，沿用系统启动顺序，但跳过 SimpleInit 自身，
也不消费为下次开机设置的 `BootNext`。没有指定文件的设备启动项由上游库尝试标准后备文件，
不绑定某个加载器名称或分区标签。产品安装的 systemd-boot 继续发现
`EFI/Linux/` 中的版本化 UKI。不使用 Android 格式的 Linux `boot.img`
或固件内的 Linux 替代项。通用启动管理的剩余工作见[开发清单](../release.md#标准-uefi-启动与存储)。具体内存与退出身份见
[UEFI 核心](../uefi-core.md#esp-启动文件与系统交接)。

普通非易失变量通过下文的文件后端保存。
UKI 内含内核、initramfs、DTB 和参数，systemd-stub 调用 Linux EFI stub；
固件提供完整内存描述、设备树修正与安全退出。

## 内置菜单与自动保存

[`config/simpleinit.static.uefi.cfg`](../../../config/simpleinit.static.uefi.cfg)
原样内置在 SimpleInit 的 rootfs 中，先作为静态配置加载，再加载
`persist/sunuefi/simpleinit.uefi.cfg` 中的用户设置。首次使用不需要复制
任何文件：目录由固件创建，修改后的设置由 SimpleInit 自动保存。
静态配置里的默认值不会整份复制到用户文件。

默认中文、横屏、DPI 600，开启动画；菜单显示后无操作五秒自动启动，
按键、触摸或滚动取消自动启动。“启动 Linux”排在
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

ext4 驱动用固定的 lwext4 源码读取变量文件、`persist/bluetooth/.bt_nv.bin`
和 `persist/time/ats_2`。只允许创建项目目录并写入两个变量文件，写入还会更新
必要的 ext4 元数据。发现卷需要恢复时只读挂载，变量修改留在内存中，日志由
Android 恢复。标准 Block I/O 和 Fastboot 不提供原厂 `persist` 的原始写入。
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
动作先在菜单中登记。界面退出后，驻留核心保存设置、关闭文件系统，并完成存储、
输入、USB 和显示资源的安全退出。`ExitBootServices` 成功后才调用 `ResetSystem`。

Recovery／Bootloader 使用原厂设备树的 PMK8550 SDAM 重启原因字段；
EDL 使用 TCSR／SCM 下载模式，代码位于 `uefi/components/reset/`。
普通重启已完成实机检查；Recovery、Bootloader 和关机尚未执行实机检查，
EDL 未在设备上执行。

启动和退出失败时，屏幕说明失败步骤及返回 Android 的方法，状态码作为
补充。USB 和固件服务仍正常时，错误页单独处理 Fastboot 的状态、日志和截图，
错误页只执行诊断。资源仍可安全退出时，Fastboot 也可请求重启，驻留核心在发送
`OKAY` 后按正常重启流程退出。检测到未能安全退出的资源时，固件保留可能
仍在使用的 DMA 缓冲区。USB 已关闭或其状态无法确认时保持断开。
软件重启无法完成时长按电源，在早期选择器按音量上选 Android。

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

固件尚未控制 VBUS 或 Type-C 端口模式。原厂 PmicDxe 的 VBUS 开关函数为空；
实机进入 UEFI 时 Type-C `+0x44` 为 `0x02`（只受电），仅检测 Rd 不能补齐
普通 OTG 设备的供电和角色切换。Linux 则通过 ADSP 的 PMIC GLINK／UCSI 和
MCA 控制充电及 OTG 通路，不能直接用旧版 PMIC 的升压寄存器布局替代。
每次 Type-C 状态变化时，`SUNUEFI_USB_TYPEC_POWER` 记录 DCDC 和 USBIN
的只读状态，`SUNUEFI_USB_TYPEC_MODE` 记录端口模式。外设供电及枚举仍未验证。

不做 USB PD 协商，也不交换数据角色：带电源的 USB-C 扩展坞给平板供电时，
平板是受电方和设备，扩展坞上的设备不会出现。同一次启动中翻转插头时，
在等待输入的过程中连续两次确认新方向，待原连接的传输和 DMA 停稳后再切换
QMP PHY 高速通道。新方向配置失败时，恢复原通道后允许 USB 2 连接，
下一次拔插再重试。实际速度以重新枚举后的主机或设备报告为准。

启动 Linux 前，USB 服务先退出主机模式：断开 XhciDxe 及其上层驱动，
确认 PCI I/O 层没有剩余映射和分配，停止并复位 xHCI，撤销中转区映射，把 DWC3
寄存器恢复为设备模式的值，然后照常关闭 SMMU、PHY 和时钟。Linux 看到的控制器
状态与没插过 USB 设备时相同。这一步在系统交接开始前进行：还有文件或驱动
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
