# Piano 的 RGB 灯、红外与侧指纹

本页记录 RGB 通道、SPI 红外和 Goodix 侧指纹的原厂接口与 Linux 实现。指纹通过原生 QTEE 客户端和带本地后端的 libfprint 接入 fprintd、GNOME 与 PAM，支持 Linux 独立录入、持久保存和认证；传感器控制与用户空间后端仍是项目适配，不是安装发行版原版 libfprint 就能使用。

| 功能 | 原厂接口 | 当前 Linux 缺口 |
| --- | --- | --- |
| RGB 通道 | PM8550 SID1 上的 `qcom,pm8350c-pwm`，SPMI LPG，通道 1/2/3 为 red/green/blue | 标准驱动和产品DT已有；补齐SDAM/LPG模块加载后已出现三个标准LED接口。发光、可见位置和用途尚待确认。 |
| 红外发射 | SE15 / SPI `0x89c000` / CS0，`ir-spi`，Android 原始波形字符设备 | 标准 LIRC 发送、超长拒绝及拒绝后再次发送通过；载波、包络和距离没有测量。 |
| 侧指纹 | Goodix，GPIO 控制与高通 QTEE 安全服务 | 已接入 fprintd、GNOME 锁屏与 PAM；依赖本地 UIO 驱动、libfprint 后端和静态客户端。 |

## RGB 通道

原厂 `/soc/qcom,spmi@c42d000/qcom,pm8550@1/led-controller` 绑定 `qcom-spmi-lpg` / `leds_qcom_lpg`。三个子节点原样指定 `reg=1/2/3`、红/绿/蓝颜色和相应 label。其两个 nvmem 依赖是 **PMK8550 SID0 的 `sdam@8400`、`sdam@8500`**，用于 LPG 通道状态和图案数据，不是相机闪光灯的 `0xee00` 外设。

