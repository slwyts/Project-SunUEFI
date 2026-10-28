# 第三方来源与本地材料

SunUEFI 站在这些项目之上：EDK2 / Mu-Silicium 提供 UEFI 基础和高通平台集成，simple-init 提供 UEFI 里的图形菜单，linux-piano 与 Linux 内核提供板级驱动，debian-piano、piano-firmware、piano-mesa、piano-sensors 提供根系统、设备固件、GPU 用户空间和传感器通信，MiCode 与本机原厂系统用作对照参考。各项目负责什么、我们增加了什么，见[上游项目](docs/devel/upstreams.md)。

本页是精确来源索引：固定的提交、使用它们的脚本和已知的许可线索。完整提交号以链接的锁文件和脚本为准。许可线索来自仓库中的文件声明，没有逐一重新核对上游许可正文。

## 源码依赖

| 组件 | 来源与固定身份 | 本仓库使用方式 / 工具 | 许可核对位置与当前范围 |
| --- | --- | --- | --- |
| Mu-Silicium | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)，`3ec9169f308c01030a7e698072aca5acb2c0a144` | [prepare_piano.py](tools/prepare_piano.py) 派生 OnePlus 模板；[build_stage0.sh](tools/build_stage0.sh) 编译 | 上游许可与文件声明；[DeviceBuild.py](uefi/platforms/pianoPkg/DeviceBuild.py) 保留 Microsoft 版权/BSD-2-Clause-Patent。 |
| mu_wide_platforms（移植参考） | [sunflower2333/mu_wide_platforms](https://github.com/sunflower2333/mu_wide_platforms)，查阅提交 `3e1d052064beb6e59cda3cfba87d50e6cc088174` 的 [Piano 平台](https://github.com/sunflower2333/mu_wide_platforms/tree/3e1d052064beb6e59cda3cfba87d50e6cc088174/Platforms/PakalaPkg/Device/xiaomi-piano) | 同机 Project Mu UEFI、平台初始化与 ACPI 的参考资料；现有构建依赖见上方 Mu-Silicium 条目 | 仓库标注 BSD-2-Clause；具体源码及附带二进制按各自声明处理。 |
| Mu_Basecore、Mu Plus、Mu OEM Sample、Silicium-ACPI、Device-Binaries | URL/commit 见 [sources.lock.json](sources.lock.json) | Mu 构建树及 [product hooks](tools/prepare_product_pump.py) | 各来源的许可/版权；Device-Binaries 还需按二进制材料核对。 |
| dtc | [dgibson/dtc](https://github.com/dgibson/dtc)，`f3451d12532b9d382707c458a2ae2e5aa0e1eee4` | `upstream/dtc`、DTB 工具 | 上游固定提交的许可和文件声明。 |
| simple-init | [BigfootACA/simple-init](https://github.com/BigfootACA/simple-init)，`3d66a6e78d519dd050fbebde4db6c5ac933f9aa4` | [prepare_simpleinit.py](tools/prepare_simpleinit.py)、[build_simpleinit.sh](tools/build_simpleinit.sh) | 上游声明；[本地 runtime](uefi/components/product-pump/simple-init/src/gui/piano_product_runtime.c) / 头文件为 LGPL-3.0-or-later。 |
| lwext4 | [gkostka/lwext4](https://github.com/gkostka/lwext4)，固定来源和归档摘要见 [uefi-ext4.json](config/uefi-ext4.json) | [prepare_ext4.py](tools/firmware/prepare_ext4.py) 暂存标准 ext4 库，UEFI 文件协议适配在 `uefi/components/ext4/`；不编译格式化或分区工具 | 多数文件 BSD-3-Clause；`ext4_extent.c`、`ext4_xattr.c` 含 GPL-2.0，组合库按 GPL-2.0 处理，保留源码版权与许可。 |
| Linux / linux-piano | [blu-sharky/linux-piano](https://github.com/blu-sharky/linux-piano)、[torvalds/linux](https://github.com/torvalds/linux)；救援 pin 见 [kernel-profiles.json](linux/kernel-profiles.json) | [build_kernel.py](tools/build_kernel.py)；完整基线见 [full builder](tools/build_piano_full_kernel.py) / [next builder](tools/build_piano_next_full.py) | 选定内核的 `COPYING`、`LICENSES/` 和文件声明；不改变主机脚本的许可。 |
| BigfootACA 高通显示补丁（Junhao Xie） | [PLL 初始化 `67b3cbd4bab5`](https://github.com/BigfootACA/linux/commit/67b3cbd4bab5687c3aa3c9fe5a14c37433e28bbf)、[DPU 停止顺序 `8f353c130fee`](https://github.com/BigfootACA/linux/commit/8f353c130fee0fcf09afbe10e7fc58f901931418)、[PHY 运行时接口时钟 `b95037aef0a0`](https://github.com/BigfootACA/linux/commit/b95037aef0a0bf5cd612e7517a67644887e9fd5d) | 默认内核补丁 `0024`、`0026`、`0034`；分别修正 PLL 初始化、视频流水线停止顺序和 PHY 接口时钟的运行时管理 | 保留原作者与 Signed-off-by；沿用相应 Linux 源文件的 SPDX 和内核许可。PHY 补丁仅调整本地已有恢复辅助代码附近的上下文。 |
| MiCode 板级资料 / 公开内核 | [MiCode/Xiaomi_Kernel_OpenSource](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)，`45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71`；DTS pin 见 [来源集](docs/archive/linux-bringup/piano-full-linux-candidate.md) | [PianoKeys.c](uefi/core/PianoKeys.c) 等注明参考来源 | 原声明保留；本地 Keys 为 GPL-2.0-only，TouchProbe 为 GPL-2.0-or-later。 |
| Debian/Piano 用户态、Mesa、固件 | 组件/精确 commit 见 [Linux 来源集](docs/archive/linux-bringup/piano-full-linux-candidate.md) | [stage_piano_full_userspace.py](tools/stage_piano_full_userspace.py) 等暂存到派生根系统 | 包、服务源码、Mesa 与固件按各自通知处理。 |
| xiaomi-sheng-thp 笔解算 | [ianchb/xiaomi-sheng-thp](https://github.com/ianchb/xiaomi-sheng-thp)，`34046210932d654a4c0df0121ecc31c008f8148c` | [Piano 参数化核心](linux/userspace/pen/piano-pen-core.h) 从独立矩阵解算与滤波代码派生，读取本机实际校准配置；不启动上游的第二个触控服务 | Apache-2.0；保留 [完整许可证](linux/userspace/pen/Apache-2.0.txt)、源文件 SPDX 和 [来源／修改说明](linux/userspace/pen/OPEN_CORE_ORIGIN.txt)。当前仅完成真实配置和压力报告处理，坐标仍需实际笔矩阵验证。 |

`sources.lock.json` 只覆盖部分依赖，其他 pin 仍在脚本中。内核 linked-worktree 获取方式见 [内核复现说明](docs/archive/linux-bringup/linux-reproducible-builds.md)。

小米平板相关参考还包括 [code002-2/Xiaomi-pad-6s-pro-Linux](https://github.com/code002-2/Xiaomi-pad-6s-pro-Linux/tree/053169b1ed48335d8139694ea6ccaacf52462dd9) 及其引用的 [debian-sheng](https://github.com/ianchb/debian-sheng)、[xiaomi-mipps-auth](https://github.com/ianchb/xiaomi-mipps-auth/tree/9176efbdf276b874fc0a97912a914449a88155fb) 和 [xiaomi-sheng-fingerprint](https://github.com/ianchb/xiaomi-sheng-fingerprint/tree/76e7301163b0e708f609c9864b3e5833e9f57402)。分别用于多发行版集成、MiPPS 认证和标准 libfprint/fprintd 接入的参考。指纹项目使用 FPC1553，Piano 原厂使用 Goodix，不能直接互换传感器协议。MiPPS 工具标注 GPL-2.0-only；指纹后端为 Apache-2.0，libfprint/fprintd 补丁及 QTEE 依赖遵循各自许可。

内核 MiPPS [补丁](patches/linux/7.3/0027-power-piano-mipps-authentication.patch)移植了上述 `xiaomi-mipps-auth` 的协议常量和摘要计算，并参考 [blu-sharky/piano-mipps-auth](https://github.com/blu-sharky/piano-mipps-auth) 对 Piano 固件漏报充电器身份的处理及原厂命令顺序，保留 GPL-2.0-only。认证在内核工作队列中完成，不安装上游的用户空间服务、状态文件和通知。来源提交记录在[内核补丁清单](patches/linux/7.3/series.json)的 `mipps_authentication` 中，协议与限制见[内核说明](docs/devel/linux/kernel.md#usb-type-c)。

HyperOS 4 的 [ADSP](vendor/firmware/hyperos4-adsp/README.md) 和
[CDSP](vendor/firmware/hyperos4-cdsp/README.md) 固件来自本机原厂固件分区，
保留原始签名、文件名和逐文件 SHA256，按清单装入四个发行版。
它们属于 Qualcomm／小米专有固件，不适用本项目的源码许可。

原生 Goodix 客户端使用 [Qualcomm MinkIPC](https://github.com/qualcomm/minkipc)、
[quic-teec](https://github.com/quic/quic-teec) 和
[QCBOR](https://github.com/laurencelundblade/QCBOR) 的公开源码。
精确提交、源码许可和构建输入记录在
[`config/fingerprint.json`](config/fingerprint.json) 及镜像内的
`/usr/share/piano-provenance/fingerprint.json`；相应许可证随静态客户端安装。
本项目独立实现本机 Goodix 命令封装，复用标准 UIO／TEE 接口。
原厂签名程序，以及从本机 `libgf_hal.so` 提取的通用防伪和识别模型参数，
均保留原字节，作为硬件固件提供，不包含用户指纹模板。来源符号、文件偏移
和校验值记录在固件清单中，原厂可执行库不随系统安装。来源与范围见
[固件说明](vendor/firmware/hyperos4-fingerprint/README.md)。

Gunyah 的 Android 驱动与主线接口适配来自
[code002-2/librevm](https://github.com/code002-2/librevm/tree/56918558e2c4303cda803a655ea716869c551b71/packaging/gunyah-driver-7.2)
整理的补丁包，原适配来自
[SuperTurtleDev/iceland-mainline-kernel-linux](https://github.com/SuperTurtleDev/iceland-mainline-kernel-linux/tree/fd503a492d381687c143b7085ea79ebce58dcdc7)。
驱动上游为 Android common `android17-6.18-2026-06_r1`，提交
`30c0eaeef78d5c19f687ad8418c27b76353ab261`。本地
[`0006-gunyah.patch`](patches/linux/7.3/0006-gunyah.patch)保留原作者、
版权和 GPL-2.0-only 等逐文件声明；未引入其他平板的设备树或内存预留。
它们用于接入设备已有的 Gunyah 固件，不替换高通 Hypervisor。

BORE 6.8.0 来自 Masahito Suzuki 的调度器实现，以及
[CachyOS/kernel-patches](https://github.com/CachyOS/kernel-patches/blob/6266b5f33f4e8caaa452174f0132c8c82f919267/7.3/sched/0001-bore-cachy.patch)
中 Piotr Gorski 整理的 Linux 7.3 补丁。本地
[`0007-bore.patch`](patches/linux/7.3/0007-bore.patch)只调整了
`CONFIG_CACHY` 专用默认值的上下文及空白格式，保留 BORE 算法和运行参数默认值。
代码沿用 Linux 内核许可；原始补丁校验值和提交记录在
[`series.json`](patches/linux/7.3/series.json)中。MGLRU、ZRAM、
`schedutil` 与抢占机制属于 Linux 自身功能，不作为 CachyOS 独有改动介绍。

FastRPC 的 DSP 重启清理修复参考高通 Jianping Li 提交的
[三个上游补丁](https://github.com/qualcomm-linux/kernel/pull/1212)，
本地适配为 [`0011-fastrpc-drain-ssr-invokes.patch`](patches/linux/7.3/0011-fastrpc-drain-ssr-invokes.patch)。
修复设备开放时的初始化顺序、轮询调用的退出通知，以及活动调用尚未退出就拆除 DMA 映射的竞态。
原始提交记录在补丁正文与内核来源清单中，沿用 Linux 源文件的 GPL-2.0-only 许可。
这些改动处理 DSP 崩溃后的清理，不代表已经解决 DSP 最初的崩溃原因。

SteamOS 根系统提取使用 [hashtagbasit/SteamOS-ARM-Port](https://github.com/hashtagbasit/SteamOS-ARM-Port/tree/e54dac93afccf76e5fefe998399b6c1b1a23f3a1) 的 `scripts/extract_rootfs.py`（GPL-2.0）。提交与文件 SHA256 固定在 `config/steamos.json`，下载时保留源文件通知。该项目提供提取方法和移植参考，SunUEFI 使用自己的启动器、设备内核与 BSP。Valve ARM64 根系统与公开 Steam 客户端分别记录在 `config/steamos.json`、`config/steam-client.json`，继续适用各组件的原有许可，不因提取脚本采用 GPL 而改变。FFmpeg 使用既有 BSP 补丁，构建为匹配 SteamOS 库 ABI 的 pacman 包，保留完整编解码配置与许可文件。

SteamOS 的 Gamescope 采用同一固定 `SteamOS-ARM-Port` 提交中
`external-and-mods/gamescope` 的移动设备适配源码，保留其逐文件版权及
Gamescope 的 BSD-2-Clause 许可。依赖子项目取自 Valve Gamescope 3.16.30
（`ad2763da1c48860f649abfe842a087188dcb6e20`），保留各自许可；
原参考脚本指定的 `6edb42b...` 对象无法取得，因此不依赖该对象。
本项目增加[渲染与显示节点分离修正](patches/gamescope/0001-drm-separate-render-and-display.patch)，
使 Adreno 渲染设备与独立 DPU 显示控制器可以一起工作。
构建来源记录在 `tools/build_steamos_gamescope.py` 和生成的 `SOURCE.json`。

SteamOS 缺少的 KDE 组件取自 [KDE 官方发行归档](https://download.kde.org/stable/)。
版本、URL、SHA-256 和依赖写在 `config/steamos.json` 的 `kde_packages` 中，
按 Valve ARM64 系统现有 ABI 构建，保留每个组件的 `LICENSES/`。

SteamOS Manager 使用 [Valve 26.4.1 源码](https://gitlab.steamos.cloud/holo/steamos-manager/-/tree/a0d399defebd60db30ba38325d9cf47dbda49bee)，
保留 MIT 许可；本项目的补丁使可选内核日志跟踪失败时仍能提供平台管理。
源码、Rust 工具链和补丁摘要由 `tools/build_steamos_manager.py` 固定。

预装兼容工具取自 [GE-Proton11-11 ARM64 发布](https://github.com/GloriousEggroll/proton-ge-custom/releases/tag/GE-Proton11-11)，
容器运行时取自 [Valve Steam Linux Runtime 4.0 ARM64](https://repo.steampowered.com/steamrt4/images/4.0.20260928.262390/)。
二进制与其附带的 Wine、FEX、DXVK、vkd3d-proton 和其他组件保留各自许可、
版权和源码提供方式，不改变为本仓库许可。下载身份、摘要及关键源提交固定在
`config/steam-runtime.json`；构建直接提取公开工具包，不复制用户的 Steam 账户数据。

## 字体、图标与显示资源

| 资源 | 身份与使用 | 通知 / 尚需核对 |
| --- | --- | --- |
| Noto CJK | [prepare_simpleinit.py](tools/prepare_simpleinit.py) 提取主机字体的 SC 字面；主机包版本未锁定 | 实际 hash 写入 `source-manifest.json`；字体许可/通知来自取得包。 |
| Noto Sans CJK SC 设置页字形 | [prepare_product_console.py](tools/prepare_product_console.py) 从 SC 字面生成 16×19 位图，以标准 HII 字体包提供中文 | SIL OFL 1.1；[完整通知](uefi/components/product-support/Library/ConsoleScale/OFL.txt) 随模块和安装包附带，字体与生成数据的 hash 记入产品构建清单。 |
| Font Awesome | 同一脚本下载 `FortAwesome/Font-Awesome` `5.15.4` 字体 | 记录实际 hash，下载前未锁内容 hash；核对该版本字体通知。 |
| AOSP bitmap font | [Font.h](uefi/platforms/pianoProbePkg/Library/RamLogSerialPortLib/Font.h) | 文件头已保存 AOSP 版权及源码/二进制分发条款；发布时保留适用通知。 |
| TianoCore 标志与品牌文字 | [ProductSplashAssets.h](uefi/components/product-support/Library/ProductBootManagerLib/ProductSplashAssets.h) 描述为六辐标志的向量解释 | 保留来源说明，品牌使用范围另核对。 |

`build_simpleinit.sh` 会把字体所在 rootfs 生成链接对象，因此它们可能进入二进制，通知核对不能只限于 Git 中的源码文件。

## 原机固件与二进制发布边界

原生 PE/DEPEX、boot/DTB、厂商模块及固件保存在被忽略的本地目录，取得与输入限制见 [本地输入](docs/devel/local-inputs.md)。

[inventory_native_drivers.py](tools/inventory_native_drivers.py) 清点提取模块；[prepare_product.py](tools/prepare_product.py) 校验并嵌入 FV；[piano_inherited_clock.py](tools/piano_inherited_clock.py) 产生固定 ROM ClockDxe 兼容派生。

这些材料的二进制再分发条件**未审查**；hash 只证明身份，不授予分发许可。未来 Release 按实际内容核对来源与通知，本页不作合法性或兼容性结论。
