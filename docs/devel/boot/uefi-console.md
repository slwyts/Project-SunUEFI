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
触屏、磁吸键盘及触控板的 UEFI 驱动尚未完成，Linux 下可用不等于 UEFI 下可用。

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
