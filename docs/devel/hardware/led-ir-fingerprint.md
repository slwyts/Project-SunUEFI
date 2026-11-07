# Piano 的 RGB 灯、红外与侧指纹

2026-10-08 核对原厂最终 DT、Android 驱动绑定、当前 Linux 源码和已有运行记录。以下三项尚未完成实机功能验证；没有操作发光、发射、指纹录入或认证。

| 功能 | 原厂接口 | 当前 Linux 缺口 |
| --- | --- | --- |
| RGB 通道 | PM8550 SID1 上的 `qcom,pm8350c-pwm`，SPMI LPG，通道 1/2/3 为 red/green/blue | 标准驱动和产品DT已有；补齐SDAM/LPG模块加载后已出现三个标准LED接口。发光、可见位置和用途尚待确认。 |
| 红外发射 | SE15 / SPI `0x89c000` / CS0，`ir-spi`，Android 原始波形字符设备 | 默认构建已接入控制器和上游 `ir-spi-led`（LIRC）；SPI 绑定和光学发射尚未在设备上确认。 |
| 侧指纹 | Goodix，GPIO 控制与高通 QTEE 安全服务 | 供电、初始化、模板枚举和 Keymaster 绑定可用；加密存储和验证已实现、未验证；录入未实现，需要 Linux 无法取得的密码授权令牌，fprintd 未接入，见[录入](#录入)。 |

## RGB 通道

原厂 `/soc/qcom,spmi@c42d000/qcom,pm8550@1/led-controller` 绑定 `qcom-spmi-lpg` / `leds_qcom_lpg`。三个子节点原样指定 `reg=1/2/3`、红/绿/蓝颜色和相应 label。其两个 nvmem 依赖是 **PMK8550 SID0 的 `sdam@8400`、`sdam@8500`**，用于 LPG 通道状态和图案数据，不是相机闪光灯的 `0xee00` 外设。

当前产品 `board.dtb` 已保留上述节点和依赖，节点未写 status，按 DT 规则启用。当前配置已有 `LEDS_QCOM_LPG=m`、`LEDS_CLASS_MULTICOLOR=m`、`NVMEM_SPMI_SDAM=m`；[上游 LPG 驱动](https://github.com/torvalds/linux/blob/master/drivers/leds/rgb/leds-qcom-lpg.c)已经支持该 compatible、通道和标准 LED class，[绑定说明](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/leds-qcom-lpg.yaml)也接受这些字段。**无需再加一个重复 RGB overlay。**

Linux起初没有RGB，LPG的deferred-probe原因为未取得LPG chan SDAM。正确提供者模块名为 `nvmem_qcom_spmi_sdam`；加载它及 `leds_qcom_lpg` 后，platform device绑定 `qcom-spmi-lpg`，出现 `red`、`green`、`blue`，初始brightness为0、max_brightness为255。没有读取SDAM内容或写brightness，尚不证明可见灯的位置或用途。

[通用模块加载文件](../../../linux/bsp/common/etc/modules-load.d/piano-rgb.conf)已写入BSP manifest和完整root stager，也已安装到当前平板；下一次正常启动会按同一策略加载，不添加daemon或重复DT节点。记录在 `内部记录目录 `sdam@7000/sm1510_present@5d` cell 曾因无效位范围阻止其提供者注册；打包器现保留完整原始字节并移除无效位切片，详见[启动设备树处理](../linux/kernel.md#piano-启动配置)。LPG 使用的是 8400/8500 两个 SDAM。

## 红外发射

原厂最终 DT 的子节点是 `/soc/qcom,qupv3_2_geni_se@8c0000/spi@89c000/ir-spi@0`，最大 SPI 时钟 19.2 MHz， Android 绑定 `ir_spi`。GPIO28/29/30/31 分别是 MISO/MOSI/CLK/CS，复用到 QUP2 SE7。原厂 HAL 清单有 `android.hardware.ir-service.example`。

[固定 MiCode 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/media/rc/ir-spi.c)注册 `/dev/ir_spi`，以 32 位 SPI word、1.92 MHz 默认时钟发送用户空间提供的原始位图。它把 `LIRC_SET_SEND_MODE` 用作缓冲区长度设置，并没有标准 rc-core 的脉冲/间隔接口。当前 ROM 模块的 probe 反汇编也确认 32 位和 1.92 MHz；不能据 ioctl 名字把它当成标准 LIRC 驱动。

原厂服务的 `ConsumerIr::transmit` 只把频率和原始脉冲数组转交给 legacy `consumerir_device_t`，底层返回值随后被忽略并返回AIDL成功，不能据此认定发射完成；这与 [AOSP AIDL 服务](https://android.googlesource.com/platform/hardware/interfaces/+/a2a9fa50039d69643527020ab6706b319f0e6c62/ir/aidl/default/main.cpp)一致。编码位于本ROM的 `/odm/lib64/hw/consumerir.qcom.so`（SHA256 `fc9e6f2c7770b0f5eb765af8e6eec17494ba7a081a47051606188bec17164f9a`），不是服务主二进制。已按目录元数据只提取这两个明确文件；没有执行。该模块是ODM目录consumerir草案，但现有属性采集未包含 `ro.hardware.consumerir`/`ro.hardware`，尚未通过运行时maps确认它被选择。

`consumerir_transmit_mi`（VA `0x40f8`）以固定 **1.92MHz** 生成波形：周期采样点数为 `round(1920000 / carrier)`，高电平点数为 `floor(2 × period / 5)`。38kHz请求得到51点周期、20高/31低，名义载波约37647Hz。偶数脉冲段调制载波，奇数间隔段全部为0并复位载波计数器。样本从原生little-endian `u32`的bit31向bit0填入，不做byte-swap，由原厂驱动的32位SPI word接口发送；最终写入长度向上对齐1024字节，填充区清零。累积时间边界减5µs、时间转换与长度上限等细节保存在私有 `ir-encoding-facts.json`，不能照搬成无边界检查的实现。

该HAL只打开 `/dev/ir_spi`、设置传输字节数、写入并关闭，没有GPIO、regulator或额外idle写操作。编码的间隔和填充为低电平，但这不能证明晶体管/光学输出极性，或在无填充的末尾脉冲后控制器最终保持的电平。

默认构建使用上游 `ir-spi-led` 驱动，不加内核补丁。[设备树覆盖](../../../linux/dts/piano-ir-spi-controller.dtso)把控制器转为 `qcom,geni-spi`、时钟名 `se`，GCC ID 123、GIC SPI462、SE7、GPIO28–31 与原厂最终 DT 和当前 `sm8750.dtsi` 对应；DMA 用 GPI2 三 cell 参数 `0/1, 7, QCOM_GPI_SPI`，三条 ICC 路径与同一 QUP2 上的笔充电 I²C 相同。default/sleep pinctrl 位于 `piano_tlmm_ml`，保留原厂 6 mA／2 mA、bias-disable。共享 QUP2 wrapper、GPI2 掩码和 Linux 拥有的 SID `0x423/0x436` 不变，SPI 缓冲区沿用 QUP 父设备的 IOMMU 路由。CS0 子节点保留原厂 `reg = <0>` 和 19.2 MHz `spi-max-frequency`，改为 `ir-spi-led` 并启用。

[`piano-ir.config`](../../../linux/configs/piano-ir.config) 打开 `CONFIG_LIRC` 和 `CONFIG_IR_SPI=m`。旧的 `80-drivers.rules` 禁止 OF 模块自动加载，[`79-piano-ir.rules`](../../../linux/bsp/common/etc/udev/rules.d/79-piano-ir.rules) 只在 SPI 设备出现时加载 `ir_spi`，做法与笔充电芯片相同。`spi_geni_qcom` 仍由触控服务按原有顺序加载，加载时一并探测这个控制器。

标准驱动的波形与原厂编码不同：每个载波周期 16 个位样本，SPI 时钟为载波 × 16，默认占空比 50%。原厂约 40% 占空比可以通过 LIRC 的 `LIRC_SET_SEND_DUTY_CYCLE` 请求，按 16 样本量化为 7/16，约 43.75%。间隔为低电平，与原厂一致。原厂驱动和 HAL 都不控制供电，驱动可选的 `irda_regulator` 因此解析为内核的 dummy regulator，启动日志会提示一次；这与原厂行为等价，不另造 supply。

在平板上按顺序检查：`dmesg | grep -i -e geni -e ir-spi` 确认控制器和发送驱动绑定，`ls /dev/lirc*` 出现设备；`ir-ctl -d /dev/lirc0 -S nec:0x04` 之类的命令发送（`ir-ctl` 在 Debian 的 `ir-keytable` 包中）；用手机摄像头对着平板的红外窗口观察闪烁，再对电视等设备实测。SE 没有被引导程序配置协议时，7.3 的 GENI 驱动会按 QUP wrapper 的 `firmware-name` 自行加载固件；当前 DT 没有这个属性，QUP1 上的触控 SPI 也不需要它。若日志出现 `Failed to read firmware-name` 或 `Invalid proto`，说明 SE7 没有以 SPI 模式交给 Linux，此时给 `qcom,qupv3_2_geni_se@8c0000` 加 `firmware-name`，并随系统提供同版原厂 ROM 的 `qupv3fw.elf`。

[`piano-ir-spi-rc-core.patch`](../../../patches/linux/7.2.9/drafts/piano-ir-spi-rc-core.patch) 是按原厂 1.92 MHz、32 位打包和 40% 占空比编码的另一种实现，只针对 7.2.9，没有进入发布序列。标准驱动在实机上确认可用后删除它；只有标准波形无法被接收设备识别时，才把它移植到 7.3 并让子节点改用 `xiaomi,piano-ir-spi`。

## 侧指纹

HyperOS 4 的 `mfp-daemon` 使用 Goodix HAL、`/dev/xiaomi-fp`
和高通 QTEE。`libQSEEComAPI.so` 通过 `libminkdescriptor.so` 打开
UID 122 的 QSEECOMPAT 服务，再加载名为 **`gfpia`** 的签名安全程序。
原厂进程的映射和文件描述符已确认这条路径，不能套用其他小米平板的
FPC 驱动或 Goodix USB 驱动。

Linux 7.3 的 `qcomtee` 驱动提供标准 `/dev/tee0`。
使用高通公开的 [quic-teec](https://github.com/quic/quic-teec) 客户端，
已在实机取得同一个 QSEECOMPAT 服务，并成功加载、释放由原厂分段文件
还原的完整签名 `gfpia`。程序大小为 11,681,880 字节，QTEE 版本为
5.2.0；安全程序和加载接口均没有修改或绕过签名。

原始 QTEE 共享内存分配器使用 `alloc_pages_exact()`，超过伙伴分配器
支持的最大阶数时会报错。项目的
[`0014-qcomtee-large-shared-memory.patch`](../../../patches/linux/7.3/0014-qcomtee-large-shared-memory.patch)
让大缓冲区使用可回收的 CMA 连续内存，小缓冲区保留原来的路径，
并继续登记和注销安全世界的共享内存桥。
[`piano-cma.dtso`](../../../linux/dts/piano-cma.dtso) 将默认池从 36 MiB
扩大到 256 MiB，保留原厂允许的分配地址范围。这部分内存空闲时仍能
供 Linux 使用，不是永久划走的固件内存。

传感器控制依据[固定 MiCode 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/input/fingerprint/mi_fp/fp_driver.c)
提供供电、复位和中断：原厂 DT 指定 GPIO73 复位、GPIO85 IRQ，
`fp_3v3_vreg-supply` 指向 PM-HUMU L9。认证数据由安全服务处理；
Goodix 客户端使用 `QSEECom_send_modified_cmd()` 发送请求。
[`0017-uio-piano-fingerprint-control.patch`](../../../patches/linux/7.3/0017-uio-piano-fingerprint-control.patch)
提供内核供电、复位和中断控制，设备树位于
[`piano-fingerprint.dtso`](../../../linux/dts/piano-fingerprint.dtso)。
打开 `piano-goodix` UIO 设备时打开供电并释放复位，关闭时停止中断、
拉低复位并归还电源引用；没有 MMIO 映射，也不把中断当成认证结果。
Linux 已完成供电、复位和关闭。

原厂 `pm_humu_l9` 为 3.300 V。现有主线 PLDO 使用不同的离散电压档位；
必须选择不超过原厂上限的 3.296 V。之前上取整到 3.304 V 时，
`regulator_enable()` 尚未返回就发生系统复位；改用较低档位后，
打开与关闭均已正常完成。不要为适应档位把原厂电压上限放宽。

Goodix 客户端的设备信息命令为路由 1、命令 1000，128 字节请求、
64 字节响应，另带传感器共享缓冲区。Linux 能加载并正常关闭安全程序。
QSEECOMPAT 的 lookup 返回 `architecture=2`，命令应使用 64 位地址，
不能按外层固件的 ELF32 封装选择地址宽度。修正后传输层返回成功，
最初响应中的缓冲区状态为 `-1`，原因是小缓冲区来自 4 GiB 以上的伙伴
分配器。该安全程序接受的共享区域必须在 4 GiB 以下；使用默认 CMA
池中的 4 KiB 缓冲区后，传输、应用和缓冲区状态均返回成功，
传感器类型为 5。无需给每条命令申请大缓冲区。

[`0018` 内核修正](../../../patches/linux/7.3/0018-qcomtee-public-buffers-cma.patch)
使应用共享内存使用设备树声明的连续内存池，保留驱动私有小消息的
普通分配方式。地址范围由池的配置决定，不通过伪造指针或用户空间
内存热补丁处理。

原生客户端位于
[`piano-goodix.c`](../../../linux/userspace/fingerprint/piano-goodix.c)，
请求布局和存储服务在
[`piano-goodix-protocol.c`](../../../linux/userspace/fingerprint/piano-goodix-protocol.c)，
由 `tests/unit/test_piano_goodix_protocol.py` 在主机上按原厂 HAL 的偏移检查。
客户端使用固定版本的 Qualcomm MinkIPC／quic-teec 与 QCBOR，由四个发行版
共用的运行时构建器静态编译；来源记录在
[`config/fingerprint.json`](../../../config/fingerprint.json)。
执行 `sudo piano-fingerprint-ctl info` 可查询设备信息，`--help` 不访问设备。
签名程序按固件清单随镜像提供，不包含用户模板或密码。
每条命令运行期间对 `/var/lib/piano-goodix` 加排他锁，同一时间只有一个进程
加载安全程序；Ctrl+C 结束等待后照常取消操作、关闭安全程序并断电。

`sudo piano-fingerprint-ctl initialize` 完成传感器初始化、原厂模型加载和
交互策略设置，再正常关闭传感器。实机返回传感器类型 5、芯片类型 49、
132 行 × 24 列阵列，以及 132 × 16 的导航区域。初始化响应中的芯片类型为零
表示沿用设备查询值，与原厂 HAL 的处理一致，不代表未知芯片。

该设备要求防伪和识别模型，两者均从同版原厂 `libgf_hal.so` 的导出数据符号
提取，属于通用算法参数，不是用户模板。CAP S／T 的选择依据设备返回的标志，
芯片类型 49 使用 `DLWeightLightDubaiU4_12_10_14MisTouch`。
来源库 SHA256、符号、文件偏移、大小和输出校验值全部记录在
[`固件清单`](../../../vendor/firmware/hyperos4-fingerprint/manifest.json)，
不会安装原厂可执行库。

命令都通过公开 QTEE 接口进入原厂签名程序，路由 1。名称取自
`libgf_hal.so` 的 `cmd_id_table`（`gf_strcmd` 使用的表），请求长度和字段偏移
取自同一库中发出该命令的函数：

| 命令 | 原厂名称 | 用途 |
| --- | --- | --- |
| 1000 | `GF_CMD_DETECT_SENSOR` | 查询传感器类型和功能标志；响应 0x70 起的 0x208 字节即 HAL 的 `g_hal_config` |
| 1001 | `GF_CMD_INIT` | 读取传感器参数与校准数据 |
| 1113 | `GF_CMD_TRANSPORT_DATA` | 分块加载防伪／识别模型；每块 511900 字节，逻辑命令 1111 `GF_CMD_INIT_MODEL` |
| 1005 | `GF_CMD_INIT_FINISHED` | 完成初始化 |
| 1109 | `GF_CMD_UPDATE_CONFIG` | 0x6c 字节为 `disable_study`，原厂默认 0 |
| 1014 | `GF_CMD_SET_ACTIVE_GROUP` | 选择分组并载入其模板，0x270 字节 |
| 1015 | `GF_CMD_ENUMERATE` | 列出模板：分组号在 0x70 起、模板号在 0x98 起，各 10 项 |
| 1010 | `GF_CMD_AUTHENTICATE` | 开始验证：分组 0x6c、64 位操作号 0x70、0x78 为 `0xffffffff` |
| 1016 | `GF_CMD_IRQ` | 读取并确认中断，同时取得采集和匹配结果，0xd134 字节 |
| 1086 | `GF_CMD_AUTHENTICATE_FINISH` | 匹配后让安全程序更新模板 |
| 1009 | `GF_CMD_CANCEL` | 结束当前操作，回到待机检测 |
| 1002 | `GF_CMD_EXIT` | 关闭传感器，随后关闭安全程序并归还供电 |

安全程序在 `GF_CMD_IRQ` 响应中返回 `GF_ERROR_*` 码（`libgf_ca` 响应的第一个字），
客户端按名称报告，例如 1006 `GF_ERROR_NOT_MATCH`。

`sudo piano-fingerprint-ctl watch 30` 在限定时间内监听 UIO 中断，最多 120 秒。
收到中断后先通过安全程序确认，再重新启用 IRQ；输出只有事件标志和中断计数，
不保存或输出图像、模板和认证令牌。启动与待机过程已收到中断并完成确认；
这不等于手指检测、匹配或解锁已经完成。Ctrl+C 可正常结束并释放硬件。

### 加密存储

安全程序把模板、认证用户号和模组信息作为“安全对象”保存：
`gf_tee_open_object`（`gfpia.elf` 0x50450）把对象名拼成固定的
`/data/vendor_de/0/fpdata/<名称>`，经 `qsee_fts` 在安全程序内加密并加完整性校验，
再由 QTEE 用旧 GPFS 文件命令 0 读、1 写、2 删、3 改名请求普通世界的
0x7000 监听服务。命令 1014 传入的路径只被复制保存
（`gf_ta_set_active_group`，0x47810），不参与存储。

客户端在加载安全程序后、发出任何命令之前，在本进程内注册这个监听服务
（`CRegisterListenerCBO`，类 ID 87），命令结束时随安全程序一起释放，
不需要常驻服务。注册失败时 `list` 和 `verify` 直接停止，其他命令照常运行但不读写存储。
每次被拒绝的请求在标准错误输出命令号和原样路径，命令结束时输出读、写、删除、
改名和拒绝的次数，用来区分“存储正常但没有模板”和“路径格式与预期不符”。服务只接受上述前缀下的单个文件名（字母、数字、`_`、`-`、`.`，
不以 `.` 开头，最多 96 字节），存到 `/var/lib/piano-goodix/<名称>`；目录为
root 所有的 0700，文件 0600，打开时不跟随符号链接。其他路径一律返回 `EACCES`，
包括 `/persist` 和带 `..` 或子目录的名称；数据区和 persist 区的新式命令 4..11
同样拒绝。文件内容是安全程序加密后的数据，Linux 不解析。Android 的 `userdata`
不会被挂载或访问。安全程序另用 `open()` 直接写 IFAA 的
`/persist/data/fingerprint_version` 和 `/persist/data/ifaa_fplist`，Linux 不为它们
提供存储，写入会失败；初始化时写版本号的调用（0x17338）不检查返回值。

`sudo piano-fingerprint-ctl list 1000` 通过命令 1014 选择 Linux UID 1000 的
分组，再通过 1015 枚举模板。分组号就是 Linux UID，对象名自带分组号，例如
`finger_1000_0.so`、`auth_token_1000.so`。载入分组时安全程序会读取这些对象，
并可能写回 `gf_module_info_<uid>.so` 和模板的 `_bak` 备份副本；这是安全程序自己的数据，
只落在上述目录。最大模板数量取自设备信息，并按响应容量解析。

`sudo piano-fingerprint-ctl prepare-auth` 初始化传感器并完成 Keymaster 到
Goodix 的认证数据绑定。没有录入或认证操作，也不输出绑定数据。
这一步已在重新启动后的 Linux 上独立完成，不依赖 Android 先运行指纹服务。

原厂加载的 64 位 `keymaster64` 报告 API 5.3／TA 5.3000。
同版 `libkeymasterdeviceutils.so` 和 `libqtikeymint.so` 表明，原生服务 151
打开指定安全应用的环境，应用内服务 150 提供命令接口。客户端执行
`0x200`／`0x207` 协商；需要时通过 `0x220e` 取得测试启动的共享参数，
用 `0x220f` 完成 HMAC sharing。请求保留准确 CBOR 长度，不把对齐填充算入协议。

绑定请求仍沿用原厂 CA 的 `0x205, 2` 和 256 字节响应，再通过 Goodix 路由 2、
命令 109 传入签名安全程序。Goodix 必须先加载并初始化；之前在它尚未加载时
查询，会得到另一个错误。只有明确返回 `-10020` 时才初始化测试启动的
sharing key，已有绑定可用时直接使用，避免重置其他客户端的有效会话。
响应的偏移和长度均检查边界，临时参数和加密绑定数据在结束时清零。
没有把密钥、令牌或指纹模板保存到普通文件。

### 验证

`sudo piano-fingerprint-ctl verify 1000 [秒数]` 选择 UID 1000 的分组、发出 1010，
然后等待一次触摸，默认 10 秒，最多 60 秒。中断处理按原厂 `hal_milan_irq`
（0x41390）和 `irq_process`（0x41bc8）：每次中断先用 1016 查询，处理步骤为 4 或 5 时
再查询一次；按下手指且步骤为 2 时再发 1016，由安全程序采集并匹配；
结果为 1064 `GF_ERROR_MATCH_FAIL_AND_RETRY` 时把 0x98 设为 2 后重试。
复位、ESD 和 fargo 错误位，或响应头第 0 字节的复位请求，会让命令停止并提示重新运行，
重新打开 UIO 设备时传感器随之复位。

结果按 `irq_image_enroll_or_authenticate`（0x42308）解释：返回 0 且操作为 1 或 2
是匹配，分组和模板号在 0x80、0x84；1006 `GF_ERROR_NOT_MATCH` 和 1119
`GF_ERROR_ALGO_FAKE` 是不匹配；按压不全、脏污、过快、过慢和掌压会以
`{"event":"retry"}` 提示并继续等待。匹配后发 1086 让安全程序更新模板，
最后发 1009 结束操作。输出只有事件、结果和模板号，退出码 0 表示匹配。
响应 0x2ac 起的 69 字节是安全程序生成的认证令牌，Linux 不使用它，也不复制或输出，
缓冲区在命令结束前清零。验证不调用 Keymaster，令牌因此不带有效签名，
这对 Linux 没有影响。该命令尚未在设备上运行。

安全程序只有收到 1090 `GF_CMD_LOCKOUT` 时才进入锁定，解除锁定（1091）需要授权令牌。
客户端不发 1090，失败次数限制交给之后的 PAM／fprintd 处理。

### 录入

录入未实现，原因在安全程序。`gf_ta_enroll`（`gfpia.elf` 0x45160）在
`g_config+0x60` 非零时检查 1007 请求里 0x76 起的 69 字节 `hw_auth_token_t`：
版本必须等于 0x75 字节，挑战值必须非零且等于 1006 `GF_CMD_PRE_ENROLL` 返回值，
最后用 Keymaster 下发的密钥重新计算 HMAC 并比较，分别对应 1055、1056、1057 错误。
这个配置字节只在 `gf_config_init_by_chip_type` 中写入（0x4b0），值为 1；
密钥由 `gf_set_hmac_key`（0x4f9c0）通过 `qsee_decapsulate_inter_app_message`
解开 Keymaster 的封装数据得到。尚未下发密钥时缓冲区为零，用零密钥构造令牌
就是伪造安全程序要求的授权，项目不这样做。

合法的令牌只来自 Gatekeeper 在核对锁屏密码后的签发。同版 `keymint.mbn`
（SHA256 `e2c4bf764aaf1b1ccb4b784aed21c3db15b2f9e6d1c98d7ab39ebedc89efb362`）中，
`verify_password` 在核对前后两次调用 `gk_modify_user_data`（0x19a8c），
它通过 0x1943c 处的 `sync_sector` 写 Keymaster 的安全存储，每次录入至少写两次。
这超出了“只改指纹安全程序自己的数据”的范围，Linux 目前不提供可写的存储服务。
要在 Linux 录入，只能用一个 Linux 专用的 Gatekeeper 用户号，并提供能写入
Keymaster 存储的服务，这需要维护者决定。在此之前不接入 libfprint 和 fprintd：
接入后 GNOME／KDE 设置里会出现一个永远无法录入的“指纹登录”入口。

随后bde3普通重启发现，虽然modules-load文件已安装，旧modprobe黑名单仍使systemd把LPG记为deny-listed。已仅移除 `leds_qcom_lpg` 这一条，并让完整root stager与通用BSP使用同一份tracked modprobe配置。重新启动标准modules-load后插入LPG并出现三个接口，随后一次完整正常重启也已确认三个接口自动出现。当前记录在 `bde3-rgb-startup.txt` 与 `bde3-rgb-policy-fixed.txt`，不能用之前手动modprobe结果替代启动策略验证。
