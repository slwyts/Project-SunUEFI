# Piano 的 RGB 灯、红外与侧指纹

2026-10-08 核对原厂最终 DT、Android 实际驱动绑定、当前 Linux 源码和已有运行记录。以下三项尚未完成实机功能验证；没有操作发光、发射、指纹录入或认证。

| 功能 | 原厂实际接口 | 当前 Linux 缺口 |
| --- | --- | --- |
| RGB 通道 | PM8550 SID1 上的 `qcom,pm8350c-pwm`，SPMI LPG，通道 1/2/3 为 red/green/blue | 标准驱动和产品DT已有；补齐SDAM/LPG模块加载后已出现三个标准LED接口。实际发光、可见位置和用途尚待确认。 |
| 红外发射 | SE15 / SPI `0x89c000` / CS0，`ir-spi`，Android 原始波形字符设备 | 已确认原厂约40%占空比、低电平间隔和32位打包；控制器标准绑定及LIRC待接入，实际光学输出未验证。 |
| 侧指纹 | `xiaomi,xiaomi-fp` 平台驱动，复位/IRQ/供电及 netlink 事件 | 公开驱动没有本机指纹图像或认证传输接口；不能直接接成 libfprint/fprintd。 |

## RGB 通道

原厂 `/soc/qcom,spmi@c42d000/qcom,pm8550@1/led-controller` 实际绑定 `qcom-spmi-lpg` / `leds_qcom_lpg`。三个子节点原样指定 `reg=1/2/3`、红/绿/蓝颜色和相应 label。其两个 nvmem 依赖是 **PMK8550 SID0 的 `sdam@8400`、`sdam@8500`**，用于 LPG 通道状态和图案数据，不是相机闪光灯的 `0xee00` 外设。

