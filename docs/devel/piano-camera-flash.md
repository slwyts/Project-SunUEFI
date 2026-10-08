# Piano 摄像头闪光灯

2026-10-08：实机只读 regmap `0-01` 的精确两寄存器读回确认 **SPMI SID 1、基址
0xee00，type=0x18、subtype=0x07**。标准驱动已有该四通道 IP 布局，两路实际接线
1/2可直接描述。默认release已接入标准驱动配置、DT overlay、cleanup补丁和
modules-load策略。新内核 `7.2.9-piano-gnome-g45bba6e91e6c` 已完整构建，
随匹配模块和默认设备树部署，并通过普通 BOOT 重启进入 Linux。
两路 `:flash-0` / `:flash-1` 标准 LED class 已注册；实际读回每路 flash
上限400000µA、timeout上限300000µs。第一路以32/255的低档torch请求运行1秒，
写入成功、fault无标志，随后brightness读回0。未观察实际发光或测光，
也未证明相机应用曝光同步。

## 身份、接线与限值

原厂最终 Android DTB 与参考 `vendor/piano-linux/board.dtb` 一致：父节点为
`/soc/qcom,spmi@c42d000/qcom,pm8550@1`，`reg=<1 0>`；旧 flash 位于
`qcom,flash_led@ee00`。原厂 compatible `qcom,qti-pm8350c-flash-led` 是 IP 名称，
不用于推测 SID。原厂 `qcom,id` 0/1 对应标准一基 `led-sources=<1>/<2>`，
保持两支独立 LED，不合并为并联通道。

