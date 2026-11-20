# 相机格式与控件

相机服务向应用提供前摄、后摄两个 V4L2 摄像头。普通模式使用 CAMSS 硬件图像处理器（ISP）输出 NV12；后摄全幅 RAW10 使用软件 Quad CFA 转换。服务只在应用开流时启动传感器和 ISP，关闭流后停止 DMA。自动曝光、白平衡和后摄对焦（3A）由同一服务的帧循环控制。当前还没有不读取图像数据的 libcamera TFE pipeline 和 IPA 算法模块，旧服务及其 loopback、控制 socket 仍是应用使用相机的依赖。

SM8750 TFE980 的 PIX sink 接收四种排列的 RAW10 Bayer，source 和对应捕获节点只输出 NV12。RAW10 原始采集走 RDI，不能因 PIX 接收 Bayer 就把它公布为 RAW10 捕获设备。[格式补丁](../../../patches/linux/7.3/0114-camss-pix-capture-formats.patch)分别描述 sink 和捕获格式，并让 SM8750 sink 的默认值落在支持的格式中；不再共用包含 RAW10 和 NV12 的捕获枚举。

NV12 捕获和裁切使用相同的尺寸步长：宽度为 16 的倍数，高度和裁切起点按完整色度样本对齐。缩放下限按 16 倍比例向上取偶数；后摄输入 4080×3060 时，平板读回的最小缩放尺寸为 256×192。DMA 基址和 PIX 行跨度按 TFE980 的 256 字节地址单位校验。

## 内核接口与 libcamera

产品相机驱动来自 [Linux 补丁链](../../../patches/linux/7.3/series.json)，不是未经修改的主线 CAMSS。图像、控件和帧事件使用 V4L2；颜色矩阵仍有本地私有控件。TFE 的 `vfe_isp_params` 是内核内部状态，不是用户空间可以排队的参数缓冲接口。

CAMSS 每个图像平面只向硬件写一个 DMA 基址。[缓冲区校验](../../../patches/linux/7.3/0135-media-camss-validate-capture-dma-ranges.patch)在写入前确认映射后的连续地址覆盖协商的 `sizeimage`。连续范围可以由相邻的多个 DMA 段组成，不要求整个缓冲区都连续；范围不足时，Videobuf2 解除映射，缓冲区不会交给硬件。
SM8750 在传感器和接收链路启动成功后才提交捕获缓冲，再在帧边界开放 CSID
端口。启动中途报错时，本次缓冲仍由 Videobuf2 持有。停流时等待 IRQ 完成
只保证软件回调已经退出，已经发出的总线写入仍需独立的硬件完成证明。
在平板上直连前后摄捕获节点，两路 1920×1440 NV12 均完成 MMAP 和 DMA-BUF 采集及停流重开；后摄 4080×3060 RAW10 也完成采集和重开。

### 现有服务与标准替代

现有服务仍提供应用所需的相机功能。libcamera 原型已能直连硬件取得图像，还不能替代服务提供的控件和拍照流程。

Debian 的 GNOME 和 KDE 使用同一组相机包。先构建 libcamera，再让 PipeWire
和 Plasma Camera 链接这一版库；发布清单核对它们使用的归档版本及 SHA-256。
用只启用 Debian 官方签名源的 ARM64 sid 构建根可单独重建：

```sh
sudo ./build.sh libcamera --sysroot "$SID_ARM64_ROOT" --output build/libcamera
sudo ./build.sh pipewire --rootfs "$SID_ARM64_ROOT" --libcamera-packages build/libcamera \
  --output build/pipewire-libcamera
sudo ./build.sh plasma-camera --sysroot "$SID_ARM64_ROOT" --libcamera-packages build/libcamera
```

构建按 [固定的 libcamera 版本](../../../config/libcamera.json)和
[CAMSS 补丁顺序](../../../patches/libcamera/0.8.0/series.json)重放源代码，
沿用 Debian 拆包和 IPA 签名。七个 ARM64 `.deb` 在输出的 `runtime/`，
源码包和 Debian 构建记录在 `source/`。PipeWire 使用该组开发包编译
`libspa-0.2-libcamera`，并核对插件实际链接的库。

打包本身不替换现有相机服务。0.8 的 IPA 模块、签名和代理由
`libcamera-ipa-0.8` 安装到独立目录，不覆盖 Debian 0.7 的 `libcamera-ipa`。
成品明确安装同版 IPA 和标准相机插件，KDE 也安装重新链接的 Plasma Camera。

Arch Linux ARM 用同一版 libcamera 源码和 CAMSS 补丁，沿用官方 `PKGBUILD`
的六个拆包。在没有安装旧相机覆盖包的隔离 ARM64 构建根，先构建并安装
libcamera，再构建 PipeWire 和 Plasma Camera。归档在各自输出目录的 `runtime/`：

```sh
sudo ./build.sh libcamera --rootfs "$ARCH_ARM64_ROOT" --output build/libcamera-arch
sudo ./build.sh pipewire --rootfs "$ARCH_ARM64_ROOT" \
  --libcamera-packages build/libcamera-arch --output build/pipewire-arch
sudo ./build.sh plasma-camera --rootfs "$ARCH_ARM64_ROOT" \
  --libcamera-packages build/libcamera-arch --output build/plasma-camera-arch
```

