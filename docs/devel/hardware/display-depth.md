# 显示位深与 HDR

## 当前实现

Piano 的默认内核使用 RGB101010 和 DSC 1.1：输入是 **10 bpc**，压缩传输为 **8 bpp**，slice 为 `800×24`。压缩位率不等于面板位深。BOE、CSOT 两份原厂最终设备树采用相同的十位传输配置，驱动按面板选择初始化序列。

标准 DRM/KMS 接口包括：

| 接口 | 能力 |
| --- | --- |
| `max bpc` | 固定 10..10，与当前面板和 DSC 配置一致 |
| `GAMMA_LUT` | 均匀 65536 项，映射为 GCv2 的 1024 个主采样点和 256 个暗部扩展点 |
| `DEGAMMA_LUT` | 1025 项均匀采样、每色 16 位，映射到 IGCv5 高精度曲线 |
| `CTM` | 现有 PCC 色彩矩阵 |
| `EDID`、`Colorspace`、`HDR_OUTPUT_METADATA` | 固定内屏的合成能力描述、BT.2020 RGB 与 PQ 输出转换；需要匹配的校准固件 |

GC 和 IGC 在同一 REGDMA 缓冲区提交到使用的两路 DSPP，完成后一起生效。实机已完成暗部扩展的恒等、非线性和旁路恢复，完整曲线为 2532 个 word，旁路为 8 个 word，两路 DSPP 均返回完成状态。实现及资源管理见 [GCv2／IGCv5 后端](gcv2-night-light.md)。

KDE 6.6 报告色深为自动 10 位，可选范围为 `10..10`，因此不显示切换位深的控件；
GNOME 51 也没有独立的 8／10／12 位选择器。当前 KWin 使用 `AR30` 十位主平面。
改变传输位深还需要匹配面板命令和 DSC PPS，不能仅扩大 `max bpc` 属性范围。

固定内屏通过标准属性接收合成器的 PQ／BT.2020 输出，由 DPU 转换为面板原生编码，
不是把 PQ 数据直接送入 DDIC。原版 KWin 可以切换 HDR 和广色域；合成器负责
混合内容和色调映射，驱动负责面板信号转换。普通 SDR 默认仍走原有路径。
EDR 是合成器利用背光余量的独立策略，不等同于这个 HDR 输出模式。

## 分辨率与刷新率

同机原厂 `DisplayManager` 和 SurfaceFlinger 枚举的七档模式均为
`2136×3200`，横屏为 `3200×2136`；刷新率为 30、48、50、60、90、120、144 Hz。
BOE、CSOT 最终设备树中每条 DSI 的时序宽度为 1600、高度为 2136，双 DSI 合成
完整宽度。Linux 面板模式表与这些尺寸和刷新率一致，默认选 120 Hz。

内核提供 2560×1708、2400×1602、1920×1280、1600×1068、1280×854，以及 EDID 兼容所需的 640×480 源模式，各自保留七档刷新率。DSI 桥在 `adjusted_mode` 中恢复同刷新率的原生时序，DPU 将平面和光标的目标坐标缩放到完整面板，再进行 SSPP 分配和双路裁剪。DSC 的 slice 和面板命令保持原样。

SteamOS／KDE 在 Linux 7.3.3 上已切换 1600×1068@120 和 2400×1602@60，再恢复 3200×2136@120，物理屏幕保持全屏显示。1600×1068 模式下，内核读回两路 `800×1068 → 1600×2136`，光标也缩放到物理坐标。其他组合及切换后的触摸定位仍需逐项检查。

标准 `scaling mode` 属性支持 `None` 和 `Full`；低分辨率需要 `Full`，超出原生尺寸的请求会被拒绝。全屏映射保留绝对触摸坐标的对应关系，不引入黑边坐标换算。KDE 默认通过 KWin 已有的 `KWIN_DRM_FORCE_SCALING_MODE=FULL` 选择全屏缩放；游戏模式使用内核默认值。桌面的逻辑缩放比例仍独立生效。

## 原厂的色彩处理

