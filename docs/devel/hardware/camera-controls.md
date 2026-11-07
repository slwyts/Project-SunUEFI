# 相机格式与控件

相机服务向应用提供前摄、后摄两个 V4L2 摄像头。普通模式使用 CAMSS 硬件 ISP 输出 NV12；后摄全幅 RAW10 使用软件 Quad CFA 转换。服务只在应用开流时启动传感器和 ISP，关闭流后停止 DMA。曝光、白平衡和后摄对焦由同一服务的帧循环控制。

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

中断内依次读取物理计数器、`ktime_get_ns()`、物理计数器，以两次计数器读数的中点换算锁存值。计数频率来自 ARM64 `CNTFRQ_EL0`；采样区间超过 50 微秒或锁存值早于采样超过一秒时拒绝换算。在 `CLOCK_MONOTONIC` 总瞬时速率与计数器标称速率相差不超过 ±500 ppm 的条件下，误差保守上界为 550 微秒。

CSI SOF 是帧包开始，积分可能更早。内核不减去曝光时间，也不标记 `TSTAMP_SRC_SOE`；拍照仍须结合传感器积分模型。SOF 到 EOF 的实际跨度也不能简单替换成有效行数乘行时间。

SOF/EOF 按既有双缓冲 FIFO 关联。标记缺失、不连续、接收端错误或无效换算会使该流持续返回 `V4L2_BUF_FLAG_ERROR`，直到重新开流。TFE 图像尺寸、CCIF 和约束错误另走 BUS 中断；处理错误与完成缓冲共用 `output_lock`，先检查错误再发布图像。只有活动的 write master 允许访问 BUS 寄存器，停止最后一条流时在关时钟前屏蔽、清除并读回中断配置。

实现见[物理帧时序补丁](../../../patches/linux/7.3/0048-camss-frame-timestamps.patch)和 [TFE BUS 修复](../../../patches/linux/7.3/0049-camss-tfe-bus-errors.patch)。`camss_frame`、`camss_frame_error` trace 提供原始计数器、交叉采样和缓冲编号。

## 闪光灯

PMIC 闪光灯使用标准 V4L2 flash 控件，后摄通过 `flash-leds` 关联两路灯。现有服务复用控制 socket 接受一次强制闪光拍照请求：

```sh
piano-camera-ctl capture rear --flash --output photo.jpg
```

程序冻结当前曝光、白平衡和对焦，等待两个真正开始于冻结之后的传感器帧，再开启有硬件超时的闪光。物理 `FRAME_SYNC` 事件与同序号捕获缓冲提供 SOF 和 EOF；只有完整曝光区间落在保守亮灯窗口内的帧才交付。取消、超时、格式切换或传感器错误均关闭闪光、恢复原控制状态，不返回其他帧。

照片尺寸沿用当前协商的 PIX 输出，不会自动切换成原生全幅。全幅 RDI 和前摄闪光请求返回 `EOPNOTSUPP`。该路径提供长脉冲曝光覆盖，不包含自动预闪测光或短脉冲冻结运动；普通视频应用也不会自动发出这条拍照请求。JPEG 由调用者接收只读封存的图像文件描述符后编码，服务不接收用户输出路径。

隐私指示灯的驱动与 PWM 联动见[实现说明](camera-privacy-led.md)。它与拍照闪光灯用途不同。
