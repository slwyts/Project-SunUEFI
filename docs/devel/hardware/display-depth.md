# 显示位深与 HDR

## 当前实现

Piano 的默认内核使用 RGB101010 和 DSC 1.1：输入是 **10 bpc**，压缩传输为 **8 bpp**，slice 为 `800×24`。压缩位率不等于面板位深。BOE、CSOT 两份原厂最终设备树采用相同的十位传输配置，驱动按面板选择初始化序列。

标准 DRM/KMS 接口包括：

| 接口 | 能力 |
| --- | --- |
| `max bpc` | 固定 10..10，与当前面板和 DSC 配置一致 |
| `GAMMA_LUT` | 1024 项，GCv2 硬件输出曲线；GNOME 夜灯使用此接口 |
| `DEGAMMA_LUT` | 1025 项均匀采样、每色 16 位，映射到 IGCv5 高精度曲线 |
| `CTM` | 现有 PCC 色彩矩阵 |

GC 和 IGC 在同一 REGDMA 缓冲区提交到使用的两路 DSPP，完成后一起生效。设备已完成恒等、非线性、恢复旁路的标准 LUT 更新，传输完成状态正常。实现及资源管理见 [GCv2／IGCv5 后端](gcv2-night-light.md)。

HDR 内容播放可用。GNOME 51 没有单独的 8／10／12 位色深选择器；当前已确认的物理传输仍为十位，不能把 HDR 内容播放等同于 12-bit 输出或面板 FRC。下面的原厂分析和底层接入要求保留为技术记录，不再把之前 HDR 播放问题列为日常使用限制。

## 原厂的色彩处理

