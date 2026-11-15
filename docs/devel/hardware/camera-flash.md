# Piano 摄像头闪光灯

两路闪光灯通过标准 LED class 和 V4L2 flash 控件接入，后摄 media graph
已建立对应的 ancillary links。手电筒请求能到达 PMIC，关闭后亮度与通道使能
恢复零，没有故障标志；实际发光和拍照曝光同步还没有确认。普通相机应用
还不能自动使用拍照闪光。

## 身份、接线与限值

原厂最终 Android 设备树将控制器放在 PM8550 的 SPMI SID 1、基址 `0xee00`。
实机只读寄存器返回 `type=0x18`、`subtype=0x07`，对应标准驱动支持的四通道 IP。
两路接线使用通道 1、2；原厂的 `qcom,id=0/1` 对应标准一基
`led-sources=<1>/<2>`，不合并为并联通道。

每路 flash 上限为 400 mA、300 ms。原厂 torch 允许短时 400 mA，5000 ms 后
降至 100 mA；标准驱动没有该时间曲线，因此持续 torch 上限采用每路 100 mA。
限值依据为 [MiCode 原厂实现](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/leds/leds-qti-flash.c#L1710)。

## 内核与设备树

共享的 [flash overlay](../../../linux/dts/piano-camera-flash.dtso) 禁用原厂
flash 节点和 trigger 配置，在原位置创建标准 `led-controller@ee00`，只描述
已接线的两路 LED。S5KJN1 的 `flash-leds` 引用这两个子节点，由
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
300000 µs。分别请求 25 mA torch 时，PMIC 的 module-enable 和 channel-enable
正确置位，ITARGET 为 4，关闭后使能归零，三项状态寄存器没有故障。

通过标准 LED 接口分别请求亮度 64、128 后也能读回请求值并关闭。固定电脑
摄像头曝光进行前后对照，没有得到明确的场景照明变化；后摄在冻结曝光和
白平衡的 150 帧采集中，平均亮度保持约 20，没有随这两次开灯请求明显变化。
灯的朝向、遮挡和场景未受控，这些结果还不足以判断实际发光或照明强度。

## 拍照与失败处理

现有相机服务使用后摄的 ancillary links 查找两路 flash。强制闪光拍照会冻结
曝光、白平衡和对焦，等待冻结后的真实传感器帧，再请求有硬件超时的闪光。
只有完整曝光区间落在保守亮灯窗口内的帧才返回；取消、超时、格式切换或
传感器错误都关闭闪光、恢复控制状态，不交付其他帧。

这一流程仍属于现有服务，未接入 libcamera，也没有完成光学曝光同步测量。
控件、SOF／EOF 帧关联和现有命令见[相机格式与控件](camera-controls.md#闪光灯)。