库和 IPA 要来自同一次构建，工具、GStreamer 和 Python 包也要求匹配的库。
PipeWire 保留 Arch 的拆包和音频依赖；Plasma Camera 使用构建根自带的 Qt
和标准 FFmpeg 后端。从官方版本切换时，libcamera、PipeWire 和相机应用
应由 pacman 一起升级或回退。成品构建从签名源安装同一批包；Plasma Camera
默认加入 SteamOS 镜像，Arch 用户可以按需安装。同一 `libcamera --rootfs`
入口也接受 SteamOS ARM64 构建根，
生成匹配 Valve Qt 和 GStreamer 的五个包，并保留 CAMSS、UVC 和软件 ISP；
它不安装 Arch 的 `libpisp` 或文档包。
用该组 libcamera 和 IPA 在同一个 Valve 构建根运行
`plasma-camera --rootfs`，可构建使用原有 Qt Multimedia／FFmpeg 后端的
SteamOS 应用包。这个包不替换系统的 Qt 或 FFmpeg。
成品仍保留现有相机服务及 WirePlumber 规则；安装原生包不会自动切换到真实摄像头。

| 功能 | 现有服务（旧路径） | 标准硬件路径（替代状态） |
| --- | --- | --- |
| 前后摄预览、录像 | CAMSS PIX 输出 NV12，服务通过两个 loopback 摄像头交给应用 | Debian 普通会话已通过正式 libcamera 和 PipeWire 从两个实际摄像头取帧；仍需补硬件统计、逐帧控制及拍照录像验收 |
| 分辨率、帧率协商 | 服务处理 loopback 的 `S_FMT`／`S_PARM`，配置 sensor、硬件缩放与裁切 | Debian 装包后，普通用户通过 GStreamer 从前后摄取得 640×480、1280×720、1920×1080 和 1024×600 NV12；后摄原生 3840×2160／60 fps 的帧序连续，停流后可切回 720p |
| 自动曝光、白平衡、后摄对焦 | 服务从 NV12 采样计算 AE／AWB 和清晰度 | TFE980 统计配置、metadata DMA 队列、完成处理和只处理硬件统计的 IPA 未实现，资料缺项见[下节](#tfe980-公开资料与-metadata) |
| 手动控件、逐帧参数 | 控制 socket 写标准 sensor／PIX 控件；CCM 使用私有控件 | 底层曝光、gain、白平衡等控件保留；标准逐帧 ISP 参数、生效帧关联和 libcamera 控件接入未实现 |
| 镜头、闪光拍照 | 镜头及两路 flash ancillary links 已确认；服务提供强制闪光拍照流程 | 标准镜头／flash 控件和关联已具备；硬件 AF 统计、镜头标定、移动稳定时间及 libcamera 闪光流程未实现 |
| 后摄全幅 | RDI 采集 Quad CFA，经已有软件转换输出 NV12 | 当前 TFE 没有实现全幅 remosaic；硬件路径先使用 binned 模式，不增加软件回退或放大冒充全幅 |

libcamera 从 NV12 捕获节点读取尺寸范围，先保留传感器的原生模式，也公布
应用常用的缩放尺寸。缩放和裁切都由 TFE 完成：传感器保持原生宽高比，
缩小后再居中裁切，不拉伸画面。输出尺寸、行跨度和缓冲区大小以捕获节点
读回为准。

libcamera 在传感器模式确定后公布 `FrameDurationLimits`。后摄开流前设置并
读回 V4L2 的垂直消隐；前摄采用驱动报告的固定帧间隔。GStreamer 的
`libcamerasrc` 从应用要求和相机能力的交集选择帧率，没有指定时用模式默认值。

Debian 平板从签名 APT 升级后，系统 GStreamer 插件实测后摄 4K、720p 均可在
30 和 60 fps 连续出帧。前摄 720p 只支持 30 fps：给出 60 和 30 两个备选档
会选 30，只接受 60 时会拒绝，随后可重新开流。Plasma Camera 录制 4K30 时，
后摄以 30 fps 采集，不再从 60 fps 抽帧。

PipeWire 的 libcamera 插件在选定模式后，把应用请求的固定帧率交给相机，
并向后来的应用公布当前格式。Debian 从签名 APT 装包后，后摄 4K 和 720p
均按 30／60 fps 连续采集，前摄拒绝固定 60 后可重新以 30 打开。
两个应用能共用后摄 60 fps；这时再请求 30 的应用会收到格式错误，
原采集仍以 60 fps 连续交付，全部退出后可重新打开 30 fps。
尺寸和像素格式的严格请求也会拒绝不支持的组合；允许就近匹配时，
PipeWire 回报 libcamera 实际配置的格式，并从同一条流取缓冲。
显式请求 DMA-BUF 布局时，只接受相机实际配置的布局。不指定布局的应用
仍可共用线性缓冲；不匹配的请求不会重设正在采集的流。
查询可用模式时，首选尺寸只列一次，分页和过滤不改变各模式的索引。
没有固定帧率请求时沿用模式默认值和真实帧时间。相机还不能在模式确定前
逐项公布可用帧率，因此第一次协商多个备选档时仍可能需要重新选择。

因此不能先移除 `piano-camerad`、loopback 或控制 socket：否则应用会失去已处理的视频输出、3A 和现有拍照入口。WirePlumber 配置同时隐藏服务占用的 CAMSS 节点并禁用 libcamera monitor，避免两个路径争抢 sensor；标准替代完成并验证这些功能前，该配置仍需保留。

`cam` 同时采集两路时，先配置并启动所有相机，再提交首批请求。
平板后摄 4K60、前摄 3264×2448／30 fps 均用默认四个缓冲，从首个已采集帧起序号连续，
中断后可重新打开。第一路启动后还没有提交缓冲的物理帧不交付给应用。
独立应用的情况不同：平板上用两个 PipeWire 客户端实测，在后摄 4K60
采集中打开前摄，前摄的同步开流占住 libcamera 管理线程，后摄漏了
12 帧，最长间隔约 217 ms；缓冲归还后恢复原帧率。

图像队列不支持 Media Request。后摄的曝光行数和模拟增益可通过一次
`VIDIOC_S_EXT_CTRLS` 成组提交：驱动按同机原厂的 `0x0104` 顺序保持、
写入并释放曝光寄存器。也可单独设置其中一项。缩短帧长时先调整超出新
范围的曝光；写入帧长失败后保留原曝光范围，不盲目写回较长的曝光。
这只确定寄存器写入方式，相机帧上的生效时序仍需测量；TFE 参数也还没有
与帧绑定。

现有 `frame_stats()` 从 NV12 的 Y/UV 采样，转换采样值用于 AE/AWB；AF 同样读取图像清晰度。把这些函数移进 IPA 并不能满足“IPA 只处理硬件统计”的目标。驱动中的统计 write master 名称不足以推导计算模块配置、统计记录布局或 DMA 缓冲大小，不能只凭名称开始排队统计 DMA。

上游 [libcamera v0.8.0 simple pipeline](https://github.com/libcamera-org/libcamera/blob/v0.8.0/src/libcamera/pipeline/simple/simple.cpp) 对 CAMSS 使用 software ISP，选择到硬件 PIX 路径也没有 TFE 3A 参数和统计处理；[上游 master](https://gitlab.freedesktop.org/camera/libcamera/-/tree/da6a6ed33ed0ab305f10b6f8b44736150d39cef4/src/libcamera/pipeline)同样没有 TFE pipeline 或 IPA。[CAMSS pipeline v3 提案](https://lists.libcamera.org/pipermail/libcamera-devel/2026-November/063337.html)增加不同相机的 CSID/VFE 分配，但硬件 ISP 部分针对 QRB2210 OPE，不是 SM8750 TFE980。[OPE 实现](https://lists.libcamera.org/pipermail/libcamera-devel/2026-November/063344.html)仍用 `SwStatsCpu` 读取原始图像并复用 softISP IPA，不能作为不处理图像数据的 TFE 方案。

libcamera 的[传感器要求](https://github.com/libcamera-org/libcamera/blob/v0.8.0/Documentation/sensor_driver_requirements.rst)还要求 RAW sensor 提供 HBLANK、VBLANK、PIXEL_RATE，并把曝光表示为图像行。当前 OV32D40 不满足这一模型，不能重新填入已证明错误的 blanking 值来通过初始化。上游也没有这两颗 sensor 的增益 helper 和实测控件延迟，默认延迟不能代替逐模式测量。

OV32D40 的[设备树 binding](../../../patches/linux/7.3/0115-media-ov32d40-binding.patch)使用通用 video-device、CSI-2 endpoint schema，描述现有四 lane D-PHY、19.2 MHz MCLK、三路电源、低有效 reset 和单个可选 power domain。binding 的 YAML 与例子通过 `dt_binding_check`；修正遗留布尔属性和无效空 `reg` 后，完整折叠 DT 中的 OV32D40 节点通过定向校验。原生 Piano skeleton 不包含完整相机节点，不能代替该检查。

SM8750 的 [CAMSS binding](../../../patches/linux/7.3/0116-media-camss-bayer-clock-binding.patch)还描述每条完整 TFE 的 Bayer、Bayer fast AHB 和 Bayer CAMNOC 时钟，共 44 项时钟资源；这些是 PIX 驱动使用的硬件时钟，不能为匹配旧 schema 而删除。捕获端只描述驱动使用的 `ahb`、`hf_mnoc` 两条 interconnect，不把独立 ICP 的资源放进 CAMSS。[带宽 overlay](../../../linux/dts/piano-camera-interconnects.dtso)使用现有 SM8750 provider，补入 APPSS→CAMERA_CFG 的活动带宽路径和 CAMNOC_HF→EBI 的图像路径；runtime resume 提交驱动带宽票据，suspend 清零。两条路径与 44 clocks 在完整折叠节点上的定向 schema 检查通过，带宽投票仍需实机检查。缺少属性时 interconnect 核心返回 NULL 并跳过约束，不能据此将硬件带宽路径改为可选。

完整 TFE980 的处理和片上总线时钟按 D-PHY 链路频率、活动 lane 数和输入像素位宽选档；
缺少链路信息时保留原像素率估算，其他 VFE 不变。它不要求传感器用 CSI 吞吐冒充
阵列像素率。平板后摄 4K60 单开时读到总线选 400 MHz（旧驱动为 300 MHz），
TFE 保持 360.28 MHz。前后摄并发采集也通过；这不等同于长时间有声录像验收。

前后 sensor 的 `power-domains` 都指向 `CAM_CC_TITAN_TOP_GDSC`。公开相机 DTS 说明该引用用于保证 CAMBIST MCLK 分支工作，并非 sensor 本体位于片上电源域；I2C core 会在 probe 前附加该域，runtime PM 在 sensor 回调前后管理它。[sensor overlay](../../../linux/dts/piano-camera-sensors.dtso)也将 Titan 依赖描述在 MCLK 供应端：CAMBIST 在原有 MMCX、MX 之外附加 Titan，通用 clock runtime PM 按 prepare/unprepare 持有、释放这些域。对应 clock binding、例子和节点校验通过，但供应端独立工作仍需实机检查；验证 probe、停流和恢复前保留 sensor 引用。S5KJN1 schema 仍拒绝该旧引用，OV32D40 schema 允许它，不通过放宽前者掩盖迁移尚未完成。

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

这些输出同属 completion group 2，`comp_done_mask[2] = 0x70` 对应三个 context；MiCode 表还列出聚合完成中断 `mc_comp_done_mask[2] = BIT(25)`，Qualcomm 的上述版本未列出该字段。用于对焦的 CAF 走独立 write master 22，`cfg`／`image_addr` 为 `0x2300`／`0x2304`，completion group 4 的完成位为 `0x100`。两组完成事件需要分别处理，不能把 SM8650 的统计中断处理原样搬过来。

上述两份公开 kernel 源码没有统计计算模块的完整配置和输出记录定义；IQ 寄存器命令由用户空间生成，再经 `CAM_ISP_IQ_BL` 和命令调度器 CDM 提交。PLAIN64 只描述传输打包，不确定统计含义或分配长度。统计 write master 的 frame-based 模式将宽高设为零，公开代码没有用分配长度设置写入上限；驱动必须先限制计算模块的输出，才能保证 DMA 不越过缓冲。

#### 原厂统计布局与长度

以下结论来自 `OS3.0.309.0.WPYCNXM` 的 CamX 前端、平台、核心、CAF 解析器及缓冲管理库的静态分析，用于独立实现硬件接口。未执行或打包这些库，也不导入其算法和调校。还没有采集同机原厂的统计 DMA 数据和最终分配记录，表中长度不作为全部硬件模式已经验证的依据。

| 统计输出 | 原厂解析布局 | 长度依据与缺项 |
| --- | --- | --- |
| BG／BE | 每区域 80 字节，小端；R、Gr、Gb、B、Y 各有普通与饱和累加记录。每项占 64 位，sum 为低 34 位，count 为高 16 位 | 原厂 AEC BE、AWB BG、Tintless BG、ALSC BG 共用 64×48 的配置上限，完整网格为 245,760 字节；Blob 分配容量是 327,680 字节。仍缺采样语义、双 TFE 分割及实机数据核对 |
| AWB BFW | 每区域也是 80 字节颜色统计，包含 ROI 编号，最多 16 个区域 | 16 个完整区域为 1,280 字节；原厂格式 23 使用每行 10 个 64-bit word、共 16 行。它不是对焦清晰度统计 |
| CAF 普通网格 | 每个滤波器 16 字节：sum 为首个 64 位字的 bits 0–36，count 为 bits 39–61；sharpness 为第二个字的低 40 位 | 滤波器数和网格上限由同一个配置模式决定，见下表；前端调整后的计数与有效数据仍缺实机核对 |
| BHIST | 原厂解析器逐项读取小端 `u32`，取低 25 位；有效项数来自该帧配置 | 三个引擎的单通道模式均为 1,024 项，即 4,096 字节；AEC 另有 R、G、B、Y 四通道模式，每通道 512 项，共 8,192 字节。三者的 Blob 分配容量均为 8,192 字节；采样和分桶语义仍需核对 |

BG／BE 的配置检查先拒绝零区域数、超过 64×48 的网格及越出输入范围的 ROI，再按每区域 16–512 像素的偶数宽高调整 ROI 和区域数。最终区域数使用 6 位减一编码写入寄存器；位宽可表示 64×64，不意味着原厂配置允许这一网格。驱动应分别限制配置、计算有效载荷和分配缓冲，不能从分配容量反推最大网格。

BHIST 的通道选择、有效项数和初始化命令数也是不同概念。原厂构造参数 `(2048, 4)`／`(1024, 1)` 中，第二项用于计算 DMI（片上统计存储访问）初始化命令所需的空间，不是输出通道数。单通道模式选择 R、B、G 或 Y；只有 AEC 接受四通道模式，解析后按 R、G、B、Y 四段交给算法。AF、TMC 拒绝该模式。ROI 会按 2×2 单元对齐，还存在多区域权重、绿色采样选择和非均匀分桶配置；这些行为未核对完整，不能把同为 1,024 项的输出直接解释成公共接口要求的 Bayer-quad luma 计数。

CAF 的原厂硬件能力沿 MCTFE metadata 传入 AF，再选择格式 tag 2 的解析器。tag 只确定记录版本，不能代替已经应用的滤波模式。普通网格的配置函数与解析器使用同一组区域计数：

| 普通网格模式 | 滤波器顺序 | 每区域字节数 | 最大网格 | 最大有效载荷 |
| --- | --- | --- | --- | --- |
| 六组滤波 | H11、V1、H12、V2、H21、H22 | 96 | 32×24 | 73,728 字节 |
| 两组滤波 | H11、V1 | 32 | 48×48 | 73,728 字节 |

原厂 TFE980 的能力描述选择 HAF 配置与格式 tag 2 的解析路径。该路径使用上述普通网格，不读取另一条 BAF 配置路径的 `windowType`。库中的 fixed／floating／flexible 分支操作的是不同布局的 BAF 对象；其中的 48×48 字段不能和 HAF 六组滤波的 96 字节记录拼在一起，也不能据此断言它们会在同一个 CAF 缓冲末尾追加数据。驱动仍须独立限制实际启用的 HAF ROI 类型、最终网格和输出长度；73,728 字节只对应表中的普通网格模式。BG／BFW 的颜色累加值不能替代 CAF 的 sharpness。

普通网格的滤波模式从 AF 配置传入前端，最终写入 HAF top 寄存器 `0x9370` 的 bit 5；两组区域计数在转换后写入 `0x9650`／`0x9658` 的 bits 16–21，使用减一编码。前端还会按裁切、采样和区域最小尺寸调整网格，因此 metadata 必须附带该帧应用的计数和模式，不能只返回请求值。ROI 类型与滤波模式是两个字段，不能将 floating／flexible ROI 的检查分支当作六组滤波模式的总长度证明。

原厂 SM8750 的静态 `UsecaseZSL` 图中，BG、BHIST 和 CAF 链接使用 Blob 格式 6；MCTFE 的 `FinalizeBufferDimensions()` 用硬件能力覆盖初始尺寸。普通 Blob 按 `width×height` 字节计算，再向上对齐到 4,096 字节；色彩空间 `0x1000` 有单独的 16 倍分支。内部非 HAL 端口初始色彩空间是 `0x101`，实时协商的默认值映射为 `0x201`，两者都不走该分支。

格式初始化清零显式总长度和 plane-size override 的计数，再由 `GetTotalSize()` 计算单帧长度。缓冲管理器保留这份格式；非共享池分配将其长度乘以单独的批量帧数，传给 `CSLAlloc()`。因此在上述普通 Blob 路径下，BG／BE、BHIST 和 CAF 的单帧分配分别为 327,680、8,192 和 73,728 字节，均已满足页对齐。链接里的初始 `0x4000`／`0xc00` 不是最终分配长度；共享池、实际批量帧数与运行图仍需从原厂采集。计算模块的输出上限和统计生效帧也需要独立验证，不能仅凭分配足够大就启用 DMA。

Linux 7.3 的 `v4l2-isp` 已合入通用 buffer/block header 和统计封装，但 header 的 `type` 明确由驱动定义，没有通用 Bayer grid、BHIST 或 BF payload。[Linux 主线](https://github.com/torvalds/linux/tree/aacb7797504183a90d66b19b1efb555cfe9b3eec/drivers/media/platform/qcom/camss)和 [media next](https://git.linuxtv.org/media.git/tree/drivers/media/platform/qcom/camss?id=bf6c63ecc20a54aaac7a2fcbd6adaf6498388825)仍无 TFE980 统计后端或 payload 定义。现有 [GENERIC metadata 格式](https://docs.kernel.org/userspace-api/media/v4l/metafmt-generic.html)描述 CSI-2 行数据的存储，不定义统计含义；把自定义 TFE 数据塞进去仍会制造项目私有 ABI。其他 ISP 的已合入格式也不能用来冒充 TFE 数据。

Qualcomm 的 [IFE BG 统计提案](https://lore.kernel.org/linux-media/20261006223953.3382273-21-gjorgji.rosikopulos@oss.qualcomm.com/)使用 64×48 网格、四颜色 `u32` 累加值，不适合直接承载 TFE980 的五通道 34-bit sum；不能截断数据去套用该结构。[CAMSS extensible BHIST 提案](https://lore.kernel.org/linux-media/20261005-linux-mainline-7-2-sm8650-hdk-rfc-0-v1-58-ed985aefd611@kernel.org/)定义 1,024-bin Bayer-quad luma 统计，TFE 的 bin 数相同也不足以证明通道与采样语义相同。这些公共接口尚未合入，采用或扩展时仍需沿上游设计核对和评审，不能在项目里自行分配 block 或 fourcc 后宣称标准接口适配完成。

独立实现还需补全 AEC BE/BHIST、AWB BG 和 CAF 的计算寄存器、允许配置、最大输出及失败停止流程。原厂相同模式的寄存器命令、缓冲尺寸／stride、统计完成顺序和统计样本用于交叉测量；普通 Linux 控件枚举和空闲寄存器读回不能补出这些定义。Qualcomm 的 [libcamx-dev 打包说明](https://github.com/qualcomm-linux/pkg-camx-dev/blob/ceee1ff870a96da9ea4fc2c7ba46b952c6e034da/debian/copyright)明确区分 BSD 打包脚本与闭源 CamX 载荷，公开下载不提供导入实现或调校的许可。

### Media graph 与并发

当前产品为后摄配置 `s5kjn1 → msm_csiphy1 → msm_csid0 → msm_vfe0_pix → msm_vfe0_video3`，前摄使用 `ov32d40 → msm_csiphy4 → msm_csid1 → msm_vfe1_pix → msm_vfe1_video3`。CSID sink 是 pad 0，IPP 是 source pad 4；VFE sink/source 分别是 pad 0/1。设备节点编号由探测顺序决定，不是配置常量。原始采集使用 CSID source pad 1 和对应 VFE RDI0。

原有产品内核的 `MEDIA_IOC_G_TOPOLOGY` 已确认后摄到镜头和两路 flash 的三个 enabled、immutable ancillary links，运行设备树的 `lens-focus` 和 `flash-leds` 指向相同设备。镜头由 GT9764BA 驱动接管后沿用该关联，仍需上机检查开流、对焦和关闭后的归位。

服务沿传感器发出的 enabled ancillary link 查找 `MEDIA_ENT_F_LENS`，不再按镜头驱动名称匹配。前摄没有该关联，保持固定焦距；关联指向不存在的设备或同一传感器关联多个镜头时，记录错误并关闭该路自动对焦，不猜测设备，也不影响另一颗相机。

原厂 `OS3.0.309.0.WPYCNXM` 的后摄模块描述和 actuator 库标明马达为 GT9764BA，初始化依次写 `0x02 = 0`、`0x02 = 2`、`0x06 = 0x40`、`0x07 = 0x73`。[厂商产品表](https://www.giantec-semi.com/yqmd/164)将 GT9764BA 标为双向、GT9769 标为单向；两者不能因位置寄存器相同就视为完整兼容。[GT9764BA 驱动](../../../patches/linux/7.3/0126-media-gt9764ba-lens.patch)和 [binding](../../../patches/linux/7.3/0125-media-gt9764ba-binding.patch)提供标准 V4L2 镜头实体、`V4L2_CID_FOCUS_ABSOLUTE` 和 runtime PM，设备树不再使用 `giantec,gt9769` 匹配 DW9768 驱动。

GT9764BA 使用 0–1023 的 DAC 范围，默认值和断电前的归位目标为 512。[MiCode 的 GPL GT9764 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/a369d30bd5df3fcfb4304ee9e7ae10fa4f11899a/drivers/misc/mediatek/lens/vcm/v4l2/gt9764/gt9764.c)也使用该中点。归位每步最多移动 50，等待 10 ms；时序来自原厂 `GetParkLens` 回调及其 I²C 命令构造调用链。运行时归位写入失败会保留供电，以便重试。解绑时归位失败仍释放驱动对稳压器的使能引用。恢复供电后按相同步幅恢复 V4L2 已接受的焦点，不采用试算或写失败的档位。10 ms 是归位步进等待，不是光学稳定时间的测量；手动移动后的稳定时间和对焦标定仍未实现。原厂 `0x73` 超出 DW9768 binding 的 6-bit timing 范围，驱动使用 GT9764BA 原厂寄存器值，不套用 DW9768 公式。

SM8750 只公布同编号 CSID→VFE 链接，与 Gen4 的寄存器更新、帧时序和完成回调一致；CSIPHY 选择和其他 SoC 的图保持不变。CSID 四个 source pad 共用格式归一化，IPP pad 4 不再接受任意 code 或尺寸。前后摄同时工作、交错停流、模式切换和失败恢复仍需实机检查；共享隐私灯的引用计数验证不证明两路图像、统计和控件互不干扰。

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

后摄开流时拒绝改变实际传感器模式，仍允许试选或重复设置当前模式。
停流切换时只更新控件范围，新模式寄存器写入后才应用帧长和曝光。
后摄曝光、增益或帧长写入失败后，驱动拒绝继续在线更新这些寄存器。
重新开流时先重置传感器并应用当前控件，再发布新帧。
平板从 4080×3060 停流切换至 3840×2160 后，连续采集 180 帧，
CSID 时间戳测得 59.98 fps；这不包含编码、录音或全幅静态照片。

后摄 8160×6120 保留全幅 Quad CFA 样本，经 RDI 采集、QPPG 转换和软件颜色处理输出 NV12。传感器的 10 fps 不等于软件输出速率：当前平板约 2.4 秒生成一张全幅图像，约 0.4 fps。工作内存约 953 MiB；转换线程处理期间丢弃新输入，不排队积压旧帧。后摄新模式使用同机原厂静态 ODM 寄存器表；来源记录在内核补丁中。19.2 MHz 输入时钟、857.6 MHz CSI 链路由共享设备树配置，旧 24 MHz 传感器表仍保留给使用该时钟的设备树。

输出可以小于传感器模式。宽度按 16 像素、高度按 2 像素对齐；普通模式的 ISP 先等比缩放，再居中裁切不同宽高比，不拉伸画面。后摄全幅先完成 Quad CFA 转换，再由 libyuv 居中裁切、缩放；不会放大较小的传感器模式冒充全幅。极小输出受 ISP 最大 16 倍缩小限制，会进一步裁切视野。60 fps 只公布给后摄 3840×2160 以内的输出，前后摄全尺寸模式均只公布 10 fps。

前摄三张寄存器表与同机原厂静态配置一致，驱动通过 V4L2 pad 帧间隔报告标称 10／30 fps。
平板用 CSID 硬件时间戳各测了 40 帧：两种 3264 宽模式帧周期为 33.3324 ms，
6528×4896 为 99.9992 ms。测试使用 1920×1440 的 PIX 输出，确认了实际传感器节奏，
未检查全幅照片输出或画质。

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

`piano-camera` 安装相机服务和 `piano-camera-ctl`。客户端复用服务已有的硬件连接；未开流时返回 `ENODEV`，不会暗中启动相机。相机包依赖设备配置和带匹配 `v4l2loopback` 的内核元包，升级内核时不必替换相机程序。

产品镜像已启用相机服务。自行安装时，Debian 由包启用服务；Arch 系使用
`systemctl enable --now piano-camerad.service`。从直接复制文件的旧镜像迁移时，
沿用[设备配置的文件归属检查](../../../linux/bsp/README.md)，不移动 `/etc/piano/camera.conf`。

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

前摄时间接口使用原厂标称帧间隔除以原生 FLL，与 CamX 的积分时间关系一致。3264×2448、3264×1836 的计数周期为 `1e9 / 84600` 纳秒；500000 纳秒取最近合法的 42 个周期，约 496454 纳秒。全尺寸周期为 `1e9 / 25510` 纳秒，相同请求取 13 个周期，约 509604 纳秒。

同芯片的[公开驱动](https://github.com/HelloVolla/android_kernel_modules_volla_mt6878/blob/bc3a2f54403e608257b3f0780002c8355578ce2b/mtkcam/imgsensor/src-v4l2/common/ov32d40_mipi_raw/ov32d40mipiraw_Sensor.c#L139-L159)按同一原生 HTS／FLL 和 100 MHz 计时。由此算得的 11.82／39.20 µs 与 Piano 上测得的帧周期相符；不同平台的输入时钟和 PLL 配置不能照搬。CSI 的传输跨度也不能证明原生计数对应多少图像行。没有校准依据的 ISO、Kelvin、物理焦距或对焦距离换算不对外提供。

控制 socket 为 `/run/piano-camerad/control.sock`，权限 `root:video 0660`。它没有额外守护进程，也不替代应用的 V4L2 格式协商。

## 帧时间戳与错误

PipeWire 的 V4L2 采集对不支持导出 DMA-BUF 的设备使用 MMAP 复制。
复制后的 SPA 缓冲与内核捕获编号分开管理，仍在排队、已交给应用或带有非零
busy 引用的帧不能被覆盖；内核缓冲在复制完成后立即归还。应用暂时持有全部
缓冲时仍推进相机时钟，允许处理循环归还帧，不修改传感器时间戳。
修复见[共享 PipeWire 补丁](../../../patches/pipewire/1.6.9/0007-v4l2-copy-buffer-pool.patch)。

Debian 构建把修复装入原有的 `libspa-0.2-modules`。`./build.sh pipewire`
按同一 Debian 源码版本一起生成服务端、库和插件。Nyarch 和 SteamOS 使用目标
发行版原有的服务端、音频与蓝牙库，并重建 GStreamer 插件和
`pipewire-v4l2-source`。独立采集包安装 SPA 库和标准
`context.spa-libs` 配置；它使用独立 source factory 名称，因为 PipeWire
按首个匹配规则选择库，已有的 `api.v4l2.*` 规则不会被后加载的同名规则替换。
WirePlumber 从该库枚举设备后将独立 factory 交给 PipeWire，两个进程因此使用
同一修复。Debian、Nyarch 和 SteamOS 共用源码与补丁。正式 Debian 包的前后摄
带声音录像已检查开始、停止、重新打开和完整解码。

Debian 的 Plasma Camera 直接使用 libcamera 读取实体相机。录像时改用
`VideoRecording` 流，优先选目标尺寸，较低的档位可从同画幅的较大输入缩小。
每张完成的 NV12 帧按捕获时间提交给 Qt，帧数据只复制一次到 Qt 缓冲；
停录后回到原预览尺寸。不会用定时器重复上一帧或将小尺寸放大成 4K。
在关闭旧服务和虚拟摄像头的实机上，前后摄拍照、切换及关窗重开已通过。
搭配实验 Qt 编码后端，前摄完成 1080p 有声录像，后摄短录 184 张连续的
4K 帧（约 60 fps）并完整解码；编码级别、长期音画同步和原生 Qt 包另行验收。

FFmpeg 8.1.1 和 9.0.2 共用[有效码流边界](../../../patches/ffmpeg/9.0.2/0008-v4l2m2m-bound-capture-payload.patch)
和[编码缓冲保留](../../../patches/ffmpeg/9.0.2/0009-v4l2m2m-reserve-capture-buffers.patch)修复。
当封装等待另一轨而持有编码包时，FFmpeg 复制压缩码流并立即归还稀缺的
CAPTURE 缓冲，默认缓冲数不变。平板接真实麦克风录制 8 秒，最小补丁版送入
Iris 的 433 帧全部写入并解码，音视频时长约 8 秒，未再出现编码缓冲耗尽。
现有应用经过 loopback 时仍会跳帧；加大共享队列也不能保证应用持有的帧不被写端复用。
绕过 loopback、从 CAMSS PIX 直连 Iris 的同机测试，60 秒 MP4 实际解码出
3600 帧 H.264，视频最大帧间隔 16.673 ms，麦克风轨完整 60 秒。
这组结果验证硬件采集和编码链路；普通应用接入、同一声光事件的音画偏差
和拍照画质仍需分别测量。

完整 SM8750 CAMSS 的 IPP 和 RDI0 使用 CSID980 锁存的物理计数器，通过标准 V4L2 接口报告帧时间。

| 接口 | 时间含义 | 同一帧的编号 |
| --- | --- | --- |
| `V4L2_EVENT_FRAME_SYNC` | `timestamp` 是 CSI SOF，时钟为 `CLOCK_MONOTONIC` | `u.frame_sync.frame_sequence` |
| `VIDIOC_DQBUF` | `timestamp` 是 CSI EOF，标志为 `TIMESTAMP_MONOTONIC` 和 `TSTAMP_SRC_EOF` | `sequence` |

应用按表中的帧编号配对；`event.sequence` 是事件队列的序号，不能用来关联图像。DMA 完成、用户态出队和 ISP 转换均可能晚于 EOF。`DQBUF` 的 `timeval` 将纳秒截断到微秒，保守覆盖 EOF 时取该微秒区间的上界，即换算为纳秒后加 999。

中断内依次读取物理计数器、`ktime_get_ns()`、物理计数器，以两次计数器读数的中点换算锁存值。驱动要求 ARM64 `CNTFRQ_EL0` 为 19.2 MHz，否则拒绝换算；采样区间超过 50 微秒或锁存值早于采样超过一秒时也拒绝换算。在 `CLOCK_MONOTONIC` 总瞬时速率与计数器标称速率相差不超过 ±500 ppm 的条件下，误差保守上界为 550 微秒。

CSI SOF 是帧包开始，积分可能更早。内核不减去曝光时间，也不标记 `TSTAMP_SRC_SOE`；拍照仍须结合传感器积分模型。SOF 到 EOF 的实际跨度也不能简单替换成有效行数乘行时间。

libcamera `SensorTimestamp` 的语义是首行开始曝光、时钟为 `CLOCK_BOOTTIME`。这里的 CSI SOF 和 EOF 都不能直接填入该字段：还需要传感器积分相对 CSI 的时序和 MONOTONIC→BOOTTIME 换算，并在睡眠恢复后重新核对。现有 simple pipeline 直接使用捕获时间戳的行为不是 TFE 时间戳语义已经适配。

`libcamerasrc` 在没有 `SensorTimestamp` 时使用已完成缓冲的时间，避免减去管线
起点时产生无效时间戳。后摄 3840×2160 经 GStreamer 和 Iris 录有声 MP4，
在编码器和封装器之间保留压缩包队列，避免封装器等待音轨时占住相机缓冲。
平板连续取到 3600 帧，录入的 3600 帧均可解码，视频最大帧间隔 16.833 ms，
内置麦克风轨约 60 秒。相机应用的长时间 4K60 模式与同一声光事件的同步偏差另需验证。

SOF/EOF 按既有双缓冲 FIFO 关联。标记缺失、不连续、接收端错误或无效换算会使该流持续返回 `V4L2_BUF_FLAG_ERROR`，直到重新开流。TFE 图像尺寸、CCIF 和约束错误另走 BUS 中断；处理错误与完成缓冲共用 `output_lock`，先检查错误再发布图像。只有活动的 write master 允许访问 BUS 寄存器，停止最后一条流时在关时钟前屏蔽、清除并读回中断配置。

停流开始后，CAMSS 不再由完成中断提交缓冲。驱动等已经取走缓冲的
VFE／CSID 中断处理完，再归还和清空队列，重开时不会用到上一次的槽位。
平板前后摄均通过实际采集中退出和重新打开。这项修复只管软件队列，
不将中断处理结束视为硬件 DMA 已排空。
见[停流修复](../../../patches/linux/7.3/0141-LOCAL-media-camss-serialize-buffer-completion.patch)。

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