当前产品 `board.dtb` 已保留上述节点和依赖，节点未写 status，按 DT 规则启用。当前配置已有 `LEDS_QCOM_LPG=m`、`LEDS_CLASS_MULTICOLOR=m`、`NVMEM_SPMI_SDAM=m`；[上游 LPG 驱动](https://github.com/torvalds/linux/blob/master/drivers/leds/rgb/leds-qcom-lpg.c)已经支持该 compatible、通道和标准 LED class，[绑定说明](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/leds-qcom-lpg.yaml)也接受这些字段。**无需再加一个重复 RGB overlay。**

实际Linux起初没有RGB，LPG的deferred-probe原因为未取得LPG chan SDAM。正确提供者模块名为 `nvmem_qcom_spmi_sdam`；加载它及 `leds_qcom_lpg` 后，实际platform device绑定 `qcom-spmi-lpg`，出现 `red`、`green`、`blue`，初始brightness为0、max_brightness为255。没有读取SDAM内容或写brightness，尚不证明实际可见灯的位置或用途。

[通用模块加载文件](../../linux/bsp/common/etc/modules-load.d/piano-rgb.conf)已写入BSP manifest和完整root stager，也已安装到当前平板；下一次正常启动会按同一策略加载，不添加daemon或重复DT节点。实际记录在 `private/provisioning/recovery-priority-20261008/a8-rgb-provider-load.txt`。另一个无关的 `sdam@7000/sm1510_present@5d` cell描述导致其提供者注册失败，但LPG使用的8400/8500已成功；不将这个告警掩盖或扩大解释为全部SDAM正常。

## 红外发射

原厂最终 DT 的子节点是 `/soc/qcom,qupv3_2_geni_se@8c0000/spi@89c000/ir-spi@0`，最大 SPI 时钟 19.2 MHz，实际 Android 绑定 `ir_spi`。GPIO28/29/30/31 分别是 MISO/MOSI/CLK/CS，复用到 QUP2 SE7。原厂 HAL 清单有 `android.hardware.ir-service.example`。

[固定 MiCode 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/media/rc/ir-spi.c)注册 `/dev/ir_spi`，以 32 位 SPI word、1.92 MHz 默认时钟发送用户空间提供的原始位图。它把 `LIRC_SET_SEND_MODE` 用作缓冲区长度设置，并没有标准 rc-core 的脉冲/间隔接口。当前 ROM 模块的 probe 反汇编也确认 32 位和 1.92 MHz；不能据 ioctl 名字把它当成标准 LIRC 驱动。

原厂服务的 `ConsumerIr::transmit` 只把频率和原始脉冲数组转交给 legacy `consumerir_device_t`，底层返回值随后被忽略并返回AIDL成功，不能据此认定发射完成；这与 [AOSP AIDL 服务](https://android.googlesource.com/platform/hardware/interfaces/+/a2a9fa50039d69643527020ab6706b319f0e6c62/ir/aidl/default/main.cpp)一致。实际编码位于本ROM的 `/odm/lib64/hw/consumerir.qcom.so`（SHA256 `fc9e6f2c7770b0f5eb765af8e6eec17494ba7a081a47051606188bec17164f9a`），不是服务主二进制。已按目录元数据只提取这两个明确文件；没有执行。该模块是ODM目录唯一consumerir候选，但现有属性采集未包含 `ro.hardware.consumerir`/`ro.hardware`，尚未通过运行时maps确认它被选择。

`consumerir_transmit_mi`（VA `0x40f8`）以固定 **1.92MHz** 生成波形：周期采样点数为 `round(1920000 / carrier)`，高电平点数为 `floor(2 × period / 5)`。38kHz请求得到51点周期、20高/31低，名义实际载波约37647Hz。偶数脉冲段调制载波，奇数间隔段全部为0并复位载波计数器。样本从原生little-endian `u32`的bit31向bit0填入，不做byte-swap，由原厂驱动的32位SPI word接口发送；最终写入长度向上对齐1024字节，填充区清零。累积时间边界减5µs、时间转换与长度上限等精确细节保存在私有 `ir-encoding-facts.json`，不能照搬成无边界检查的实现。

该HAL只打开 `/dev/ir_spi`、设置传输字节数、写入并关闭，没有GPIO、regulator或额外idle写操作。编码的间隔和填充为低电平，但这不能证明晶体管/光学输出极性，或在无填充的末尾脉冲后控制器最终保持的电平。

最小标准适配方向是：将控制器转为 `qcom,geni-spi`、时钟名 `se`，逐项按当前GENI/GPI的clock、interconnect、DMA binding转换资源；子节点用 `ir-spi-led`，配置开启 `LIRC` 和 `IR_SPI`。现有产品DT仍是厂商 `qcom,spi-geni` / `se-clk`，不能只替换子节点compatible。标准驱动每个载波使用16个位样本、SPI时钟为载波×16；请求40%占空比时当前公式得到7/16高位，约43.75%，并不等同于原厂1.92MHz量化波形。移植可复用标准rc-core/LIRC接口及已确认的低电平间隔，但需明确是否保留原厂编码、实际word顺序与约40%占空比。原厂两层均无供电控制，标准驱动的regulator请求仍需按真实无可控供电接口处理，不能虚构supply或默默依赖dummy regulator。相关标准接口见 [IR SPI绑定](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/irled/ir-spi-led.yaml)。

## 侧指纹

原厂 `/soc/xiaomi_fingerprint` 实际绑定 `mi_fp`：GPIO73 复位、GPIO85 IRQ，`fp_3v3_vreg-supply` 指向 PM-HUMU L9，netlink 协议号 25，选择字符串 `p81_n1_142`。ROM 名称清单含 `libgoodixhwfingerprint.so`，但该名称不能独自确定芯片型号或实际认证传输。

[固定 MiCode `fp_driver.c`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/input/fingerprint/mi_fp/fp_driver.c)在非 MTK 配置使用 platform driver，提供电源、复位、IRQ、按键和屏幕事件 ioctl；SPI driver 分支只用于 MTK。本机 ROM 模块的外部调用也只有 GPIO、pinctrl、regulator、input、netlink 等支持接口，没有 SPI、TEE 或 SCM 数据传输调用。当前 Linux 源码没有 `xiaomi,xiaomi-fp` 对应驱动；移植这层只能取得上述控制和事件，不能完成图像读取、录入或认证。

还缺本机实际使用的 HAL/服务及其传感器或安全环境传输入口、芯片协议、以及可接入标准认证用户空间的实现。当前没有证据证明其确切安全环境通道。后续先用原厂 init/VINTF 与加载依赖确认公开接口元数据，不读取指纹模板、密钥、校准私密值或用户数据；不要创建占位的认证设备。

本轮源清单、精确节点和哈希保存在私有分析目录 `private/analysis/piano-led-ir-fp-20261008/`。未修改当前内核源码、配置、构建目录或产品 DT。
