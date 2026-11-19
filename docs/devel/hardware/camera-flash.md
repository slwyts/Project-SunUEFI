# Piano 摄像头闪光灯

产品设备树将两个灯路分别接入标准 LED class 和 V4L2 flash 控件，后摄 media
graph 已建立对应的 ancillary links。这两个灯路单独或同时打开均不出光。
原厂手电筒同时打开四个电流通道；Linux 用同样的四路分组做对照时也能亮灯、
关灯。当前 V4L2 控件不报告两路失败时芯片的汇总故障。普通相机应用还不能
自动使用拍照闪光。

## 身份、接线与限值

原厂最终 Android 设备树将控制器放在 PM8550 的 SPMI SID 1、基址 `0xee00`。
实机只读寄存器返回 `type=0x18`、`subtype=0x07`，对应标准驱动支持的四通道 IP。
现有产品设备树按原厂后摄的两个请求入口 `qcom,id=0/1` 声明了通道 1、2。
原厂手电筒驱动会把请求电流分到四个通道。驱动的请求入口不是物理接线，
不能据此判定背面有几颗发光器件，或拍照与手电筒使用同样的分组。

每路 flash 上限为 400 mA、300 ms。原厂 torch 允许短时 400 mA，5000 ms 后
降至 100 mA；标准驱动没有该时间曲线，因此持续 torch 上限采用每路 100 mA。
限值依据为 [MiCode 原厂实现](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/leds/leds-qti-flash.c#L1710)。

## 内核与设备树

共享的 [flash overlay](../../../linux/dts/piano-camera-flash.dtso) 禁用原厂
flash 节点和 trigger 配置，在原位置创建标准 `led-controller@ee00`，只描述
通道 1、2 的 LED 节点。S5KJN1 的 `flash-leds` 引用这两个子节点，由
`v4l2_async_register_subdev_sensor()` 建立关联，不增加用户空间设备或守护进程。
绑定见 [qcom,spmi-flash-led.yaml](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/qcom,spmi-flash-led.yaml)。

[内核配置](../../../linux/configs/piano-flash.config) 启用 `LEDS_CLASS_FLASH`、
`LEDS_QCOM_FLASH`、`V4L2_FLASH_LED_CLASS` 和 `VIDEO_V4L2_SUBDEV_API`。
驱动提供 torch brightness、flash brightness、timeout、strobe 和 fault。
V4L2 wrapper 提供对应的标准控件；节点编号随探测顺序变化，应通过 media graph
和设备名定位，不固定 `/dev/v4l-subdevN`。

[平台补丁](../../../patches/linux/7.3/0001-platform.patch)修正释放循环的索引，
并让合法 5 mA 电流档位经过 LED 亮度转换后保持一致：转换到 0..255 编码时
向上取整，硬件 setter 继续向下取整并保留原电流上限、温控与故障处理。
25 mA 请求对应亮度 64、ITARGET 4，零亮度仍表示关闭。

## 设备检查范围

产品内核上两路 LED 的 flash 上限均读回 400000 µA，timeout 上限为
300000 µs。分别请求 25 mA 时，PMIC 的模块与通道使能正确置位，
电流寄存器读回的档位与请求一致。

分别以 100 mA 打开两路手电筒，后摄锁住曝光、白平衡与对焦。关灯、两路开灯
以及各自关闭后的 153 帧没有随灯光变化。两路开启时模块、通道和电流寄存器
均正确写入，实时状态寄存器 `0xee10` 从 `0x00` 变成 `0x49`：汇总故障置位，
未出现上升完成。原厂将开路、短路和电源未就绪并到这个标志，单次读数无法区分。
主线驱动读取的 `0xee06`、`0xee07`、`0xee09` 为零，故 V4L2 `faults` 也为零。
关灯后两路使能和汇总故障都清零。

用同一内核和两份仅灯路组合不同的设备树，分别一次打开两路和四路，芯片读回
均为每路 35 mA。平板后摄固定曝光、白平衡和对焦：两路仍不出光，芯片状态为
`0x49`；四路有明显照明变化，芯片状态为 `0x30`，与同机 HyperOS 点亮时一致。
四路关闭后，画面恢复到开灯前的亮度，芯片使能及状态也归零。两份 Linux
设备树使用相同的每路电流上限和供电配置。

四路对照只有 LED class 手电筒，没有 V4L2 拍照闪光。驱动可以运行四路分组，
但当前设备树绑定只允许每个 LED 节点列出两个通道。正式配置须解决这一绑定
差异，并分别验证手电筒和拍照闪光，不能把对照设备树直接作为成品。

## 拍照与失败处理

现有相机服务使用后摄的 ancillary links 查找两路 flash。强制闪光拍照会冻结
曝光、白平衡和对焦，等待冻结后的真实传感器帧，再请求有硬件超时的闪光。
只有完整曝光区间落在保守亮灯窗口内的帧才返回；取消、超时、格式切换或
传感器错误都关闭闪光、恢复控制状态，不交付其他帧。

这一流程仍属于现有服务，未接入 libcamera，也没有完成光学曝光同步测量。
控件、SOF／EOF 帧关联和现有命令见[相机格式与控件](camera-controls.md#闪光灯)。
