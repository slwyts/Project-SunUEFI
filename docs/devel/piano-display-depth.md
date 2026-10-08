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

## 读取实际 DRM 属性与 blob

`tools/android/piano_drm_snapshot.c` 通过标准 DRM UAPI 读取 connector、CRTC、plane 的当前属性与 blob 原始字节。它只改变本次文件描述符的 client capability，不设置显示模式、不申请 DRM master、不访问 MMIO；Android 和 Linux 可使用同一个静态 ARM64 程序。

在准备好的构建环境中编译：

```sh
python3 tools/build_drm_snapshot.py \
  --kernel-source build/kernel-worktrees/piano-fastrpc-dma-7.2.9 \
  --sysroot build/distros/release-7.2.9/rootfs \
  --output build/android-drm-snapshot-capture
```

使用设备的实际 KMS card 节点，不使用 `renderD*`；节点编号不固定。设备恢复后，在同一刷新率、亮度和色彩模式下，先保存 SDR，再播放实际 HDR 内容保存第二份。例如 Linux 中运行：

```sh
sudo /run/piano-drm-snapshot /dev/dri/cardN --blobs > sdr.json
sudo /run/piano-drm-snapshot /dev/dri/cardN --blobs > hdr.json
```

Android 可将程序放在 `/data/local/tmp/`，使用 `adb exec-out su -c` 执行，把标准输出直接保存到电脑。默认省略 blob 内容，`--blobs` 才输出原始十六进制数据，包括存在的 HDR metadata、色彩 LUT 与原厂 dither blob；属性不存在时不会编造。每份记录包含单调时钟起止时间和真实 errno。对象依次读取，**不是跨对象的原子快照**；采集期间应保持画面模式稳定，属性数量增长或 blob 已删除会报错，不把部分结果当成完整采集。

2026-10-08 已在 g086 Linux 的实际 `/dev/dri/card2` 运行静态 ARM64 产物，读取完成且 errors=0；活动 CRTC 为103，DSI connector 指向同一 CRTC。当前没有 `HDR_OUTPUT_METADATA`、`Colorspace` 或 `max bpc` 属性，CTM为0。驱动现有 state 同时显示主画面 `XR30`、光标 `AR24` 和 `3200×2136@144` 模式。这证明当前 Linux 的十位扫描输出，不证明 HDR/PQ 或额外两位 FRC 已启用；原厂 Android 的 SDR/HDR 对照仍未采集。

原始记录为本地 `private/analysis/recover-dma-20261008-131916/g086-drm-snapshot.json`。采集程序不读取扫描输出的 framebuffer 格式或活跃硬件寄存器，仍需配合驱动现有 state/dump；有 blob 不等同于硬件已消费它。
