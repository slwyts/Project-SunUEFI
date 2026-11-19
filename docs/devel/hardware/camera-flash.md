# Piano 摄像头闪光灯

手电筒经标准 LED 接口控制，后摄也能通过标准 V4L2 flash 控件触发两个灯组。
两组分别对应控制器的 1＋4 和 2＋3 通道。普通相机应用还不能自动使用闪光灯：
硬件能发出短闪光，但 libcamera 尚未将它与曝光同步。

## 身份、接线与限值

原厂最终 Android 设备树将控制器放在 PM8550 的 SPMI SID 1、基址 `0xee00`。
实机只读寄存器返回 `type=0x18`、`subtype=0x07`，对应标准驱动支持的四通道 IP。
原厂后摄的两个请求入口不是物理接线：HyperOS 控制中心实际开启全部四路。
原厂设备树也注明了 `0x9`（1＋4）和 `0x6`（2＋3）两个配对。
平板上验证两组均能单独出光，采用与[主线 SM8750 参考板](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8750-mtp.dts)
相同的两通道分组，无需扩展绑定或改动供电。

电流属性表示一组的总电流，驱动在组内均分。每组持续手电筒上限为 100 mA，
闪光上限为 400 mA、300 ms；改为配对后仍保留原有板级总电流上限。
[原厂实现](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/leds/leds-qti-flash.c#L1710)
允许短时提高 torch 电流，再按时间降档。标准驱动不提供这条曲线。

## 内核与设备树

共享的 [flash overlay](../../../linux/dts/piano-camera-flash.dtso) 禁用原厂
flash 节点和 trigger 配置，在原位置创建标准 PM8550 `led-controller@ee00`。
S5KJN1 的 `flash-leds` 引用两个配对节点，由
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
零亮度关闭灯组。[定时器修复](../../../patches/linux/7.3/0137-LOCAL-leds-qcom-flash-fix-safety-timer.patch)
让硬件按请求的时长截止：最低 10 ms 对应 `0x80`，而 `0x00` 表示关闭定时器，
供持续手电筒使用。

## 设备检查范围

平板后摄在固定曝光、白平衡和对焦时拍到两个灯组分别点亮、同时点亮和熄灭。
手电筒按每通道 35 mA 测试，灯控芯片报告上升完成。旧配置的 1＋2 组合
在同电流下不出光，芯片报告开路、短路或供电未就绪的汇总故障。当前驱动只读
部分故障寄存器，V4L2 `faults=0` 不能排除这种故障。

每组以 100 mA 发出 40 和 80 ms 的短闪光，后摄拍到亮帧及随后熄灭。
修正前芯片约在 52 和 91 ms 完成下降；换入修正后的驱动后，在相同档位约为
42 和 81 ms。10、20、300 ms 也在灯关闭时核对了实际编码。
这里只证明标准控件可以发出短闪光、关灯和硬件计时正确，不是对相机自动闪光的验收。

## 拍照与失败处理

现有相机服务使用后摄的 ancillary links 查找两路 flash。强制闪光拍照会冻结
曝光、白平衡和对焦，等待冻结后的真实传感器帧，再请求有硬件超时的闪光。
只有完整曝光区间落在保守亮灯窗口内的帧才返回；取消、超时、格式切换或
传感器错误都关闭闪光、恢复控制状态，不交付其他帧。

这一流程仍属于现有服务，未接入 libcamera，也没有完成光学曝光同步测量。
控件、SOF／EOF 帧关联和现有命令见[相机格式与控件](camera-controls.md#闪光灯)。