当前产品 `board.dtb` 已保留上述节点和依赖，节点未写 status，按 DT 规则启用。当前配置已有 `LEDS_QCOM_LPG=m`、`LEDS_CLASS_MULTICOLOR=m`、`NVMEM_SPMI_SDAM=m`；[上游 LPG 驱动](https://github.com/torvalds/linux/blob/master/drivers/leds/rgb/leds-qcom-lpg.c)已经支持该 compatible、通道和标准 LED class，[绑定说明](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/leds-qcom-lpg.yaml)也接受这些字段。**无需再加一个重复 RGB overlay。**

Linux起初没有RGB，LPG的deferred-probe原因为未取得LPG chan SDAM。正确提供者模块名为 `nvmem_qcom_spmi_sdam`；加载它及 `leds_qcom_lpg` 后，platform device绑定 `qcom-spmi-lpg`，出现 `red`、`green`、`blue`，初始brightness为0、max_brightness为255。没有读取SDAM内容或写brightness，尚不证明可见灯的位置或用途。

LPG 和 SDAM 通过设备树的硬件别名自动加载，不需要模块列表或守护进程。`sdam@7000/sm1510_present@5d` cell 的无效位切片由打包器移除，保留原始字节，详见[启动设备树处理](../linux/kernel.md#piano-启动配置)。LPG 使用的是 8400/8500 两个 SDAM。

## 红外发射

Piano 的发送器通过标准 rc-core/LIRC 接口接收脉冲与间隔，设备为
`/dev/lircN`。无需 Android HAL 或常驻服务，`ir-ctl` 由 v4l-utils 提供。
发送时可用 `ir-ctl -d /dev/lirc0 --carrier=38000 --send=remote.txt`，文件使用
ir-ctl 的 `pulse` / `space` 文本格式。产品内核上 `ir-ctl` 的 NEC 发送返回成功，
超过 1024 段的请求返回 `EINVAL`，一秒长脉冲返回 `EMSGSIZE`，随后短报文仍能
发送成功。载波、包络、距离和发送后的光学熄灭状态还需要测量。

原厂最终设备树将发送器放在
`/soc/qcom,qupv3_2_geni_se@8c0000/spi@89c000/ir-spi@0`，即
SE15 / QUP2 SE7 / CS0。GPIO28/29/30/31 分别为 MISO/MOSI/CLK/CS，
子设备最大时钟为 19.2 MHz。
[原厂驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/media/rc/ir-spi.c)
使用私有 `/dev/ir_spi` 位图接口。Linux 适配保留原厂接线与波形量化，改用
`xiaomi,piano-ir-spi` 匹配标准 ir-spi 驱动，不要求原厂未描述的供电开关。

原厂 `/odm/lib64/hw/consumerir.qcom.so` 的 SHA256 为
`fc9e6f2c7770b0f5eb765af8e6eec17494ba7a081a47051606188bec17164f9a`。
其固定 1.92 MHz 采样时钟、约 40% 默认占空比、原生 32 位 MSB-first
字序和低电平间隔由编码器保留。载波接受 30–56 kHz，并支持 LIRC 占空比设置。
38 kHz 请求量化为 51 点周期、20 点高电平，名义载波约 37647 Hz。
累计时间决定采样边界，不沿用原厂减 5 µs 的时间修正。

每次请求须为正值、奇数段、最多 1024 段。缓冲区在完整零 word 后对齐到
1024 字节，包含填充最多 32 KiB，约能容纳 136 ms 报文；超长返回 `EMSGSIZE`。
当前 GPI 只接受单个 DMA 段，不能把长报文简单拆成多个 SPI transfer 后
假定时钟连续。空调等长报文需要先补齐 GPI 的多 TRE 连续传输和完成事件处理。

共享 [`设备树`](../../../linux/dts/piano-ir-spi-controller.dtso)沿用原厂
GCC ID123、GIC SPI462、GPI2 SE7 和 IOMMU 路由。运行与休眠的差异只在
GPIO29：idle/sleep 状态将 MOSI 主动拉低，其余引脚沿用原厂 GPIO 休眠配置。
GENI 驱动在消息开始时选择 default，在队列空闲且 SE 确认停止后选择 idle；
自动休眠选择相同的低电平状态。IR 客户端不直接抢占控制器引脚。

发送超时后，GENI 必须等两路 GPI 通道确认终止才返回 SPI 核心，避免核心
提前解除仍被 DMA 使用的缓冲区映射。终止失败时保留消息、映射、时钟和引脚，
继续恢复；不把超时当成硬件已停止。以上改动分别在 Linux 7.3
[`series.json`](../../../patches/linux/7.3/series.json) 的 0025–0027 中维护，
`RC_CORE`、`LIRC` 和 `IR_SPI` 配置由共享平台片段启用。

## 侧指纹

旧 MinkIPC／quic-teec 客户端已完成独立凭据创建、指纹录入、模板保存和重新打开后的验证。
实机录入右食指后，关闭并重新打开客户端仍能匹配；未录入的左食指返回不匹配，
不是等待超时。Linux 使用自己的凭据和模板目录，不复制 Android 已录入的指纹。
自包含 Linux TEE 客户端保留同一业务实现，实机验证范围如下：

| 功能 | 旧 MinkIPC／quic-teec 路径 | 自包含 Linux TEE 路径 |
| --- | --- | --- |
| 加载签名程序、查询设备并关闭 | 实机完成 | 单次 `info` 成功，1.06 秒；`sensor_type=5`、`command_address_bits=64`、`device_info_ok=true` |
| 初始化、模型加载和 Keymaster 绑定 | 实机完成 | 产品内核及匹配固件上初始化成功，sensor 5、chip 49，三份模型加载完成 |
| Gatekeeper 凭据、HAT 和授权录入 | 实机完成 | 源码保留，未取新令牌、未录入、未调用 RPMB 后端 |
| 密封模板持久化、重开后匹配与不匹配 | 实机完成 | 源码保留，未采样或匹配 |

新产品内核及匹配固件已完成 `info`、`initialize` 和 fprintd 设备枚举；
新传输路径的录入、模板重开及匹配／不匹配仍缺实机复测，不能沿用旧客户端的完整认证结果。

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

### Linux TEE 对象接口

QTEE 使用通用 Linux TEE uAPI 的对象调用，不是 GlobalPlatform（GP）会话协议。
`TEE_IOC_VERSION` 在 Piano 返回 `impl_id=4`、`impl_caps=0`、`gen_caps=16`，
分别对应 `TEE_IMPL_ID_QTEE` 和 `TEE_GEN_CAP_OBJREF`；没有声明 `TEE_GEN_CAP_GP`。
[Linux 7.3 的头文件](https://github.com/torvalds/linux/blob/v7.3/include/uapi/linux/tee.h)
已经包含 `TEE_IOC_OBJECT_INVOKE`、`UBUF`、`OBJREF`，
[QTEE 驱动](https://github.com/torvalds/linux/blob/v7.3/drivers/tee/qcomtee/call.c)
的 `qcomtee_ops` 实现 `object_invoke_func`、`supp_recv` 和 `supp_send`，
没有实现 `open_session` 或 `invoke_func`。不能将对象 ID 当作 GP session，
也不能用 `TEE_IOC_OPEN_SESSION`／`TEE_IOC_INVOKE` 替换对象调用。

| 用途 | 已有内核接口 | QTEE 的参数含义 |
| --- | --- | --- |
| 调用服务或签名程序 | `TEE_IOC_OBJECT_INVOKE` | 对象 ID、操作号、输入／输出缓冲区和对象引用；`TEE_OBJREF_NULL` 作为调用目标表示 root object |
| 分配可映射共享内存 | `TEE_IOC_SHM_ALLOC`、`mmap` | 分配返回的共享内存 ID 以 memory `OBJREF` 传入，不能用用户地址冒充物理地址 |
| 文件服务、凭据等回调 | `TEE_IOC_SUPPL_RECV`、`TEE_IOC_SUPPL_SEND` | 同一 TEE context 中的 callback object 和 request ID，通过 meta 参数收发 |
| 释放远端引用 | `TEE_IOC_OBJECT_INVOKE` | QTEE 保留操作 `65535`，由驱动释放该 context 中的对象引用 |

对象引用的 TEE／USER／MEM 标志和回调 meta 布局属于 QTEE 对通用接口的使用方式，
不是所有 TEE 实现都接受的协议。上游[接口说明](https://docs.kernel.org/tee/qtee.html)
同时描述了 direct invocation 和 callback request。
[`piano-goodix-tee.c`](../../../linux/userspace/fingerprint/piano-goodix-tee.c)
直接使用这些 ioctl，处理远端对象、内存对象和 callback object，不链接 MinkIPC 或 quic-teec。
它在创建 root object 时检查 implementation 和 capability，拒绝只有 GP 会话的设备；
失败时关闭 fd 并回收内存，fd 带 `O_CLOEXEC`。这项检查不会加载安全程序或修改安全存储。

每次传给 QTEE 的本地 callback 都有单独 ID 和引用，收到内核释放通知或关闭 context 后回收；
输入／输出缓冲区和对象引用分别转换，不把 GP memory reference 当作 QTEE memory object。
回调输出缓冲区只属于该次调用，保持到 `SUPPL_SEND` 返回，再清零释放。
传输错误不自动重发，也不假定 QTEE 未保留 callback；剩余引用在关闭 context 后回收。
关闭传感器及安全程序、发出文件监听释放后才关闭 root。回调线程只有阻塞在 `SUPPL_RECV`
时允许取消，执行文件操作、发送回复或持有用户空间锁时不可取消；随后等待线程退出。
共享页由标准 TEE 内存引用保护，Linux 用户空间解除映射不绕过内核／安全世界的引用计数。

### 共享内存与传感器控制

原始 QTEE 共享内存分配器使用 `alloc_pages_exact()`，超过伙伴分配器
支持的最大阶数时会报错。项目的
[`0014-qcomtee-large-shared-memory.patch`](../../../patches/linux/7.3/0014-qcomtee-large-shared-memory.patch)
让大缓冲区使用可回收的 CMA 连续内存，小缓冲区保留原来的路径，
并继续登记和注销安全世界的共享内存桥。
[`piano-cma.dtso`](../../../linux/dts/piano-cma.dtso) 将默认池从 36 MiB
扩大到 256 MiB，保留原厂允许的分配地址范围。这部分内存空闲时仍能
供 Linux 使用，不是永久划走的固件内存。

打包时停用没有主线消费者的 Android 动态可回收池，避免它们先占满
4 GiB 以下的可用区，导致默认池创建失败并退回 32 MiB。固定分配、
`no-map` 固件保留区和实际用于 FastRPC 的 ADSP 池不作回收。

传感器控制依据[固定 MiCode 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/input/fingerprint/mi_fp/fp_driver.c)
提供供电、复位和中断：原厂 DT 指定 GPIO73 复位、GPIO85 IRQ，
`fp_3v3_vreg-supply` 指向 PM-HUMU L9。认证数据由安全服务处理；
Goodix 客户端使用 `QSEECom_send_modified_cmd()` 发送请求。
[`0017-uio-piano-fingerprint-control.patch`](../../../patches/linux/7.3/0017-uio-piano-fingerprint-control.patch)
提供内核供电、复位和中断控制，设备树位于
[`piano-fingerprint.dtso`](../../../linux/dts/piano-fingerprint.dtso)。
打开 `piano-goodix` UIO 设备时打开供电并释放复位，关闭时停止中断、
拉低复位并归还电源引用；没有 MMIO 映射，也不把中断当成认证结果。
原生客户端完成操作后关闭传感器及安全程序。桌面接口范围见下文的
[Gatekeeper 与桌面认证](#gatekeeper-与桌面认证)。

UIO 仍是过渡路径。对照 [Linux 上游提交 `aacb77975041`](https://github.com/torvalds/linux/tree/aacb7797504183a90d66b19b1efb555cfe9b3eec)
的设备模型，内核资源接口可以复用，但没有能直接替换这条控制路径的 QTEE 指纹 consumer：

| 已有机制 | 可复用的部分 | 尚未连接的部分 |
| --- | --- | --- |
| firmware、regulator、GPIO、IRQ、runtime PM | 内核驱动可用 `request_firmware()`、`devm_regulator_get()`、`devm_gpiod_get()`、`devm_request_threaded_irq()` 和 `pm_runtime_resume_and_get()` 管理板级资源 | 这些函数不负责选择 `gfpia` 对象、确认 1016 握手或跟踪用户空间认证操作；资源 API 的存在不等于已有传感器驱动 |
| [TEE client 总线和 API](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/include/linux/tee_drv.h) | `tee_client_driver` 按 TA UUID 匹配；[OP-TEE](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/drivers/tee/optee/device.c) 提供对应枚举实例 | QTEE 不枚举 `tee_client_device`；通用 `tee_client_open_session()`／`tee_client_invoke_func()` 也不是 QTEE 对象调用的内核入口 |
| [QTEE 内核对象层](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/drivers/tee/qcomtee/qcomtee_object.h) | `qcomtee_object_do_invoke()`、`qcomtee_object_get_service()` 和内核 callback 可承载 consumer 与安全服务的通信 | 声明仍在驱动内部，未导出供独立 consumer 模块使用；[primordial object](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/drivers/tee/qcomtee/primordial_obj.c) 只实现内存映射、yield 和 sleep，不提供传感器资源服务 |
| [QSEECom auxiliary client](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/drivers/firmware/qcom/qcom_qseecom.c) | 以已加载应用的 ID 建立内核子设备 | `qcom_qseecom_apps` 只有 `qcom.tz.uefisecapp`，不是当前 QSEECOMPAT 对象的固件加载或传感器绑定模型 |
| [pwrseq](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/drivers/power/sequencing/core.c)、[device links](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/Documentation/driver-api/device_link.rst) | 内核 consumer 可请求电源时序，并以 `DL_FLAG_PM_RUNTIME` 维持资源依赖 | 不会将一个用户空间 TEE object 自动变成 consumer，也不传递 IRQ 确认；[`power/control=on`](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/Documentation/ABI/testing/sysfs-devices-power) 是设备级策略，不是随 fd 关闭释放的操作引用 |

具体边界在 [`qcomtee/call.c`](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/drivers/tee/qcomtee/call.c)：
`qcomtee_context_add_qtee_object()`／`qcomtee_context_find_qtee_object()` 使用各 context 的 ID 表；
`qcomtee_probe()` 注册通用 TEE 设备，没有传感器 DT、供电或 IRQ 关联。
本地 `open_client()` 则先通过用户空间 loader 加载 TA，再单独打开控制器；
`run_operation()` 在 1016 的阶段交换结束后才重新启用 IRQ。
另开一个内核 context 不能直接使用这些用户空间对象 ID，单靠 runtime PM 也无法判断交换何时结束。

可以据此设计内核 consumer，但还需明确 QTEE 内核对象调用入口、板级设备与 TA 的关联，
以及业务仍在用户空间时 IRQ 完成和电源引用如何传递；现有上游实现尚未提供这套连接。
将文件移到 `drivers/misc/` 不能补齐它：[misc 核心](https://github.com/torvalds/linux/blob/aacb7797504183a90d66b19b1efb555cfe9b3eec/drivers/char/misc.c)
的 `misc_open()` 只是切换到驱动自带的 `file_operations`，并未定义指纹操作。
GPIO character-device API 只管理引脚，evdev 也没有 QTEE 握手完成语义；拼接它们仍会把硬件时序留给用户空间。
本机总线由安全世界管理，不声明为 AP 可访问的 spidev 或 USB 设备，也不新增 misc 私有协议冒充标准接口。

原厂 `pm_humu_l9` 为 3.300 V。现有主线 PLDO 使用不同的离散电压档位；
必须选择不超过原厂上限的 3.296 V。之前上取整到 3.304 V 时，
`regulator_enable()` 尚未返回就发生系统复位；改用较低档位后，
打开与关闭均已正常完成。不要为适应档位把原厂电压上限放宽。

Goodix 客户端的设备信息命令为路由 1、命令 1000，128 字节请求、
64 字节响应，另带传感器共享缓冲区。Linux 能加载并正常关闭安全程序。
QSEECOMPAT 的 lookup 返回 `architecture=2`，命令应使用 64 位地址，
地址宽度按服务返回值选择；当前签名固件本身也是 ELF64。修正后传输层返回成功，
最初响应中的缓冲区状态为 `-1`，原因是小缓冲区来自 4 GiB 以上的伙伴
分配器。该安全程序接受的共享区域必须在 4 GiB 以下；使用默认 CMA
池中的 4 KiB 缓冲区后，传输、应用和缓冲区状态均返回成功，
传感器类型为 5。无需给每条命令申请大缓冲区，也无需为这个查询启动
文件／时间监听服务。

[`0018` 内核修正](../../../patches/linux/7.3/0018-qcomtee-public-buffers-cma.patch)
使应用共享内存使用设备树声明的连续内存池，保留驱动私有小消息的
普通分配方式。地址范围由池的配置决定，不通过伪造指针或用户空间
内存热补丁处理。

原生客户端位于
[`piano-goodix.c`](../../../linux/userspace/fingerprint/piano-goodix.c)，
使用标准 Linux TEE 对象传输与固定版本的 QCBOR，CLI 由共享运行时构建器
静态编译；来源记录在
[`config/fingerprint.json`](../../../config/fingerprint.json)。
执行 `sudo piano-fingerprint-ctl info` 可查询设备信息，`--help` 不访问设备。
签名程序按固件清单随镜像提供，不包含用户模板或密码。

libfprint 在自己的构建中直接编译同一份后端和依赖源码，不再通过
`piano-goodix.pc` 链接平台私有 `libpiano-goodix.a`。源码归档和补丁由运行时清单校验，
Debian、SteamOS 和 Nyarch 各自使用目标发行版的头文件和库编译。
Debian 构建依赖留在独立 rootfs，要求 libc／GLib 等开发包匹配目标运行库版本，
依赖由 `dpkg-shlibdeps` 生成；安装前以目标 loader 核对新库和 fprintd 的符号解析。
Debian 的 `SOURCE.json` 同时校验源码输入、目标 ABI、后端源码归档、二进制摘要、包身份和
`Depends`；命中时不再创建构建 rootfs 或编译，仍执行目标链接检查与 APT 依赖模拟。
Arch 系缓存同时核对配方摘要和运行库版本，不把另一套 ABI 的缓存直接安装。
这仍是本地 libfprint 包；源码补丁进入仓库不等于上游已收录，更不等于发行版原版包
已经提供此设备后端。传感器控制仍使用 UIO，原厂签名程序的业务命令和 HAT 协议没有改变。
上游提交还需解决 UIO 控制模型、Gatekeeper／共享 RPMB 的录入策略及固件再分发授权。
libfprint 的 [`HACKING.md`](https://gitlab.freedesktop.org/libfprint/libfprint/-/blob/80a4b5ec612892c5c056c48dddaa561452cf37ec/HACKING.md)
不接受专有驱动集成；签名 TA 与模型是否符合其固件边界，也需维护者核对，不能凭源码开源或 OTA 摘要宣称符合。

签名程序与模型共用与内核 firmware loader 相同的搜索顺序：先读取已有的
`/sys/module/firmware_class/parameters/path`，再查 `/lib/firmware/updates/<内核版本>/`、
`/lib/firmware/updates/`、`/lib/firmware/<内核版本>/` 和 `/lib/firmware/`。
因此 `firmware_class.path=/run/firmware` 也供客户端使用；该目录须在切换根系统后保留。
客户端只读取此标准参数，不写 sysfs，也不要求固件必须位于 rootfs。
TA 仍由安全世界验证原厂签名，模型保留现有大小和加载协议检查。拒绝符号链接、非普通文件和
不可访问的高优先级文件，不用低优先级副本掩盖这些错误。

原生 API 位于 [`piano-goodix.h`](../../../linux/userspace/fingerprint/piano-goodix.h)，
命令行与 libfprint 后端共用录入、验证、枚举和删除实现。以下以 Linux UID 1000
和右食指为例；手指名称沿用 libfprint，也可用 1–10：

```sh
sudo piano-fingerprint-ctl enroll 1000 right-index-finger
sudo piano-fingerprint-ctl verify 1000 right-index-finger
sudo piano-fingerprint-ctl list 1000
sudo piano-fingerprint-ctl delete 1000 right-index-finger
```

录入默认限时 120 秒，验证默认 30 秒，可在对应命令末尾指定秒数。Ctrl+C、
SIGTERM 和 API 取消请求会先完成安全程序的取消与关闭，再返回；同步 QTEE 调用
只能在返回后检查取消和时限。已保存的模板不会因随后收到取消而被回滚。

`sudo piano-fingerprint-ctl initialize` 完成传感器初始化、原厂模型加载和
交互策略设置，再正常关闭传感器。实机返回传感器类型 5、芯片类型 49、
132 行 × 24 列阵列，以及 132 × 16 的导航区域。初始化响应中的芯片类型为零
表示沿用设备查询值，与原厂 HAL 的处理一致，不代表未知芯片。

该设备要求防伪和识别模型，两者均从同版原厂 `libgf_hal.so` 的导出数据符号
提取，属于通用算法参数，不是用户模板。CAP S／T 的选择依据设备返回的标志，
芯片类型 49 使用 `DLWeightLightDubaiU4_12_10_14MisTouch`。
提取器 [`import_ota.py`](../../../tools/firmware/import_ota.py) 按 ELF 导出符号查找模型，
不会安装原厂可执行库。现有[`固件清单`](../../../vendor/firmware/hyperos4-fingerprint/manifest.json)
记录 ROM、输出大小和校验值，未记录来源库摘要和逐模型偏移；不能将输出哈希当作这些来源字段。
固件和模型的来源记录也不是再分发授权，许可范围见
[`固件说明`](../../../vendor/firmware/hyperos4-fingerprint/README.md)。

本机原厂 HAL 的构建目标为 `xiaomi_P81_GF3656ZSCB_qsee5_sm8750_64bit_aidl_2025`。
`gf_hal_show_version`（`0x1ed64`）给出包版本 `V1.0.6.0.0` 和软件版本
`1.0.6.0.2.0`。当前国际版库与固件清单中的国内版库整文件摘要不同，但按 ELF
导出符号提取的三份模型逐字节相同，因此不需要替换 Linux 模型。两份完整签名
`gfpia` 的可加载代码和数据仅有三处构建时间字符串不同，另有对应签名元数据差异。
这些静态比对不代替实际录入或认证测试。

协议顺序如下，所有命令都通过公开 QTEE 接口进入原厂签名程序：

| 命令 | 功能 |
| --- | --- |
| 1000 | 查询传感器类型和功能标志 |
| 1001 | 读取传感器参数与校准数据 |
| 1113 | 分块加载防伪／识别模型；每块 511900 字节，逻辑模型命令为 1111 |
| 1005、1109 | 完成初始化并应用交互策略 |
| 1009 | 进入待机检测 |
| 1016 | 读取并确认传感器中断 |
| 1002 | 关闭传感器，随后关闭安全程序并归还供电 |

`sudo piano-fingerprint-ctl watch 30` 在限定时间内监听 UIO 中断，最多 120 秒。
事件接口位于 [`piano-goodix-events.c`](../../../linux/userspace/fingerprint/piano-goodix-events.c)，
由现有客户端调用；它将传输错误与安全程序返回的采集状态分开，保留事件包推进
阶段 4、5，完成后才重新启用 IRQ。输出包括事件标志、中断计数、操作、阶段和
应用状态，不保存或输出图像、模板 ID 和认证令牌。普通阶段 2 保留原包继续一次；
导航阶段 8 清除已消费的导航区后继续，不生成键盘或认证事件。匹配重试状态 1064
按原厂协议重新交换，每次调用只发送一条命令，供调用者检查取消和时限。
`watch` 只观察事件；`enroll` 根据设备功能标志选择普通或扩展采集包。
收到认证成功的安全程序响应时，诊断客户端也会取消，
不代替真正认证客户端执行收尾或报告匹配成功。超时、Ctrl+C 和错误
退出都先执行真实取消，再关闭安全程序及 UIO。这里的时限在每条同步 QTEE 命令
返回后检查，不能中断一条尚未返回的安全调用。

上述字段来自原厂 Milan 函数表 `gf_hal_milan_function_customize`（`0x3f778`）：
取消函数 `0x40188` 发送命令 1009、`0x70` 字节；IRQ 函数 `0x41390` 发送
1016、`0xd134` 字节，设置 `+0x98=1`、`+0xa8=512`、`+0x2c5=0x02000000`。
响应的 `+0x6c` 是事件位图、`+0x70` 是操作、`+0x98` 是处理阶段。
`0x415a4–0x41610` 在阶段 4、5 保留原包再次调用 1016。阶段 2 的普通路径同样
保留原包继续 1016；特定录入模式将包扩至 `0x10e9e0` 字节。阶段 8 在处理导航
通知后清零 `+0x2f4` 起 `0x5a20` 字节，再继续交换。原生状态机处理普通阶段 2、
导航交换和设备要求的扩展录入包，匹配由原厂签名安全程序完成。
`+0x2c5` 是输出硬件认证令牌（HAT）中的大端 `authenticator_type`，数值表示指纹类型 2，
不是可替代录入授权的令牌。`libgf_ca.so:gf_ca_invoke_command`（`0x4170`）将
64 字节响应中的第一个字作为应用状态返回，第二个字单独表示共享缓冲区错误；
不能把所有非零应用状态都当成 QTEE 传输失败。

每个 Linux 用户的数据放在 `/var/lib/piano-goodix/<uid>/`：

```text
gatekeeper/   独立随机凭据及安全程序返回的凭据句柄
fpdata/       Goodix 安全程序密封的模板与算法文件
profile       Linux UID、分组、安全用户 ID（SID）和手指名称到模板 ID 的映射
```

目录权限为 0700，文件为 0600，均由 root 管理。`list` 在命令 1014 选择该用户分组后，
通过 1015 核对安全程序中的模板，只返回 Linux 元数据登记且仍存在的手指。
保存中断时也只恢复已记录的待提交 ID，不收集或删除未知模板。

`storage-probe --state-dir <empty-directory> <group>` 显式启动受限 FS/GPFS
服务，初始化传感器并选择分组，随后关闭安全程序和服务。目录必须是新建的
Linux 空目录；服务将安全程序使用的虚拟路径映射到这里，不访问原厂目录。
初始化会在该目录生成校准文件，所以这是带本地写入的探测，不是只读查询。
命令不复制模板、不录入、不认证，也不执行删除或重命名。

文件服务位于 [`piano-goodix-storage.c`](../../../linux/userspace/fingerprint/piano-goodix-storage.c)。
普通 FS 提供只读打开、关闭和错误状态；GPFS 提供读取、按偏移写入、备份、重命名和
删除。安全程序的 `/data/vendor_de/0/fpdata/` 虚拟路径映射到所选 Linux 用户的
`fpdata/`，拒绝目录穿越、符号链接、硬链接和外部文件描述符。写入保留未覆盖的数据，
要求备份时先落盘同目录 `.bak`，再原子替换主文件并同步文件和目录。
这些回调随原生操作建立和释放，不另加指纹守护进程。

FS／GPFS 布局取自公开 Qualcomm listener 头文件，仅保留本后端使用的消息。
它们在 [`piano-goodix-storage-protocol.h`](../../../linux/userspace/fingerprint/piano-goodix-storage-protocol.h)
中按原始尺寸描述，保留来源与许可，不依赖外部 listener 服务或 Mink 头文件。

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
绑定临时参数与 HAT 不落盘；独立 Linux 凭据和密封模板按上述目录分别保存。

GPFS 与 RPMB 都是签名程序要求的回调，但用途不同：前者保存 Linux 目录中的密封模板，
后者访问同机安全存储，不能用另一个普通目录替代。

录入和认证的入口布局已由原厂 `libgf_hal.so` 静态确认，均使用路由 1：

| 操作 | 函数与命令 | 共享包内容 |
| --- | --- | --- |
| 生成录入挑战 | `gf_hal_common_pre_enroll`，`0x2115c`，1006／`0x78` 字节 | 挑战位于 `+0x70` |
| 开始录入 | `gf_hal_common_enroll`，`0x2161c`，1007／`0xbc` 字节 | 分组 `+0x6c`，真实的 69 字节 HAT 位于 `+0x76` |
| 结束录入 | `gf_hal_common_post_enroll`，`0x219b4`，1008／`0x6c` 字节 | 结束该次录入流程 |
| 开始认证 | `gf_hal_common_authenticate`，`0x21a18`，1010／`0x80` 字节 | 分组 `+0x6c`，操作 ID `+0x70`，`+0x78=-1` |
| 保存模板 | `gf_hal_save`，`0x20a74`，1012／`0x78` 字节 | 分组、模板 ID 位于 `+0x6c`、`+0x70` |
| 删除模板 | `gf_hal_common_remove`，`0x21f34`，1013／`0xcc` 字节 | 分组、模板 ID 位于 `+0x6c`、`+0x70`；`+0xc8` 为删除后的剩余总数 |

成功结果由 Milan 结果分发函数 `0x42308` 判断：应用状态为零后，再区分操作。
操作 1、2 的认证结果携带分组、模板 ID（`+0x80`、`+0x84`）及 HAT
（`+0x2ac`，69 字节）；`0x42774` 将这些字段交给认证成功回调。
`gf_hal_common_authenticate_success`（`0x248b4`）使用同样布局。录入操作 0
则读取剩余次数 `+0x88`，归零后必须经 1012 保存，不能仅凭手指中断或
进度归零报告录入成功。客户端在 1012 成功、Linux 手指映射落盘且完成 1008 后
才返回录入成功。删除只使用该 UID 已登记的模板 ID，1013 成功后再用 1015 确认
目标消失，最后更新 Linux 映射，不执行全局清空。

匹配重试状态 1064（`GF_ERROR_MATCH_FAIL_AND_RETRY`）需要将 `+0x98` 的
64 位值设为 2 并继续 1016。认证成功还需完成原厂的策略更新 1109，以及
`GF_CMD_AUTHENTICATE_FINISH`（1086、`0x148` 字节）；后者不是可省略的日志命令。
原生验证只有完成这两步才返回匹配成功，并核对分组、已登记模板 ID、SID 和挑战。
未匹配返回 `-EACCES`，与采集质量重试、取消和超时分别处理。
认证操作 1、2 收到图像结果状态 1006 或 1119 时，按原厂
`gf_hal_common_authenticate_not_match`（`0x24c98`）读取同一 1016 返回包的
`+0x79` 字节：非零表示误触采集提示（acquired 1028），继续当前认证；
为零才作为未匹配处理。该分支不改变匹配阈值或成功结果的身份校验。
操作 4（FIDO 认证）的原厂路径同样执行 1086，但未作为本项目的认证入口。

### Gatekeeper 与桌面认证

原厂 `libqtigatekeeper.so` 与 `libkeymasterdeviceutils.so` 表明，Gatekeeper 与
上述 Keymaster 共用 service 151 → `keymaster64` → service 150 链路。
本机 5.3 版本使用现代 CBOR 协议：请求是 32 位命令号后接准确长度的 CBOR，
响应是有符号状态、32 位长度和 CBOR。正状态值表示需等待的重试时间，不是
传输错误。版本命令 `0x200` 和握手 `0x207` 仍用各自的固定二进制格式。

| 操作 | 命令 | 请求 CBOR 字段 |
| --- | --- | --- |
| 建立独立凭据 | `0x21001` | `3`：UID，`8`：新凭据；更换时另带 `5`：旧 handle、`7`：旧凭据 |
| 校验凭据 | `0x21002` | `3`：UID，`4`：挑战，`6`：handle，`9`：凭据 |

Linux 用 `0x40000000 | LinuxUID` 作为 Gatekeeper UID 和 Goodix 分组，
接受的 Linux UID 范围为 `0`–`0x0fffffff`。凭据是客户端生成并持久保存的随机值，
不是用户的登录口令；已完成的凭据直接复用，不重新创建或替换 SID。
不读取或复制 Android 文件系统中的密码句柄和指纹模板，也不运行 Android gatekeeperd。

录入用 Goodix 1006 返回的挑战执行 Gatekeeper 校验，再将其
69 字节 HAT 原样交给 Goodix 1007。令牌中的类型和时间戳保留网络字节序，MAC
必须为 32 字节；不能用 Linux 密码校验或自行生成的 HMAC 代替。
原厂 Keymaster 绑定、独立凭据创建、挑战校验和 Goodix 授权录入均已在实机完成。

签名程序自身也检查录入令牌，不只是 Linux 客户端检查包格式。
针对固件清单中的 `gfpia.elf`，动态符号表和可执行段给出以下位置；该文件的
可执行 `PT_LOAD` 从文件偏移 `0x1000` 映射到虚拟地址 `0`，所以下表地址加
`0x1000` 即该指令的文件偏移。

| 符号与虚拟地址 | 行为 |
| --- | --- |
| `gf_config_init_by_chip_type`，`0x420`，写入点 `0x4b0` | 将 `g_config+0x60` 设为 1，启用正常配置的令牌检查 |
| `gf_ta_enroll`，`0x45160`，检查段 `0x45320–0x4538c` | 按配置检查 HAT 版本、挑战，再复制 69 字节令牌，将 MAC 字段清零并计算期望 HMAC；比较原请求中的 32 字节 MAC |
| `gf_ta_enroll`，错误段 `0x45494–0x454f4`、`0x45390–0x453c0` | 版本、挑战、MAC 错误分别返回 1055、1056、1057；MAC 错误日志为 `token authenticate failed` |
| `gf_generate_hmac_enroll`，`0x4fe30` → `gf_generate_hmac`，`0x4ff30` | 对令牌前 37 字节计算 HMAC，将结果写入后 32 字节 |

这些指令可以在不调用安全世界的情况下从仓库固件只读复核；它们不证明每种固件配置
都强制检查，但足以说明不能把正常录入协议改成挑战值、零 MAC 或自制令牌。
[`goodix_operation_start_enroll()`](../../../linux/userspace/fingerprint/piano-goodix-operations.c)
只作挑战、SID 和类型的结构检查，MAC 的真实性仍由原厂签名程序判定。

[`piano-goodix-rpmb.c`](../../../linux/userspace/fingerprint/piano-goodix-rpmb.c)
通过 UFS BSG 的 `SG_IO` 提供回放保护存储（RPMB）通道。原厂读请求使用协议版本
`0`，认证写请求使用 `0x100`；两者共用 24 字节请求头、512 字节帧和 20 字节响应头，
响应保留请求版本。独立 Linux 凭据已通过该通道创建并持久保存。
通道只转发计数器读取和认证读写，写后的结果查询由主机生成，拒绝密钥烧录、purge、配置写及
未知版本／帧。写入或响应传输失败后停止该会话，不自动重试。

录入期间，FS、GPFS 和 RPMB 三个回调由同一个对象管理。只有匹配所选高位 UID 的
`0x21001` 创建或 `0x21002` 校验同步调用期间开放 RPMB；传感器初始化、挑战获取、
采样、模板保存和关闭时均关闭。普通指纹验证、枚举和删除不打开 RPMB 后端。
共享请求先复制到主机私有缓冲区，再校验和提交，回复后清零；日志只记录命令与
帧结构状态，不输出数据、MAC、nonce、计数器、凭据或 HAT。

对同机签名 `keymaster64` 的静态分析已确认 Gatekeeper 的记录管理：现代
请求解析器拒绝超出 32 位的 UID，查找时比较完整 UID，不截断或取模。46 个记录槽
仅从空槽分配，满表拒绝新建，不淘汰旧用户。验证失败计数和时间戳更新选中的记录；
底层写回 512 字节扇区时会重算同扇区的 HMAC，不更改相邻记录的 UID、SID 或失败计数。
共用的启动初始化会更新非零时间戳，不会清除失败计数。

RPMB 与 Android 共用，包含全局写计数器、启动记录和安全存储目录元数据。
回调协议没有 TA 身份或 UID，主机不能按 Linux UID 过滤认证写，也不能从加密帧识别
安全程序内部的全局目录更新。UID 隔离依赖上述原厂签名 Keymaster 的实现；
命令白名单和短暂开放时段不等于独立的物理 RPMB。Linux 客户端不调用凭据删除或
全局重置命令，Goodix 模板的普通文件则保存在独立的 Linux `fpdata/`。

因此，禁止修改 RPMB 时不能测试新录入，也不能仅凭“已有 Linux 凭据”就执行
Gatekeeper 校验来取得新 HAT：`piano_goodix_enroll()` 经 `goodix_gatekeeper_create()`、
`goodix_gatekeeper_verify()` 调用 `gatekeeper_command()`，后者在同步调用期间允许
RPMB 认证读写。已有凭据可避免重复创建 SID，但校验和启动记录仍可能更新共享安全存储。
HAT 绑定当前录入挑战，不落盘供下一次录入复用。
[Android Gatekeeper 规范](https://source.android.com/docs/security/features/authentication/gatekeeper#request-throttling)
要求验证前写入失败计数、成功后清除，所以不能把正常校验改成只读来绕过防重试保护。
高位 UID 与独立模板目录可以隔离用户记录，但不改变共享存储这一事实。

现有 `piano_goodix_verify()`、`piano_goodix_list()`、`piano_goodix_delete()` 使用
`session_start(..., 0)`，只注册 FS／GPFS，不打开本项目的 RPMB 后端；其中认证复用
Linux profile 中的 SID 和模板，验证的是 Goodix 返回的指纹 HAT，不再获取 Gatekeeper HAT。
这是可保留的普通认证路径。`TEE_IOC_VERSION` 只确认内核接口能力，不验证指纹采集、
匹配或模板持久化；删除操作仍会更改 Linux 模板。

[标准 fprintd 录入接口](https://fprint.freedesktop.org/fprintd-dev/Device.html)
只接收手指名称，由 Polkit 授权，不提供 Gatekeeper 凭据或 HAT。
[`本地 libfprint 后端`](../../../patches/libfprint/1.94.100/series.json)
直接编译同一原生实现，提供录入、单手指验证、多指识别、枚举、按 UID 删除和取消；
后端的设备私有数据只保存 Linux UID 和手指名称。fprintd 管理标准用户记录，
密封模板仍由设备端存储管理。
GNOME 账户设置通过标准 fprintd 接口录入，锁屏认证使用标准 PAM 链路。

`fprintd` 的 `any` 请求在登记多根手指后使用标准 `IDENTIFY`，可识别该用户的任一
已登记手指。后端按图库中的 Linux UID 分组，每次只选择一个用户的 QTEE 分组，
调用原生 `verify <uid> any`，继续校验分组、模板 ID、挑战、SID 和认证类型，
再将结果对应回请求图库中的原始记录。指定手指的 `VERIFY` 仍只接受该手指。

fprintd 在新录入前通过同一接口检查重复指纹。跨用户图库按 UID 依次执行独立采样，
前一个用户完成不匹配后，才尝试下一个用户，因此可能需要再次轻触；认证数据错误、
存储错误、超时和取消不会被当成不匹配继续。图库为空时会先枚举 Linux 自有记录，
让 fprintd 识别并清理丢失主机登记信息的指纹，Android 的分组不参与识别。
