# Piano 摄像头闪光灯

2026-10-08：已准备标准 LED Flash/V4L2 接口的非默认诊断 DT；未部署、未点灯，也没有实机 peripheral type/subtype 读值。控制器保持 `disabled`，默认构建与内核配置不变。

## 实际硬件路径

原厂最终 Android DTB 与参考 `vendor/piano-linux/board.dtb` 都使用：

```text
/soc/qcom,spmi@c42d000/qcom,pm8550@1/qcom,flash_led@ee00
```

父设备为 `qcom,spmi-pmic`，`reg = <1 0>`，即 **SPMI SID 1、外设基址 0xee00**。原厂 flash compatible 是 `qcom,qti-pm8350c-flash-led`；该 IP 名称不能用来猜父 PMIC 的 SID。原厂 `/soc/qcom,camera-flash0` 的 `flash-source` 指向 `qcom,flash_0/1`，`torch-source` 指向 `qcom,torch_0/1`。两路 `qcom,id` 为 0/1，对应主线的一基 `led-sources = <1>/<2>`，不把两路臆造为一个并联 LED。

每路原厂 flash 上限为 400mA、300ms；torch 的时间表为 400mA 至 5000ms、随后 100mA。主线没有这份小米时间曲线，诊断描述将连续 torch 上限直接设为 **100mA/路**，flash 保留 400mA/300ms。[MiCode 原厂实现](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/leds/leds-qti-flash.c#L1710)记录了时限与分档字段。

本地依据为 `private/analysis/android-board-runtime-2026-10-05/live.dtb`（SHA-256 `8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc`）与参考板 DTB（SHA-256 `36e6adef75306d7727f79e97d0406376baa43e1b454e29d1c40bc0fc36d47db3`）。原厂绑定记录有 `leds-qti-flash`，这只证明绑定，不能证明两路实际点亮或供电正常。

## 诊断 DT 与驱动

[piano-camera-flash.dtso](../../linux/dts/piano-camera-flash.dtso)先禁用旧 vendor flash 节点和旧 camera trigger consumer，再在同一 SID 创建独立的标准 `led-controller@ee00`。不能只替换旧节点的 compatible：它有多于四个 vendor 子节点，主线按子节点总数和 `led-sources` 解析，会拒绝这份结构。新控制器只有两路标准 LED 子节点，仍保持 `disabled`；没有增加猜测的电源、GPIO 或寄存器初始化。

现有 [leds-qcom-flash.c](../../build/kernel-worktrees/release-7.2.9/drivers/leds/flash/leds-qcom-flash.c)读取父 regmap 的 `0xee04` type 与 `0xee05` subtype。只有 type `0x18` 且 subtype `0x03/0x04/0x07` 受支持，分别选择已有三路/四路布局；其他 subtype 返回 `flash LED subtype ... is not yet supported` 和 `-ENODEV`。尚未取得本机读值，不能根据 compatible 宣布匹配成功，也不增加强制 subtype 的补丁。[上游绑定](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/qcom,spmi-flash-led.yaml)定义了标准属性。

已另准备 [cleanup 索引补丁](../../patches/linux/7.2.9/0004-leds-qcom-flash-cleanup-index.patch)：原驱动在成功计数等于数组长度时读取 `v4l2_flash[leds_count]`，会越界。两处清理改为先检查计数、递减后释放每个槽位，包括 NULL；这份补丁等待统一的新源码准备，不修改冻结 worktree 或默认 release tree pin。

## 最小标准接口接入

当前 release 配置已有 `CONFIG_LEDS_CLASS_FLASH=m`、`CONFIG_LEDS_QCOM_FLASH=m`、SPMI PMIC 支持；`CONFIG_V4L2_FLASH_LED_CLASS` 未启用。本次不改变默认值。独立诊断构建在合入上述补丁后，可配置：

```text
CONFIG_LEDS_CLASS_FLASH=m
CONFIG_LEDS_QCOM_FLASH=m
CONFIG_V4L2_FLASH_LED_CLASS=m
CONFIG_VIDEO_V4L2_SUBDEV_API=y
```

保留已有 SPMI/PMIC 与媒体框架依赖。验证 peripheral type/subtype、父 regmap 和两路接线后，才能在诊断 DT 副本中将新控制器设为 `okay`；旧 vendor 节点继续 disabled。标准 LED class 提供 torch brightness、flash brightness/timeout/strobe/fault；V4L2 wrapper 注册异步 flash subdevice。相机侧仍需标准 fwnode/notifier 关联，不能仅凭注册就宣称曝光同步或 `/dev/v4l-subdev*` 已可用，也不恢复 vendor trigger daemon。[Linux Flash LED 文档](https://docs.kernel.org/leds/leds-class-flash.html)

## 离线组合检查

只对实际参考板组合一次，不改变参考 DTB：

```sh
mkdir -p /tmp/piano-camera-flash-check
cpp -nostdinc -undef -x assembler-with-cpp \
  -I upstream/linux-piano/include linux/dts/piano-camera-flash.dtso \
  > /tmp/piano-camera-flash-check/piano-camera-flash.dts
dtc -@ -I dts -O dtb \
  -o /tmp/piano-camera-flash-check/piano-camera-flash.dtbo \
  /tmp/piano-camera-flash-check/piano-camera-flash.dts
fdtoverlay -i vendor/piano-linux/board.dtb \
  -o /tmp/piano-camera-flash-check/board-flash-disabled.dtb \
  /tmp/piano-camera-flash-check/piano-camera-flash.dtbo
```

本次已完成上述一次参考板组合，结果保留 SID 1、基址 0xee00、通道 1/2 与上述电流/时限，三个诊断涉及的 controller/consumer 均 disabled。结果保存于 `/tmp/piano-camera-flash-check/composition.json`，组合 DTB SHA-256 为 `20fab5813a2d36e2f6512b7b0072a25b8665a253977995ac08eb571a59dcc387`。

cleanup mail patch 的 `git apply --check` 通过；使用现有 release 的实际 `.leds-qcom-flash.o.cmd` 参数、原配置及生成头文件，LLVM 23.1.1 编译修改后的单个 AArch64 C 对象成功，源副本和结果都在 `/tmp/piano-camera-flash-check/`。该配置尚未启用 V4L2 flash wrapper，带 wrapper 的构建留给统一的新内核准备。没有修改冻结/上游工作树、配置或默认源码 pin，也没有全量构建。

以上仅证明源码与 DT 可以准备、组合和编译；实机 type/subtype、点灯、thermal/fault、供电状态与相机同步仍未验证。
