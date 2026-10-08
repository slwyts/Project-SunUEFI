# Piano 显示位深与 HDR

2026-10-08，依据 `release-7.2.9` 源码、原厂最终 Android DTB、公开显示驱动及现有 Android HWC/Linux DRM 记录核对。Android 当前没有 HDR layer；尚未取得 HDR 输出时的色彩处理状态或活跃显示寄存器。

## 已有的十位链路

Piano 面板驱动已经使用 `MIPI_DSI_FMT_RGB101010`，DSC 1.1 输入为 **10bpc**、压缩位率为 **8bpp**，slice 为 `800×24`。原厂 BOE、CSOT 的最终 DTB 都是 RGB30bpp，三个 DSC 时序均为 10bpc；page `0x10` 的 DSC 初始化命令也与当前驱动一致。压缩后的 8bpp 是带宽参数，不是八位面板。

本地源码：[NT36532 驱动](../../build/kernel-worktrees/release-7.2.9/drivers/gpu/drm/panel/panel-novatek-nt36532.c)，`piano_dsc_cfg`、`piano_init_sequence()` 与 `PIANO_PANEL_INFO`；原厂采集位于 `private/analysis/android-board-runtime-2026-10-05/live.dtb`。本机原厂选择为 CSOT `p81_35_02_0b`，应保留对应初始化序列。