每路 flash 限值为 **400mA、300ms**；原厂 torch 允许短时400mA，5000ms后降至
100mA。主线没有该时间曲线，因此连续 torch 上限采用 **100mA/路**。
限值字段依据 [MiCode 原厂实现](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/leds/leds-qti-flash.c#L1710)。

只读身份记录在本地
`private/provisioning/recovery-priority-20261008/flash-type.json`：ee04=18、ee05=07，
共读取两寄存器。原厂 live DTB SHA256 为
`8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc`，参考板 DTB 为
`36e6adef75306d7727f79e97d0406376baa43e1b454e29d1c40bc0fc36d47db3`。
这确认身份和描述依据，不证明亮度、供电、thermal/fault 或曝光同步已经正常。

## 唯一默认 release 接线

[piano-camera-flash.dtso](../../linux/dts/piano-camera-flash.dtso) 禁用旧 vendor
flash 和 `/soc/qcom,camera-flash0` trigger consumer，创建只有两个标准子节点的
`led-controller@ee00` 并设为 `okay`，保持100mA torch、400mA/300ms flash。
旧 vendor 多子节点结构不能直接换 compatible；标准绑定见
[qcom,spmi-flash-led.yaml](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/qcom,spmi-flash-led.yaml)。
没有加入电源/GPIO猜测或手工寄存器初始化。

[piano-flash.config](../../linux/configs/piano-flash.config) 是现有完整内核上的小
fragment，不创建第二个 profile：

```text
CONFIG_LEDS_CLASS_FLASH=m
CONFIG_LEDS_QCOM_FLASH=m
CONFIG_V4L2_FLASH_LED_CLASS=m
CONFIG_VIDEO_V4L2_SUBDEV_API=y
```

现有 LED、SPMI PMIC、VIDEO_DEV 依赖保留；V4L2_FLASH_LED_CLASS 按 Kconfig 选择
MEDIA_CONTROLLER、V4L2_ASYNC 和 subdev API。`build_piano_full_kernel.py`默认合并
这四项及既有UHID，并检查四项最终值和PUBLIC其余选项。配置检查通过，未修改
此前已完成的7fff内核O目录或artifact。

现有根系统会阻止SoC `of:*`自动modprobe，默认完整stager现复制同一
`etc/modules-load.d/piano-flash.conf`，标准加载`leds_qcom_flash`；camera可选BSP
也通过manifest包含这份策略。不新增daemon或profile，不把DT `okay`当作已绑定。
`build.sh package`与release的additional_dtb_overlays均保留DMIC并加入flash。

统一的新源码准备已经带上既有
[cleanup 索引补丁](../../patches/linux/7.2.9/0004-leds-qcom-flash-cleanup-index.patch)。
它修正两个释放循环的数组越界，先检查成功计数、递减索引再释放，包括 NULL。
此前该补丁已做单AArch64对象编译。本轮以旧7fff真实tree加补丁的临时index算出
新tree `b8f07b7e9f9efb0cece6dc465f7052fc5d7079a8`，标准准备器复现同一tree，
新commit为`45bba6e91e6c1f04b1d306024a7ef3780c0380dc`；旧snapshot保持不变。
新O配置SHA为`8baf147f407fdb21659ad012f0535f285fba5c382da719f0f03aa2e904729aa1`。

标准 LED class 提供 torch brightness、flash brightness/timeout/strobe/fault；
V4L2 wrapper 注册异步 flash subdevice。相机侧 fwnode/notifier 关联仍需验证，不能
仅凭注册宣布 `/dev/v4l-subdev*` 或曝光同步可用，也不恢复 vendor trigger daemon。
接口语义见 [Linux Flash LED 文档](https://docs.kernel.org/leds/leds-class-flash.html)。

本次实际 `/sys/class/video4linux/*/name` 尚无flash subdevice；LED class注册不能
代替media关联。部署和低档请求记录位于本机
`private/provisioning/recovery-priority-20261008/45b-cold-interfaces.txt` 与
`flash-torch-low-current.txt`。原UFS系统分区和BOOT载荷未在这轮部署中重写；
更新的是项目ESP中的Linux boot文件及根系统模块/软件包。

下一默认 overlay 已给实际 S5KJN1 节点增加标准 `flash-leds`，引用两路 LED
子节点。该 sensor 使用 `v4l2_async_register_subdev_sensor()`，其现有 notifier
读取这项属性；flash wrapper 已按 LED fwnode 注册，因此不另加用户空间设备。
组合 DTB 中两个 phandle 已分别解析到真实 led-0/led-1。关联已随a8版启动文件
部署并通过普通BOOT重启，实际出现 `/dev/v4l-subdev34` 的 `:flash-0` 与
`/dev/v4l-subdev35` 的 `:flash-1`。标准V4L2接口列出LED mode、software/hardware
strobe、timeout、flash/torch intensity和fault；当前off、无fault。这确认了
真实subdevice与控制接口，不证明拍照曝光同步或桌面应用已经接好闪光灯。

同一a8版本又通过标准V4L2控件分别请求两路25mA torch，并只读实际PMIC
regmap的11个明确寄存器。开启第一路时，module-enable `ee46=80`、channel-enable
`ee4e=01`、5mA resolution `ee49=01`、第一路current-target `ee42=03`；
开启第二路时对应 `ee4e=02`、`ee49=02`、`ee43=03`。5mA步进的target=3实际
编程电流为20mA：现有强度与0..255亮度的两次向下取整，使25mA请求降到24mA，
随后PMIC再向下取整。不能将请求值当作实测或编程电流。两次三个状态寄存器
`ee06/ee07/ee09`均为0；关闭后module/channel-enable均恢复0。
这确认控件已到达PMIC输出配置，没有直接写寄存器。实际光学发光与曝光同步
仍未证明：此前后摄统计没有明确亮度变化，场景距离和遮挡也未受控，不能据此
认定灯不亮。原始读回位于本机 `private/provisioning/recovery-priority-20261008/flash-register-a8.json`。

上述取整问题已有下一默认内核的[标准驱动补丁](../../patches/linux/7.2.9/0010-leds-qcom-flash-torch-rounding.patch)：仅在电流转0..255 LED编码时使用ceil，保留setter的floor、上限和温控路径；反向控件转换按相同每通道5mA档返回总电流，避免把亮度64报告成25098µA。25mA因此对应亮度64、setter25mA、ITARGET4；nearest并不足够，15mA仍会经亮度38降为10mA。1..4通道在500mA/通道上限下，ceil编码误差小于一个硬件5mA档，合法V4L2步进得以保留。正值最低档来自实际setter的ITARGET0，brightness0仍为关闭。独立ARM64对象编译及必要整数边界检查已通过，记录在`private/analysis/piano-flash-rounding-20261008/result.json`；未修改活动a589源码/O或部署新驱动。该反向转换不是寄存器测量，温控仍可能进一步降低电流，新ITARGET与光学结果需要实机读回。

此前主机已对参考board.dtb组合一次overlay，检查标准节点okay、旧vendor/trigger
disabled及两路通道、电流和时限，结果在`build/flash-overlay-check/`。本轮另确认
完整stager实际复制modules-load策略、可选camera BSP payload含相同文件；没有运行
服务、触碰设备或生成新的测试镜像。下一次统一build使用这条默认接线。
