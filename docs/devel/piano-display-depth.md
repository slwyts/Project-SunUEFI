# Piano 显示位深与 HDR

2026-10-08，依据冻结的 `release-7.2.9` 源码、原厂最终 Android DTB 与公开显示驱动做静态核对；尚未采集 Android 的 SDR/HDR 原子状态或活跃显示寄存器。

## 已有的十位链路

Piano 面板驱动已经使用 `MIPI_DSI_FMT_RGB101010`，DSC 1.1 输入为 **10bpc**、压缩位率为 **8bpp**，slice 为 `800×24`。原厂 BOE、CSOT 的最终 DTB 都是 RGB30bpp，三个 DSC 时序均为 10bpc；page `0x10` 的 DSC 初始化命令也与当前驱动一致。压缩后的 8bpp 是带宽参数，不是八位面板。

本地源码：[NT36532 驱动](../../build/kernel-worktrees/release-7.2.9/drivers/gpu/drm/panel/panel-novatek-nt36532.c)，`piano_dsc_cfg`、`piano_init_sequence()` 与 `PIANO_PANEL_INFO`；原厂采集位于 `private/analysis/android-board-runtime-2026-10-05/live.dtb`。本机原厂选择为 CSOT `p81_35_02_0b`，应保留对应初始化序列。

[小米官方规格](https://www.mi.com/global/product/xiaomi-pad-8-pro/)和 [FAQ](https://www.mi.com/ph/support/faq/details/KA-667493/)明确宣传 12-bit 色深，但未说明 native12、10+2 FRC 或内部 LUT 精度。现有 DTB 与未公开含义的 DDIC 命令，不能确定“额外两位”在哪一层实现。

## 原厂 PP dither 不用于这套十位配置

MiCode 显示驱动固定提交 `aa06fd1757c28dce96fbe4e04d4530ec21b52aac` 的 [_sde_encoder_setup_dither()，第 4002 行](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_encoder.c#L4002)明确在 DSC 10bpc 或面板 30bpp 时调用 `setup_dither(..., NULL, 0)` 关闭 PP dither。原厂 [bitdepth 映射](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_pingpong.h#L18)长度为 9，[接收端](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_pingpong.c#L328)拒绝 10/12 的 bitdepth 参数。因此，开启 PP temporal 不能作为还原原厂 10+2 的实现。

另一个 [DSPP PA dither 实现](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_color_processing_v1_7.c#L967)仅设置 enable、offset、strength 和矩阵，没有 temporal 控制。PP temporal、DSPP PA dither、FP16/PQ 与 DDIC FRC 应分别确认。公开源码描述的配置也尚未用实际模块的寄存器状态验证。

## 十位 framebuffer 与 HDR

十位 framebuffer 和 DSC10 只能说明像素精度与传输配置。HDR 还需要正确的原色、传递函数、亮度映射，以及实际消费这些设置的显示路径。

Mutter 48.7 从 [KMS connector 与 EDID](https://github.com/GNOME/mutter/blob/48.7/src/backends/native/meta-output-kms.c#L469)同时检查 BT.2020、HDR type 1/PQ 与 metadata 能力，满足后才提供 [BT.2100 模式](https://github.com/GNOME/mutter/blob/48.7/src/backends/meta-monitor.c#L210)。冻结基线的面板 `get_modes()` 没有报告 bpc/HDR 或 EDID，[MSM DSI connector](../../build/kernel-worktrees/release-7.2.9/drivers/gpu/drm/msm/dsi/dsi_manager.c)也没有附加并消费 `Colorspace`、`HDR_OUTPUT_METADATA`。DSI 不走 HDMI connector 的自动属性安装路径。

最小改动顺序是补充已知 `bpc=10`，保留 RGB101010、DSC10 和原厂 DDIC 序列，再接入有依据的色彩处理与标准 DRM 属性。**不得用假 HDR metadata、未经确认的 HDR EDID 或 `max_bpc=12` 让桌面出现开关。**

## 最少的 Android 动态采集

保持同一刷新率、亮度和色彩模式，SDR 与实际 HDR 内容各保存一个稳定状态，只采集活跃显示路径：

- DSI connector、CRTC、plane 的关联，实际 framebuffer 格式与 DSC 配置/PPS；静态 `hdr_properties` 只用于核对面板能力，不能证明当前输出 PQ。
- Connector `SDE_PP_DITHER_V2`，CRTC `SDE_DSPP_PA_DITHER_V1`、`SDE_DSPP_GC_V2`、`SDE_DSPP_IGC_V5`、`SDE_DSPP_GAMUT_V4` 的实际 blob ID 与内容；同时记录活跃 SSPP 的 FP16 GC mode、CSC/IGC 配置。
- 驱动现有 dump 中，活跃 PP 的 dither enable、bitdepth/temporal 位，以及 DSPP dither control 和矩阵。实例基址按原厂 DT 与活跃对象确定，不能套用主线 catalog 地址。

若这些记录仍不能解释 10+2，再查 DDIC 的可靠寄存器定义与对应状态。当前采集没有 DRM blobs 或 MMIO，不能据此宣布 FRC/HDR 已启用。