Night Light的缺口同样在真实色彩处理backend：[Mutter48.7 `update_night_light_supported()`](https://github.com/GNOME/mutter/blob/48.7/src/backends/meta-monitor-manager.c#L1340)检查CRTC gamma LUT size是否大于0。g086 connector实际报告NightLightSupported=false，DRM只有CTM；SM8750的GC2尚未接标准gamma接口，因此不添加night-light-enabled强制设置或用户空间染色替身。

[小米官方规格](https://www.mi.com/global/product/xiaomi-pad-8-pro/)和 [FAQ](https://www.mi.com/ph/support/faq/details/KA-667493/)明确宣传 12-bit 色深，但未说明 native12、10+2 FRC 或内部 LUT 精度。现有 DTB 与未公开含义的 DDIC 命令，不能确定“额外两位”在哪一层实现。

## 原厂 PP dither 不用于这套十位配置

MiCode 显示驱动固定提交 `aa06fd1757c28dce96fbe4e04d4530ec21b52aac` 的 [_sde_encoder_setup_dither()，第 4002 行](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_encoder.c#L4002)明确在 DSC 10bpc 或面板 30bpp 时调用 `setup_dither(..., NULL, 0)` 关闭 PP dither。原厂 [bitdepth 映射](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_pingpong.h#L18)长度为 9，[接收端](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_pingpong.c#L328)拒绝 10/12 的 bitdepth 参数。因此，开启 PP temporal 不能作为还原原厂 10+2 的实现。

另一个 [DSPP PA dither 实现](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_color_processing_v1_7.c#L967)仅设置 enable、offset、strength 和矩阵，没有 temporal 控制。PP temporal、DSPP PA dither、FP16/PQ 与 DDIC FRC 应分别确认。公开源码描述的配置也尚未用实际模块的寄存器状态验证。

## 十位 framebuffer 与 HDR

十位 framebuffer 和 DSC10 只能说明像素精度与传输配置。HDR 还需要正确的原色、传递函数、亮度映射，以及实际消费这些设置的显示路径。

Mutter 48.7 从 [KMS connector 与 EDID](https://github.com/GNOME/mutter/blob/48.7/src/backends/native/meta-output-kms.c#L469)同时检查 BT.2020、HDR type 1/PQ 与 metadata 能力，满足后才提供 [BT.2100 模式](https://github.com/GNOME/mutter/blob/48.7/src/backends/meta-monitor.c#L210)。冻结基线的面板 `get_modes()` 没有报告 bpc/HDR 或 EDID，[MSM DSI connector](../../build/kernel-worktrees/release-7.2.9/drivers/gpu/drm/msm/dsi/dsi_manager.c)也没有附加并消费 `Colorspace`、`HDR_OUTPUT_METADATA`。DSI 不走 HDMI connector 的自动属性安装路径。

最小改动顺序是补充已知 `bpc=10`，保留 RGB101010、DSC10 和原厂 DDIC 序列，再接入有依据的色彩处理与标准 DRM 属性。**不得用假 HDR metadata、未经确认的 HDR EDID 或 `max_bpc=12` 让桌面出现开关。**

## 当前真正缺的接线

Android 2026-10-08 的 SurfaceFlinger/HWC实际报告 wideColorGamut、HDR10/10+/HLG/DV，metadata mask=7，最小/最大/最大平均亮度为0.7/1000/1000；DisplayManager也报告HDR类型1/2/3/4。SF每mode的`INVALID`不能据此解释为整机不支持HDR。此份捕获是DISPLAY_P3、120Hz、numHdrLayers=0，SF显示powerMode=Off，因此是能力及软件状态证据，不是正在输出PQ或FRC的证明。原始文件在`private/captures/2026-10-08-android-display-pen/`。

原厂两个P81节点都有真实hdr-enabled、WRGB primaries、peak/blackness字段；MiCode [`dsi_panel_parse_hdr_config`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_panel.c#L4061)读取它们，再发布vendor immutable `hdr_properties`。这不是标准HDR_OUTPUT_METADATA，也没有直接给出PQ/type1标志。两份XBL panel XML都描述DSC1.1/profile0x8/slice800×24，但其PanelDescription都写P81_42/24bpp，连P81_35文件也如此；该文字不能覆盖最终DT的30bpp/DSC10配置，更不能作为额外两位FRC寄存器定义。

标准Linux路径的具体缺口不只是connector属性：当前SM8750 catalog的DSPP只接PCC6，`gc.base=0`，所以`dpu_crtc.c:1925`不发布GAMMA_LUT，当前真实CRTC只有CTM，connector没有HDR_OUTPUT_METADATA/Colorspace/max bpc。原厂DT实际给出GC2.0、IGC5、gamut4.3和PA dither1.7；MiCode [`dspp_gc`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_dspp.c#L97)对GC2只绑定REGDMA实现，失败则NULL，没有旧GC的AHB fallback。[GC2实现](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_reg_dma_v1_color_proc.c#L1214)包含high-precision扩展LUT、swap和REGDMA kickoff。不能只把catalog的gc地址补上就复用现有主线10bit GC，然后宣称HDR已接好。

要支持标准HDR，应先为SM8750接正确GC2/IGC5/色彩处理原子更新，核对panel实际PQ/亮度行为，再让DSI atomic check/commit真正消费Colorspace和HDR_OUTPUT_METADATA；Mutter48.7还要求可信sink/EDID色彩与EOTF信息。添加属性、迁移Android枚举数字或生成虚构EDID都不能替代这些步骤。PP dither、无temporal字段的PA dither和原厂DT另有的AIQE dither必须分别追踪；当前公开AIQE参数与DDIC命令尚不能确认12-bit FRC由谁实现。

已有标准入口 [`0003-panel-nt36532-piano-color-depth.patch`](../../patches/linux/7.2.9/0003-panel-nt36532-piano-color-depth.patch)让Piano RGB101010面板在`get_modes()`报告实际DSC bpc，并已由`prepare_release_kernel.py`引用。本轮准备的重复改动已删除，不留下另一个未接线patch。对应原始字段、版本、source hashes和当前接口差异保存在`private/analysis/piano-hdr-frc-gap-20261008/review.json`；这不是HDR/FRC实现完成的声明。

## 最少的 Android 动态采集

保持同一刷新率、亮度和色彩模式，SDR 与实际 HDR 内容各保存一个稳定状态，只采集活跃显示路径：

- DSI connector、CRTC、plane 的关联，实际 framebuffer 格式与 DSC 配置/PPS；静态 `hdr_properties` 只用于核对面板能力，不能证明当前输出 PQ。
- Connector `SDE_PP_DITHER_V2`，CRTC `SDE_DSPP_PA_DITHER_V1`、`SDE_DSPP_GC_V2`、`SDE_DSPP_IGC_V5`、`SDE_DSPP_GAMUT_V4` 的实际 blob ID 与内容；同时记录活跃 SSPP 的 FP16 GC mode、CSC/IGC 配置。
- 驱动现有 dump 中，活跃 PP 的 dither enable、bitdepth/temporal 位，以及 DSPP dither control 和矩阵。实例基址按原厂 DT 与活跃对象确定，不能套用主线 catalog 地址。

若这些记录仍不能解释额外两位，再查DDIC的可靠寄存器定义与对应状态。目前已取得Android保留的P3软件payload，但仍缺HDR输出时的稳定状态对照，不能据此宣布FRC/HDR已启用。

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

2026-10-08 已在 g086 Linux 的实际 `/dev/dri/card2` 运行静态 ARM64 产物，读取完成且 errors=0；活动 CRTC 为103，DSI connector 指向同一 CRTC。当前没有 `HDR_OUTPUT_METADATA`、`Colorspace` 或 `max bpc` 属性，CTM为0。驱动现有 state 同时显示主画面 `XR30`、光标 `AR24` 和 `3200×2136@144` 模式。这证明当前 Linux 的十位扫描输出，不证明 HDR/PQ 或额外两位 FRC 已启用。

Android相同程序在真正`/dev/dri/card0`完成读取（errors=0），原始记录为`private/captures/2026-10-08-android-display-pen/drm-snapshot.json`。DSI connector67指向CRTC205，首份记录ACTIVE=0、SF powerMode=Off，是保留的软件配置。正常唤醒后追加`drm-snapshot-awake.json`（errors=0），PowerManager为Awake、CRTC205 ACTIVE=1、connector67仍指向205；其余CRTC inactive。四份blob的ID/长度/原始字节与首份完全相同，因此现在取得了active atomic软件配置，仍不是寄存器消费读回。

`hdr_properties`为44字节，enabled、WRGB、peak10000000和black7000与原厂DT完全相同。IGC_V5/GC_V2/gamut_V4分别为4632/7688/39536字节，flags3/2/0；IGC strength=4，包含IGC dither和高精度标志，GC也有高精度标志。两次PP_DITHER、PA_DITHER和hdr_metadata均为0。本次没有播放HDR或强改模式；高精度LUT和内部IGC dither不能直接说明PQ或12-bit temporal FRC。raw payload可作为移植GC2/IGC5数据布局的参照，不应原样复制为所有模式的HDR曲线。离线对照在`private/analysis/piano-hdr-frc-gap-20261008/android-blobs-awake-decoded.json`。

原始记录为本地 `private/analysis/recover-dma-20261008-131916/g086-drm-snapshot.json`。采集程序不读取扫描输出的 framebuffer 格式或活跃硬件寄存器，仍需配合驱动现有 state/dump；有 blob 不等同于硬件已消费它。
