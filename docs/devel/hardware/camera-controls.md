# 相机格式与控件

相机服务向应用提供前摄、后摄两个 V4L2 摄像头。普通模式使用 CAMSS 硬件图像处理器（ISP）输出 NV12；后摄全幅 RAW10 使用软件 Quad CFA 转换。服务只在应用开流时启动传感器和 ISP，关闭流后停止 DMA。自动曝光、白平衡和后摄对焦（3A）由同一服务的帧循环控制。当前还没有不读取图像数据的 libcamera TFE pipeline 和 IPA 算法模块，旧服务及其 loopback、控制 socket 仍是应用使用相机的依赖。

SM8750 TFE980 的 PIX sink 接收四种排列的 RAW10 Bayer，source 和对应捕获节点只输出 NV12。RAW10 原始采集走 RDI，不能因 PIX 接收 Bayer 就把它公布为 RAW10 捕获设备。[格式补丁](../../../patches/linux/7.3/0114-camss-pix-capture-formats.patch)分别描述 sink 和捕获格式，并让 SM8750 sink 的默认值落在支持的格式中；不再共用包含 RAW10 和 NV12 的捕获枚举。

## 内核接口与 libcamera

产品相机驱动来自 [Linux 补丁链](../../../patches/linux/7.3/series.json)，不是未经修改的主线 CAMSS。图像、控件和帧事件使用 V4L2；颜色矩阵仍有本地私有控件。TFE 的 `vfe_isp_params` 是内核内部状态，不是用户空间可以排队的参数缓冲接口。

| 部分 | 当前实现 | libcamera 硬件路径还缺什么 |
| --- | --- | --- |
| 图像队列 | `VIDEO_CAPTURE_MPLANE`、MMAP、DMABUF；PIX 输出 NV12，RDI 输出原始数据 | pipeline 配置硬件缩放、裁切和缓冲生命周期，不映射图像计算 3A |
| 3A 统计 | 驱动只列出统计 write master 编号，没有统计配置、DMA 队列、格式或完成处理 | 有来源的 TFE980 统计布局、阈值、尺寸、完成顺序及标准 metadata 节点 |
| ISP 参数 | PIX subdev 的标准 gain、白平衡、饱和度、对比度控件；CCM 为私有 `0x00981ff0` | 上游可接受的逐帧参数接口与生效帧关联，不能另造私有控件替代 |
| 请求 | 图像队列没有 Media Request 支持；控件逐项写入后触发寄存器更新 | sensor 控件延迟和 TFE 参数实际生效帧的测量；不能把写入成功当作指定帧已经生效 |
| 前摄时序 | 原生积分计数、固定模式帧间隔；不提供失实的 HBLANK/VBLANK | 可靠的像素阵列时钟及读出模型，或 libcamera 接受的非图像行曝光模型 |
| 后摄对焦 | DW9768 使用 `FOCUS_ABSOLUTE`，sensor→lens ancillary link 已确认；现有服务对 NV12 计算清晰度 | 硬件对焦统计、镜头标定及移动稳定时间 |

现有 `frame_stats()` 从 NV12 的 Y/UV 采样，转换采样值用于 AE/AWB；AF 同样读取图像清晰度。把这些函数移进 IPA 并不能满足“IPA 只处理硬件统计”的目标。驱动中的统计 write master 名称不足以推导计算模块配置、统计记录布局或 DMA 缓冲大小，不能只凭名称开始排队统计 DMA。