固定的 [MiCode 显示驱动](https://github.com/MiCode/vendor_opensource_display-drivers/tree/aa06fd1757c28dce96fbe4e04d4530ec21b52aac)及同机 Android DRM 状态表明：

- 原厂 SDR／P3 模式使用 IGCv5、GCv2 和 Gamut v4，IGC 和 GC 都开启高精度扩展。同机运行状态中 IGC 的内部 dither 开启；QDCM 配置里 `expert_*`、`hal_*`、`video*` 等模式关闭它，见[12-bit 与抖动](#12-bit-与抖动)。
- IGC 的高精度格式是 257 个普通点加 128 个暗部扩展点，不能按均匀 385 点解释。原厂 QDCM 的 sRGB 表与普通步长 `1/256`、暗部步长 `1/1024` 对应；独立的 Gamma 2.2 表误差不超过一个十六位码值。默认后端向用户空间提供均匀 1025 点接口，再选取硬件所需的两组采样点。
- 原厂 DSI 的 `hdr_properties` 描述静态原色和亮度能力；它不等于 HDMI／DP 的动态 `HDR_OUTPUT_METADATA`，也不证明 DDIC 直接接收 PQ 像素。
- 同机 HyperOS 4 播放 HDR10 视频时，视频图层为 `BT2020_ITU_PQ`，显示输出仍为 `DISPLAY_P3`，HDR 的 render intent 为 `0x113`。两路视频平面使用 Gamut v6 三维查找表，整屏继续使用 IGCv5、GCv2 和 Gamut v4。移植时需要处理视频内容到面板输出的转换，不能把 PQ 数据直接送往现有面板链路。

参考源码：[`reg_dmav2_setup_dspp_igcv5`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_reg_dma_v1_color_proc.c#L5366)、[`dsi_panel_parse_hdr_config`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_panel.c#L4061)。

HyperOS 4 的配置位于 `/odm/etc/display/qdcm_calib_data_xiaomi_p81_35_02_0b_vid_mode_dualdsi_dsc_panel.json` 和对应的 `p81_42_02_0a` 文件。JSON 中的十六进制 payload 还需还原字节对顺序，才能读取内部曲线和 XML：

- `native_hdr` 的输出 Gamma 为 2.2，包含 P3 面板校准、17 点色调映射、实时亮度补偿和内容统计；CSOT 配置中的满屏峰值约 844.5 nit，SDR 参考峰值为 400 nit。这些是配置值，不是测试物理亮度测量。
- HDR 模式和 intent 1 的 `standard_*` 模式配置了 LUMA dither，但 `temporalEnable=0`；配置内容不能代替播放时的寄存器状态，也不能单凭该字段认定面板 FRC。
- [`0045` 高精度后端](../../../patches/linux/7.2.9/0045-drm-igcv5-high-precision-grid.patch)暴露均匀 1025 项 `DEGAMMA_LUT`，由驱动选取两组硬件采样点。组合 DMA 扩大到 16 KiB；实机恒等、非线性和旁路恢复已完成，提交 2142 个 word，两路 DSPP 均返回完成状态。
- Linux 7.3 的 [`0016` 补丁](../../../patches/linux/7.3/0016-drm-igcv5-high-precision-dither.patch)按原厂 IGC 控制字，在存在 Degamma LUT 时打开高精度内部抖动，强度为 4；空 LUT 仍保持旁路。该内核已启动 KDE 和 Gamescope，但活动 IGC 曲线下的物理色阶还需检查。它不改变十位 DSC，也不代表面板 FRC 或完整 HDR 已实现。

### HDR 亮度与背光

原厂使用两颗 KTZ8866 背光芯片。系统的 `0..4095` 请求先经过普通亮度／高亮度区间映射，再经过 `mi_bl_level_remap` 曲线，最后写入两颗芯片的 11 位亮度寄存器。不能把系统请求值直接当作芯片码值或亮度。

2026-10-20 的 HDR10 播放对照中，最低亮度时系统请求为 17，两颗芯片均读回 374；最高亮度时请求为 4095，芯片均读回 2047。保持最低亮度后，物理屏幕的变暗得到目视确认。原厂 `actual_brightness` 回调固定返回 0，不能用于判断屏幕是否熄灭；寄存器读回也不是亮度计测得的 nit。

暂停在同一 HDR 视频缓冲区后，再做“最高 → 最低 → 最高”的对照：整屏 IGC、GC、PCC、Gamut 表保持一致，两路视频平面的 Gamut v6 表随亮度变化，最后恢复为第一份表。合成器报告的视频白点也从 800 变为 4.5，再回到 800 nit；这是软件亮度描述，不是实测亮度。原厂 HDR 处理包含亮度相关的视频色调映射，移植时需要同时处理背光和内容映射。

播放器可以通过窗口亮度覆盖系统设置。调试时先检查 `mWindowManagerBrightnessOverride`，再判断系统调光命令是否适用；目视测试保持一个档位等待观察，避免自动恢复后才询问结果。

相关实现：[`dsi_panel_set_backlight`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_panel.c)、[`mi_backlight_ktz8866.c`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/mi_disp/mi_backlight_ktz8866.c)、[`sde_backlight_device_get_brightness`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_connector.c)。

## 12-bit 与抖动

[小米官方规格](https://www.mi.com/global/product/xiaomi-pad-8-pro/)宣称支持 12-bit 色深。原厂链路是十位传输；额外两位只可能来自面板内部 FRC 或 SoC 侧的抖动，下文列出已排除和尚未读取的部分。

不能直接打开高通普通 pingpong dither 来代替。原厂 [`_sde_encoder_setup_dither`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_encoder.c#L4002)在 DSC10／30bpp 配置下关闭该模块，其位深表只覆盖 6／7／8 位输出。IGC 的 16 位 LUT、内部 dither 和面板 12-bit FRC 也不是同一个概念。

 HDR10 播放时，虽然 DRM 的 `SDE_PP_DITHER_V2` 属性包含配置，PP0／PP1 的 dither 使能寄存器（SDE 偏移 `0x6a0e0`／`0x6b0e0`）均读回 0，与原厂驱动的关闭路径一致。不能仅凭属性 blob 非空认定硬件已经启用抖动。

更高位深的链路不存在：原厂 CSOT 节点 `qcom,mdss_dsi_p81_35_02_0b_dualdsi_dsc_vid` 写明 `qcom,mdss-dsi-bpp = 30`、`qcom,mdss-dsc-bit-per-component = 10`，DPU 的 DSC 编码器最高也只支持 10 bpc。Linux 的 CSOT 初始化序列与原厂 120 Hz 时序组的 `qcom,mdss-dsi-on-command` 逐条一致，面板若在内部做 FRC，Linux 下同样生效；BOE 序列尚未逐条对照。

剩下一处没有读过的是 DSPP 抖动。原厂设备树声明了 `qcom,sde-dspp-dither = <0x82c 0x10007>`，DSPP0／DSPP1 位于 SDE 偏移 `0x55000`／`0x57000`，抖动寄存器即 `0x5582c`／`0x5782c`（物理地址 `0xae5582c`／`0xae5782c`）。在 Android 上分别于普通画面和 HDR10 播放时读取这两处，并用 `piano_drm_snapshot --blobs` 导出同一时刻的 DSPP 色彩属性。两处都为 0，说明原厂没有额外的抖动，Linux 与原厂一致；开启时再按读到的位深、矩阵和时间抖动设置在 DPU 中实现，默认开启，不加开关。

原厂配置只在部分模式里要求后混合抖动。`/odm/etc/display/` 下 CSOT 的 `qdcm_calib_data_xiaomi_p81_35_02_0b_vid_mode_dualdsi_dsc_panel.json` 和 BOE 的 `p81_42_02_0a` 同名文件各有 35 个模式，抖动字段逐项相同：

| 模式（render intent） | `PostBlendDither` | `PostBlendPaDither` | IGC 内部抖动 |
| --- | --- | --- | --- |
| `native_hdr`、`native_p3`、`native_srgb`（275，即 HDR10 播放时的 `0x113`） | 开 | 关 | 开，强度 4 |
| `standard_hdr`、`standard_p3`、`standard_srgb`（1，Android 的 ENHANCE） | 开 | 关 | 开，强度 4 |
| `GM1`、`GM2`、`GM3`、`GM11`、`GM22`、`GM33`（263～265），`smart_MC`、`smart_MC1`（258） | 关 | 关 | 开，强度 4 |
| `expert_*`（267～269）、`hal_*`（256、257）、`video*`（259～262）、`Native_D65_*`（273）、`standard_native`（0） | 关 | 关 | 关 |

开启的 `PostBlendDither` 都是同一组值：`ditherMode` 为 `LUMA`，`strengthR`／`strengthG`／`strengthB` 均为 6，`temporalEnable=0`，4×4 矩阵 `[15,7,13,5,3,11,1,9,12,4,14,6,0,8,2,10]`。小米自己的 SDR 色彩模式（intent 256～273）都不开后混合抖动，Linux 也不设置，与这些模式一致；IGC 内部抖动由 7.3 的 `0016` 补丁在存在 Degamma LUT 时按强度 4 打开。活动模式是 intent 1 或 275 时，DPU 需要按上面的值设置抖动：LUMA 模式、三个通道强度 6、这个矩阵、关闭时间抖动。

配置文件没有写明三件事：日常画面用哪个模式（`/product/etc/device_features/piano.xml` 的 `default_display_color_mode=3` 由 displayfeature 服务换算成模式）；`PostBlendDither` 落在 DSPP 的 `0x82c` 还是 pingpong 抖动；强度和 LUMA 模式怎样换算成寄存器位。读上面两个寄存器时，同时记下 SurfaceFlinger 当前的 render intent，并在设置里的每种色彩模式下各读一次。HDR10 播放时 PP0／PP1 已读回 0，如果此时 `0x5582c` 不为 0，这组设置就落在 DSPP。

## HDR 接入要求

后续实现需要完整处理内容的传递函数、色域、面板亮度和色调映射，再通过标准 DRM/KMS 能力交给桌面使用。不能只添加 HDR 属性或虚构 EDID 来显示开关。Mutter 当前还从 EDID 联合检查 BT.2020、PQ 和静态 HDR metadata 能力；无 EDID 的内屏需要的能力描述与对应输出处理。

[`tools/android/piano_drm_snapshot.c`](../../../tools/android/piano_drm_snapshot.c)可在 Android 和 Linux 中只读导出 DRM 属性及 LUT blob。将同一画面设置下的 SDR 与 HDR 播放状态对照，同时确认活动 CRTC、HDR 图层和电源状态；blob 存在或 DMA 完成不能单独证明物理 HDR 输出。