固定的 [MiCode 显示驱动](https://github.com/MiCode/vendor_opensource_display-drivers/tree/aa06fd1757c28dce96fbe4e04d4530ec21b52aac)及同机 Android DRM 状态表明：

- 原厂 SDR／P3 模式使用 IGCv5、GCv2 和 Gamut v4，IGC 和 GC 都开启高精度扩展。同机运行状态中 IGC 的内部 dither 开启；QDCM 配置里 `expert_*`、`hal_*`、`video*` 等模式关闭它，见[12-bit 与抖动](#12-bit-与抖动)。
- IGC 的高精度格式是 257 个普通点加 128 个暗部扩展点，不能按均匀 385 点解释。原厂 QDCM 的 sRGB 表与普通步长 `1/256`、暗部步长 `1/1024` 对应；独立的 Gamma 2.2 表误差不超过一个十六位码值。默认后端向用户空间提供均匀 1025 点接口，再选取硬件所需的两组采样点。
- GC 的高精度格式是 1024 个主采样点加 256 个暗部扩展点。原厂 Gamma 2.2 主表覆盖 `0..1`，暗部表覆盖约 `0..255/65536`，输出码值为 `0..82`。SDR 与活动 HDR 播放中两路 GC 控制字均为 `0x5`，两组表相同；Linux 用标准均匀 LUT 提供这两组采样，不把暗部扩展解释为 FRC。
- 原厂 DSI 的 `hdr_properties` 描述静态原色和亮度能力；它不等于 HDMI／DP 的动态 `HDR_OUTPUT_METADATA`，也不证明 DDIC 直接接收 PQ 像素。
- 同机 HyperOS 4 播放 HDR10 视频时，视频图层为 `BT2020_ITU_PQ`，显示输出仍为 `DISPLAY_P3`，HDR 的 render intent 为 `0x113`。两路视频平面使用 Gamut v6 三维查找表，整屏继续使用 IGCv5、GCv2 和 Gamut v4。移植时需要处理视频内容到面板输出的转换，不能把 PQ 数据直接送往现有面板链路。

参考源码：[`reg_dmav2_setup_dspp_igcv5`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_reg_dma_v1_color_proc.c#L5366)、[`dsi_panel_parse_hdr_config`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_panel.c#L4061)。

HyperOS 4 的配置位于 `/odm/etc/display/qdcm_calib_data_xiaomi_p81_35_02_0b_vid_mode_dualdsi_dsc_panel.json` 和对应的 `p81_42_02_0a` 文件。JSON 中的十六进制 payload 还需还原字节对顺序，才能读取内部曲线和 XML：

- `native_hdr` 的输出 Gamma 为 2.2，包含 P3 面板校准、17 点色调映射、实时亮度补偿和内容统计；CSOT 配置中的满屏峰值约 844.5 nit，SDR 参考峰值为 400 nit。这些是配置值，不是测试物理亮度测量。
- HDR 模式和 intent 1 的 `standard_*` 模式配置了 LUMA dither，但 `temporalEnable=0`；配置内容不能代替播放时的寄存器状态，也不能单凭该字段认定面板 FRC。
- [显示补丁中的高精度后端](../../../patches/linux/7.3/0002-display.patch)暴露均匀 1025 项 `DEGAMMA_LUT`，由驱动选取两组硬件采样点。当前组合 DMA 为 64 KiB，容纳两路 GC、IGC 和 Gamut 状态；实机恒等、非线性和旁路恢复已完成。
- Linux 7.3 的 [`0016` 补丁](../../../patches/linux/7.3/0016-drm-igcv5-high-precision-dither.patch)按原厂 IGC 控制字，在存在 Degamma LUT 时打开高精度内部抖动，强度为 4；空 LUT 仍保持旁路。它不改变十位 DSC，也不提供面板 FRC 控制。

### HDR 亮度与背光

原厂使用两颗 KTZ8866 背光芯片。系统的 `0..4095` 请求先经过普通亮度／高亮度区间映射，再经过 `mi_bl_level_remap` 曲线，最后写入两颗芯片的 11 位亮度寄存器。不能把系统请求值直接当作芯片码值或亮度。

2026-10-20 的 HDR10 播放对照中，最低亮度时系统请求为 17，两颗芯片均读回 374；最高亮度时请求为 4095，芯片均读回 2047。保持最低亮度后，物理屏幕的变暗得到目视确认。原厂 `actual_brightness` 回调固定返回 0，不能用于判断屏幕是否熄灭；寄存器读回也不是亮度计测得的 nit。

暂停在同一 HDR 视频缓冲区后，再做“最高 → 最低 → 最高”的对照：整屏 IGC、GC、PCC、Gamut 表保持一致，两路视频平面的 Gamut v6 表随亮度变化，最后恢复为第一份表。合成器报告的视频白点也从 800 变为 4.5，再回到 800 nit；这是软件亮度描述，不是实测亮度。原厂 HDR 处理包含亮度相关的视频色调映射，移植时需要同时处理背光和内容映射。

播放器可以通过窗口亮度覆盖系统设置。调试时先检查 `mWindowManagerBrightnessOverride`，再判断系统调光命令是否适用；目视测试保持一个档位等待观察，避免自动恢复后才询问结果。

相关实现：[`dsi_panel_set_backlight`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_panel.c)、[`mi_backlight_ktz8866.c`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/mi_disp/mi_backlight_ktz8866.c)、[`sde_backlight_device_get_brightness`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_connector.c)。

## 12-bit 与抖动

[小米官方规格](https://www.mi.com/global/product/xiaomi-pad-8-pro/)宣称支持 12-bit 色深。原厂链路是十位传输；额外两位只可能来自面板内部 FRC 或 SoC 侧的抖动，下文列出已排除和尚未读取的部分。

不能直接打开高通普通 pingpong dither 来代替。原厂 [`_sde_encoder_setup_dither`](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_encoder.c#L4002)在 DSC10／30bpp 配置下关闭该模块，其位深表只覆盖 6／7／8 位输出。IGC 的 16 位 LUT、内部 dither 和面板 12-bit FRC 也不是同一个概念。

同机 HyperOS 4 在 SDR 和活动 HDR10 播放时，两路 pingpong 抖动使能寄存器（SDE 偏移 `0x6a0e0`／`0x6b0e0`）和 DSPP 抖动使能寄存器（`0x5582c`／`0x5782c`）均读回 0。HDR 播放时同时确认了 `BT2020_ITU_PQ` 图层和 `renderIntent=0x113`；属性 blob 和静态配置均不能代替这组硬件读回。

更高位深的链路不存在：原厂 CSOT 节点 `qcom,mdss_dsi_p81_35_02_0b_dualdsi_dsc_vid` 写明 `qcom,mdss-dsi-bpp = 30`、`qcom,mdss-dsc-bit-per-component = 10`，DPU 的 DSC 编码器最高也只支持 10 bpc。Linux 的 CSOT 初始化序列与原厂 120 Hz 时序组的 `qcom,mdss-dsi-on-command` 逐条一致，面板若在内部做 FRC，Linux 下同样生效；BOE 序列尚未逐条对照。

DSPP 抖动位置来自原厂最终设备树的 `qcom,sde-dspp-dither = <0x82c 0x10007>` 和 DSPP0／DSPP1 基址 `0x55000`／`0x57000`。这两路后混合模块均关闭，不能作为原厂两位 FRC 的来源。DDIC 内部 FRC 的控制方式仍没有从公开源码或寄存器中确认。

原厂配置只在部分模式里要求后混合抖动。`/odm/etc/display/` 下 CSOT 的 `qdcm_calib_data_xiaomi_p81_35_02_0b_vid_mode_dualdsi_dsc_panel.json` 和 BOE 的 `p81_42_02_0a` 同名文件各有 35 个模式，抖动字段逐项相同：

| 模式（render intent） | `PostBlendDither` | `PostBlendPaDither` | IGC 内部抖动 |
| --- | --- | --- | --- |
| `native_hdr`、`native_p3`、`native_srgb`（275，即 HDR10 播放时的 `0x113`） | 开 | 关 | 开，强度 4 |
| `standard_hdr`、`standard_p3`、`standard_srgb`（1，Android 的 ENHANCE） | 开 | 关 | 开，强度 4 |
| `GM1`、`GM2`、`GM3`、`GM11`、`GM22`、`GM33`（263～265），`smart_MC`、`smart_MC1`（258） | 关 | 关 | 开，强度 4 |
| `expert_*`（267～269）、`hal_*`（256、257）、`video*`（259～262）、`Native_D65_*`（273）、`standard_native`（0） | 关 | 关 | 关 |

配置中开启的 `PostBlendDither` 都是同一组值：`ditherMode` 为 `LUMA`，`strengthR`／`strengthG`／`strengthB` 均为 6，`temporalEnable=0`，4×4 矩阵 `[15,7,13,5,3,11,1,9,12,4,14,6,0,8,2,10]`。实际 SDR 和 HDR 播放中这些后混合模块仍关闭，Linux 不额外开启；IGC 内部抖动由 7.3 的 `0016` 补丁在存在 Degamma LUT 时按强度 4 打开。


## 标准 HDR 输出

面板驱动合成 EDID 1.4、CTA HDR 数据块和 DisplayID 原生时序。这是完整 DPU／
固定内屏路径的能力描述，不是屏幕实际提供的 EDID。十位输入、P3-D65 原色、
Gamma 2.2 和亮度来自对应原厂配置；原生 120 Hz 时钟超过基础 EDID 的
655.35 MHz 上限，由 DisplayID type VII 完整描述。基础块中的低分辨率输入由
DPU 全屏缩放处理，不改变 DSI／DSC 时序。KDE 的默认显示配置同时匹配旧的无 EDID
入口和合成身份，保持 3200×2136@120、两倍缩放。

匹配的 `qcom/sm8750/piano-{boe,csot}-display.bin` 提供转换数据。
生成来源与方法见[显示校准固件](../../../vendor/firmware/piano-display/README.md)。
固件缺失、格式错误或硬件块不匹配时不附加 HDR 属性、不生成 HDR EDID，
普通 SDR 继续启动。能力属性在 DRM 设备注册前创建。

输出状态分为三种：普通 SDR 旁路、BT.2020 SDR 和 PQ HDR。后两者依次使用
IGCv5 线性化、Gamut v4.3 的 17³ 色域及面板校准、GCv2 原厂曲线，
同一批 REGDMA 命令广播到两路 DSPP。转换生效时，如果合成器同时请求
`CTM`、`GAMMA_LUT` 或 `DEGAMMA_LUT`，原子检查拒绝该组合，让合成器回退到
GPU 完成这些操作，不悄悄忽略用户空间请求。普通 SDR 的标准 LUT 和夜灯接口不变。
驱动退出或 DMA 出错继续遵守[REGDMA 的资源保留规则](gcv2-night-light.md)。

SteamOS KWin 通过标准属性完成 HDR／广色域启闭、DPMS 关屏恢复和一次
`deep` 挂起后的 RTC 唤醒。恢复前后启动 ID 一致，挂起成功计数加一，
ADSP／CDSP 保持运行，前摄采集和麦克风录音恢复。PQ 模式下两路
Gamut、IGC、GC 控制字分别读回 `0x1`、`0x10114`、`0x5`；关闭后全部旁路，
REGDMA 完成状态为 `0x8`、错误状态为零。背光在 HDR 时升到 2047；
KWin 6.6.5 的通用修正在退出 HDR 时使亮度缓存失效，
实机背光完成 `1535 → 2047 → 1535` 切换。补丁放在
`patches/kwin/6.6.5/`，不改动 HDR 能力判断或色彩处理接口。
PQ 四级图样有可见亮度差异。该验证不代表逐台峰值、黑位或色准完成测量，
也不代表十二位 FRC 已实现。BOE 的数据已按对应原厂配置生成，尚无该面板的实机结果。

[`tools/android/piano_drm_snapshot.c`](../../../tools/android/piano_drm_snapshot.c)可在 Android 和 Linux 中只读导出 DRM 属性及 LUT blob。将同一画面设置下的 SDR 与 HDR 播放状态对照，同时确认活动 CRTC、HDR 图层和电源状态；blob 存在或 DMA 完成不能单独证明物理 HDR 输出。