上游 [libcamera v0.8.0 simple pipeline](https://github.com/libcamera-org/libcamera/blob/v0.8.0/src/libcamera/pipeline/simple/simple.cpp) 对 CAMSS 使用 software ISP，选择到硬件 PIX 路径也没有 TFE 3A 参数和统计处理。2026-11-12 的上游 master 同样没有 TFE pipeline 或 IPA。[CAMSS pipeline v3 提案](https://lists.libcamera.org/pipermail/libcamera-devel/2026-November/063337.html)增加不同相机的 CSID/VFE 分配，但硬件 ISP 部分针对 QRB2210 OPE，不是 SM8750 TFE980。[OPE 实现](https://lists.libcamera.org/pipermail/libcamera-devel/2026-November/063344.html)仍用 `SwStatsCpu` 读取原始图像并复用 softISP IPA，不能作为不处理图像数据的 TFE 方案。

libcamera 的[传感器要求](https://github.com/libcamera-org/libcamera/blob/v0.8.0/Documentation/sensor_driver_requirements.rst)还要求 RAW sensor 提供 HBLANK、VBLANK、PIXEL_RATE，并把曝光表示为图像行。当前 OV32D40 不满足这一模型，不能重新填入已证明错误的 blanking 值来通过初始化。上游也没有这两颗 sensor 的增益 helper 和实测控件延迟，默认延迟不能代替逐模式测量。

OV32D40 的[设备树 binding](../../../patches/linux/7.3/0115-media-ov32d40-binding.patch)使用通用 video-device、CSI-2 endpoint schema，描述现有四 lane D-PHY、19.2 MHz MCLK、三路电源、低有效 reset 和单个可选 power domain。binding 的 YAML 与例子通过 `dt_binding_check`；修正遗留布尔属性和无效空 `reg` 后，完整折叠 DT 中的 OV32D40 节点通过定向校验。原生 Piano skeleton 不包含完整相机节点，不能代替该检查。

SM8750 的 [CAMSS binding](../../../patches/linux/7.3/0116-media-camss-bayer-clock-binding.patch)还描述每条完整 TFE 的 Bayer、Bayer fast AHB 和 Bayer CAMNOC 时钟，共 44 项时钟资源；这些是 PIX 驱动使用的硬件时钟，不能为匹配旧 schema 而删除。捕获端只描述驱动使用的 `ahb`、`hf_mnoc` 两条 interconnect，不把独立 ICP 的资源放进 CAMSS。[带宽 overlay](../../../linux/dts/piano-camera-interconnects.dtso)使用现有 SM8750 provider，补入 APPSS→CAMERA_CFG 的活动带宽路径和 CAMNOC_HF→EBI 的图像路径；runtime resume 提交驱动带宽票据，suspend 清零。两条路径与 44 clocks 在完整折叠节点上的定向 schema 检查通过，带宽投票仍需实机检查。缺少属性时 interconnect 核心返回 NULL 并跳过约束，不能据此将硬件带宽路径改为可选。

前后 sensor 的 `power-domains` 都指向 `CAM_CC_TITAN_TOP_GDSC`。公开相机 DTS 说明该引用用于保证 CAMBIST MCLK 分支工作，并非 sensor 本体位于片上电源域；I2C core 会在 probe 前附加该域，runtime PM 在 sensor 回调前后管理它。S5KJN1 schema 仍拒绝此属性，OV32D40 schema 允许它。应将 Titan 依赖放到 MCLK 供应端并验证 probe、停流和恢复后再迁移引用，不能删除引用或只放宽 sensor schema 来掩盖供应端模型缺项。

### TFE980 公开资料与 metadata

MiCode 的 [Piano camera-kernel](https://github.com/MiCode/vendor_qcom_opensource_camera-kernel/blob/piano-w-oss/drivers/cam_isp/isp_hw_mgr/isp_hw/vfe_hw/vfe17x/cam_tfe980.h)和 Qualcomm 官方 [camera-driver](https://github.com/qualcomm-linux/camera-driver/blob/camera-kernel.qclinux.0.0/camera/drivers/cam_isp/isp_hw_mgr/isp_hw/vfe_hw/vfe17x/cam_tfe980.h)都公开了 `cam_tfe980.h`，文件许可为 GPL-2.0-only。`tfe980_bus_hw_info` 描述统计 write master 10–18 的寄存器、格式和完成组；同一源码的 `cam_vfe_bus_ver3.c` 提供帧模式、stride、地址和 context 编程流程，可以作为统计 DMA 实现的依据。

| TFE980 输出 | write master | `cfg`／`image_addr` 偏移 | 传输格式 |
| --- | --- | --- | --- |
| AEC BE | 10 | `0x1700`／`0x1704` | PLAIN64 |
| AEC BHIST | 11 | `0x1800`／`0x1804` | PLAIN64 |
| Tintless BG | 12 | `0x1900`／`0x1904` | PLAIN64 |
| AWB BG | 13 | `0x1a00`／`0x1a04` | PLAIN64 |
| AWB BFW | 14 | `0x1b00`／`0x1b04` | PLAIN64 |
| AF BHIST | 15 | `0x1c00`／`0x1c04` | PLAIN64 |
| ALSC BG | 16 | `0x1d00`／`0x1d04` | PLAIN64 |
| Bayer RS | 17 | `0x1e00`／`0x1e04` | PLAIN32 |
| TMC BHIST | 18 | `0x1f00`／`0x1f04` | PLAIN64 |

这些输出同属 completion group 2，`comp_done_mask[2] = 0x70` 对应三个 context；MiCode 表还列出聚合完成中断 `mc_comp_done_mask[2] = BIT(25)`，Qualcomm 的上述版本未列出该字段。不能把 SM8650 的 group 4 完成处理直接搬过来。上述两份公开 kernel 源码中尚未找到这些统计计算模块的启用、ROI、grid/bin 数、阈值、输出 shift、tap point 配置和输出记录定义；IQ 寄存器命令由用户空间生成，再经 `CAM_ISP_IQ_BL` 和 CDM 提交。因此 PLAIN64 只说明打包宽度，不说明每个值是哪个颜色通道、有效位宽或帧，也不能据此确定 DMA 缓冲大小。TFE980 的 AWB BFW、AF BHIST、另一路 CAF 也不能直接当作旧版 BF 统计。

Linux 7.3 的 `v4l2-isp` 已合入通用 buffer/block header 和统计封装，但 header 的 `type` 明确由驱动定义，没有通用 Bayer grid、BHIST 或 BF payload。现有 [GENERIC metadata 格式](https://docs.kernel.org/userspace-api/media/v4l/metafmt-generic.html)描述 CSI-2 行数据的存储，不定义统计含义；把自定义 TFE 数据塞进去仍会制造项目私有 ABI。其他 ISP 的已合入格式也不能用来冒充 TFE 数据。

Qualcomm 的 [IFE BG 统计提案](https://lore.kernel.org/linux-media/20261006223953.3382273-21-gjorgji.rosikopulos@oss.qualcomm.com/)与 [CAMSS extensible BHIST 提案](https://lore.kernel.org/linux-media/20261005-linux-mainline-7-2-sm8650-hdk-rfc-0-v1-58-ed985aefd611@kernel.org/)是可复用的上游工作，但尚未合入，且基于不同 IFE 模块。采用前需证明 TFE980 的布局和语义相同，并沿该公共接口评审，而不是在项目里自行分配 block 或 fourcc。

下一项必要资料是带开放许可的 TFE980 统计计算寄存器和输出定义，至少覆盖 AEC BE/BHIST、AWB BG 与可用于 AF 的统计。原厂相同模式的寄存器命令、缓冲尺寸／stride、统计完成顺序和统计样本可用于交叉测量，但普通 Linux 控件枚举和空闲寄存器读回不能补出这些定义。Qualcomm 的 [libcamx-dev 打包说明](https://github.com/qualcomm-linux/pkg-camx-dev/blob/ceee1ff870a96da9ea4fc2c7ba46b952c6e034da/debian/copyright)明确区分 BSD 打包脚本与闭源 CamX 载荷，不能因下载公开就视为可导入实现或调校的开放源码。

### Media graph 与并发

当前产品为后摄配置 `s5kjn1 → msm_csiphy1 → msm_csid0 → msm_vfe0_pix → msm_vfe0_video3`，前摄使用 `ov32d40 → msm_csiphy4 → msm_csid1 → msm_vfe1_pix → msm_vfe1_video3`。CSID sink 是 pad 0，IPP 是 source pad 4；VFE sink/source 分别是 pad 0/1。设备节点编号由探测顺序决定，不是配置常量。原始采集使用 CSID source pad 1 和对应 VFE RDI0。

产品内核的 `MEDIA_IOC_G_TOPOLOGY` 已确认后摄到 DW9768 和两路 flash 的三个 enabled、immutable ancillary links，运行设备树的 `lens-focus` 和 `flash-leds` 指向相同设备。关联不是 libcamera 接入的缺项，仍缺的是算法、标定和闪光拍照流程。

内核图目前公布 CSID 与 VFE 之间的全连接，但 Gen4 的寄存器更新、帧时序与完成回调按相同硬件编号分派；不能把所有公布的跨编号链接都当作已实现的路由。pipeline 必须先约束已实现的路由，并核实是否需要收窄内核图，再验证前后摄同时工作、交错停流、模式切换和失败恢复。共享隐私灯的引用计数验证不证明两路图像、统计和控件互不干扰。

原有 7.3.3 产品内核的两个 PIX 节点各通过 48 项不含开流的 `v4l2-compliance 1.30.1` 检查，0 warning，Requests 报告 Not Supported。这只说明该基线的 ioctl 行为，不覆盖格式修正后的开流、3A、闪光同步或 libcamera。

### 调校来源与全幅模式

相机源码的 MIT 许可不自动覆盖外部调校数据。现有 `cam_cfgs[].ccm` 注释写明来自原厂调校矩阵均值，但没有独立来源和再分发许可记录；TFE 寄存器编程的 CamX 来源也需要逐项确认公开来源与许可。libcamera 的新调校必须使用许可清楚的公开数据或重新测量的数据，不能导入 OEM tuning、私有 Chromatix 或把现有常量换个 YAML 文件名继续使用。

后摄 8160×6120 输出的是 Quad CFA，现有 TFE 配置只选择普通 2×2 Bayer 顺序、普通 demosaic，并关闭 BDS2，没有实现全幅 remosaic。该源码能说明当前路径不支持此模式，不能证明芯片永远没有相应能力，也不能仅凭注释中的 7008 像素就声称硬件宽度上限。硬件路径应先使用后摄 4080×3060、前摄 3264×2448 的 binned 模式；全幅需要单独的公开硬件资料和验证，不增加软件 debayer、缩放或颜色转换回退，也不把 binned 图像放大后标为全幅。旧服务的软件全幅路径在有可运行替代前保留，不记为 TFE 全幅适配。

## 分辨率和帧率

默认输出为 1920×1440、30 fps。应用通过标准 `VIDIOC_S_FMT` 和 `VIDIOC_S_PARM` 选择尺寸和帧率，不需要修改服务配置。

| 相机 | 传感器模式 | 传感器标称帧率 |
| --- | --- | --- |
| 后摄 S5KJN1 | 4080×3060、4080×2296 | 30 fps |
| 后摄 S5KJN1 | 3840×2160 | 60 fps |
| 后摄 S5KJN1 | 8160×6120 | 10 fps |
| 前摄 OV32D40 | 3264×2448、3264×1836 | 30 fps |
| 前摄 OV32D40 | 6528×4896 | 10 fps |

后摄 8160×6120 保留全幅 Quad CFA 样本，经 RDI 采集、QPPG 转换和软件颜色处理输出 NV12。传感器的 10 fps 不等于软件输出速率：当前平板约 2.4 秒生成一张全幅图像，约 0.4 fps。工作内存约 953 MiB；转换线程处理期间丢弃新输入，不排队积压旧帧。后摄新模式使用同机原厂静态 ODM 寄存器表；来源记录在内核补丁中。19.2 MHz 输入时钟、857.6 MHz CSI 链路由共享设备树配置，旧 24 MHz 传感器表仍保留给使用该时钟的设备树。

输出可以小于传感器模式。宽度按 16 像素、高度按 2 像素对齐；普通模式的 ISP 先等比缩放，再居中裁切不同宽高比，不拉伸画面。后摄全幅先完成 Quad CFA 转换，再由 libyuv 居中裁切、缩放；不会放大较小的传感器模式冒充全幅。极小输出受 ISP 最大 16 倍缩小限制，会进一步裁切视野。60 fps 只公布给后摄 3840×2160 以内的输出，前后摄全尺寸模式均只公布 10 fps。前摄三张寄存器表与同机原厂静态配置逐条一致；驱动通过现代 pad-level 帧间隔接口报告固定模式的原厂标称 10/30 fps，服务开流前读回并核对。该数值不是硬件帧计数器的测量结果。

原始 v4l2loopback 在生产者设置格式后只向读取者公布该格式。共享的
[v4l2loopback 补丁](../../../patches/v4l2loopback/0.15.4/series.json) 与
[相机源码](../../../linux/userspace/camera/piano-camerad.c) 增加同一设备上的生产者协商：

1. 首帧写入前，服务依次设置各模式的最大尺寸、帧间隔，并订阅 `SOURCE_CHANGE`，登记该生产者支持的组合。
2. 应用使用标准尺寸、帧间隔枚举和 `S_FMT` / `S_PARM`。驱动通过 `SOURCE_CHANGE` 通知服务，并等待生产者确认。
3. 服务停止 CAMSS，停止 OUTPUT 队列并释放旧映射、缓冲，接受尺寸和帧率后重建 MMAP 队列，通过 `QBUF` 发布完整黑帧。驱动在尺寸、帧间隔和整帧都匹配后完成应用的协商；应用真正 `STREAMON` 后仍需等新传感器帧，不能读走协商用的黑帧。
4. 原有应用使用计数事件仍负责实际开流和停流；原始传感器返回的尺寸贯穿 PHY、CSID 和 ISP 配置。

这是本项目对 loopback 的扩展，不是原始驱动已有的动态尺寸功能。未订阅的生产者保持原有行为。读取端仍有映射、正在推流，或其他读取端占有格式时会阻止切换。生产者自己的输出映射允许进入协商，但必须全部释放后才能真正重分配；停止失败时保留采集缓冲和进程，等待应用超时，不释放可能仍用于 DMA 的映射。没有增加虚拟摄像头、格式选择按钮或额外服务。

后摄全幅 RAW10 的黑电平采用原厂静态描述中的 64，白电平为 1023。转换先计算 `(sample−64)/(1023−64)`，再应用已有数字增益、白平衡、颜色矩阵和色调曲线。这里使用静态黑电平，不包含动态暗电平或镜头阴影校正。普通 PIX 模式仍使用内核 TFE 黑电平配置。

## 手动控件

正常 runtime 构建同时安装相机服务和 `piano-camera-ctl`。客户端复用服务已有的硬件连接；未开流时返回 `ENODEV`，不会暗中启动相机。

```sh
piano-camera-ctl caps rear
piano-camera-ctl get rear
piano-camera-ctl set rear ae manual
piano-camera-ctl set rear exposure-time-ns 500000
piano-camera-ctl set rear ae auto
```

`rear` 可换为 `front`，但前摄没有对焦马达。`caps` 返回当前模式的最小值、最大值、步长、默认值和标志；应用换模式后应重新查询。

| 控件 | 单位 | 硬件接口 |
| --- | --- | --- |
| exposure | 原生积分计数，后摄以行为单位 | `V4L2_CID_EXPOSURE` |
| exposure-time-ns | 纳秒，按合法曝光行数取整 | 同一传感器曝光控件 |
| analog-gain | 驱动原生整数 | `V4L2_CID_ANALOGUE_GAIN` |
| digital-gain | Q10，1024 为 1× | TFE `V4L2_CID_DIGITAL_GAIN` |
| red-balance / blue-balance | Q10 | TFE 红、蓝平衡控件 |
| focus | 驱动原生 DAC 值 | 后摄 `V4L2_CID_FOCUS_ABSOLUTE` |

AE、AWB、AF 分别保存自动或手动状态。数值写入要求先把对应域切为 `manual`，避免自动算法覆盖；恢复 `auto` 从当前控件状态继续。每次只写一个控件并读回，失败保留 errno；多个控件不构成硬件原子事务。回复丢失时先执行 `get`，再决定是否重试。

后摄曝光行时间由驱动的 VT `PIXEL_RATE`、图像宽度和 `HBLANK` 计算，帧长通过 `VBLANK` 设置。原厂 19.2 MHz 表的 VT 时钟为 `19200000 × 131 × 4 / (3 × 6)` Hz，与 CSI 传输率分别处理；不把 857.6 MHz 链路吞吐当成积分时钟。

前摄保留原生积分计数。原厂程序不按分辨率缩放曝光计数，直接写入 `0x3500..0x3502`；6528×4896 的原厂 FLL 为 2551，不能把它当成 4896 行图像做减法。驱动删除失实的 HBLANK/VBLANK 控件，模式表固定 FLL，并将曝光上限设为 FLL−12，避免旧 blanking 值覆盖新模式时序。CSI 推导的 `PIXEL_RATE` 仅沿用作现有 CAMSS 时钟票据，不用于积分时间换算。

前摄时间接口使用原厂标称帧间隔除以原生 FLL，与 CamX 的积分时间关系一致。3264×2448、3264×1836 的计数周期为 `1e9 / 84600` 纳秒；500000 纳秒取最近合法的 42 个周期，约 496454 纳秒。全尺寸周期为 `1e9 / 25510` 纳秒，相同请求取 13 个周期，约 509604 纳秒。它们保持原厂时间单位，不声称解决了像素阵列时钟或并行读出比例。没有校准依据的 ISO、Kelvin、物理焦距或对焦距离换算不对外提供。

控制 socket 为 `/run/piano-camerad/control.sock`，权限 `root:video 0660`。它没有额外守护进程，也不替代应用的 V4L2 格式协商。

## 帧时间戳与错误

完整 SM8750 CAMSS 的 IPP 和 RDI0 使用 CSID980 锁存的物理计数器，通过标准 V4L2 接口报告帧时间。

| 接口 | 时间含义 | 同一帧的编号 |
| --- | --- | --- |
| `V4L2_EVENT_FRAME_SYNC` | `timestamp` 是 CSI SOF，时钟为 `CLOCK_MONOTONIC` | `u.frame_sync.frame_sequence` |
| `VIDIOC_DQBUF` | `timestamp` 是 CSI EOF，标志为 `TIMESTAMP_MONOTONIC` 和 `TSTAMP_SRC_EOF` | `sequence` |

应用按表中的帧编号配对；`event.sequence` 是事件队列的序号，不能用来关联图像。DMA 完成、用户态出队和 ISP 转换均可能晚于 EOF。`DQBUF` 的 `timeval` 将纳秒截断到微秒，保守覆盖 EOF 时取该微秒区间的上界，即换算为纳秒后加 999。

中断内依次读取物理计数器、`ktime_get_ns()`、物理计数器，以两次计数器读数的中点换算锁存值。驱动要求 ARM64 `CNTFRQ_EL0` 为 19.2 MHz，否则拒绝换算；采样区间超过 50 微秒或锁存值早于采样超过一秒时也拒绝换算。在 `CLOCK_MONOTONIC` 总瞬时速率与计数器标称速率相差不超过 ±500 ppm 的条件下，误差保守上界为 550 微秒。

CSI SOF 是帧包开始，积分可能更早。内核不减去曝光时间，也不标记 `TSTAMP_SRC_SOE`；拍照仍须结合传感器积分模型。SOF 到 EOF 的实际跨度也不能简单替换成有效行数乘行时间。

libcamera `SensorTimestamp` 的语义是首行开始曝光、时钟为 `CLOCK_BOOTTIME`。这里的 CSI SOF 和 EOF 都不能直接填入该字段：还需要传感器积分相对 CSI 的时序和 MONOTONIC→BOOTTIME 换算，并在睡眠恢复后重新核对。现有 simple pipeline 直接使用捕获时间戳的行为不是 TFE 时间戳语义已经适配。

SOF/EOF 按既有双缓冲 FIFO 关联。标记缺失、不连续、接收端错误或无效换算会使该流持续返回 `V4L2_BUF_FLAG_ERROR`，直到重新开流。TFE 图像尺寸、CCIF 和约束错误另走 BUS 中断；处理错误与完成缓冲共用 `output_lock`，先检查错误再发布图像。只有活动的 write master 允许访问 BUS 寄存器，停止最后一条流时在关时钟前屏蔽、清除并读回中断配置。

实现见[物理帧时序补丁](../../../patches/linux/7.3/0048-camss-frame-timestamps.patch)和 [TFE BUS 修复](../../../patches/linux/7.3/0049-camss-tfe-bus-errors.patch)。`camss_frame`、`camss_frame_error` trace 提供原始计数器、交叉采样和缓冲编号。

产品内核不提供 ISP 寄存器脚本或任意 MMIO 读写入口。TFE 默认设置、缩放和图像控件由驱动编程；诊断使用现有帧 trace 和错误日志。[删除补丁](../../../patches/linux/7.3/0113-camss-remove-register-debug.patch)移除 `camss_isp` debugfs 及其共用的 CSID/VFE 指针，避免两个摄像头互相覆盖调试目标，不改变开流、停流或 DMA 清理。

## 闪光灯

PMIC 闪光灯使用标准 V4L2 flash 控件，后摄设备树通过 `flash-leds` 引用两路灯，到两颗 flash subdev 的 `MEDIA_LNK_FL_ANCILLARY_LINK` 已确认。现有服务从这些链接查找闪光灯，复用控制 socket 接受一次强制闪光拍照请求：

```sh
piano-camera-ctl capture rear --flash --output photo.jpg
```

程序冻结当前曝光、白平衡和对焦，等待两个真正开始于冻结之后的传感器帧，再开启有硬件超时的闪光。物理 `FRAME_SYNC` 事件与同序号捕获缓冲提供 SOF 和 EOF；只有完整曝光区间落在保守亮灯窗口内的帧才交付。取消、超时、格式切换或传感器错误均关闭闪光、恢复原控制状态，不返回其他帧。

照片尺寸沿用当前协商的 PIX 输出，不会自动切换成原生全幅。全幅 RDI 和前摄闪光请求返回 `EOPNOTSUPP`。该路径提供长脉冲曝光覆盖，不包含自动预闪测光或短脉冲冻结运动；普通视频应用也不会自动发出这条拍照请求。JPEG 由调用者接收只读封存的图像文件描述符后编码，服务不接收用户输出路径。

libcamera v0.8.0 的通用 sensor 类只识别关联镜头，没有现成的 TFE 闪光拍照流程。未来接入仍须保留硬件超时、故障读回、取消和完整曝光覆盖条件，先完成标准 kernel 接口与 libcamera 控件设计，不能继续用项目 socket 冒充 libcamera 支持。现有闪光流程的源码不等于已经完成光学发光与曝光同步测量。

隐私指示灯的驱动与 PWM 联动见[实现说明](camera-privacy-led.md)。它与拍照闪光灯用途不同。
