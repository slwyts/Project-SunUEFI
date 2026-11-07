# Piano 的 RGB 灯、红外与侧指纹

本页记录 RGB 通道、SPI 红外和 Goodix 侧指纹的原厂接口与 Linux 实现。指纹通过原生 QTEE 客户端和标准 libfprint／fprintd 接入 GNOME 与 PAM，支持 Linux 独立录入、持久保存和认证。

| 功能 | 原厂接口 | 当前 Linux 缺口 |
| --- | --- | --- |
| RGB 通道 | PM8550 SID1 上的 `qcom,pm8350c-pwm`，SPMI LPG，通道 1/2/3 为 red/green/blue | 标准驱动和产品DT已有；补齐SDAM/LPG模块加载后已出现三个标准LED接口。发光、可见位置和用途尚待确认。 |
| 红外发射 | SE15 / SPI `0x89c000` / CS0，`ir-spi`，Android 原始波形字符设备 | 已接入标准 SPI/LIRC、原厂波形量化与 MOSI 空闲处理；光学输出未验证。 |
| 侧指纹 | Goodix，GPIO 控制与高通 QTEE 安全服务 | Linux 独立录入、持久验证和错误手指拒绝已接入标准 fprintd、GNOME 锁屏与 PAM。 |

## RGB 通道

原厂 `/soc/qcom,spmi@c42d000/qcom,pm8550@1/led-controller` 绑定 `qcom-spmi-lpg` / `leds_qcom_lpg`。三个子节点原样指定 `reg=1/2/3`、红/绿/蓝颜色和相应 label。其两个 nvmem 依赖是 **PMK8550 SID0 的 `sdam@8400`、`sdam@8500`**，用于 LPG 通道状态和图案数据，不是相机闪光灯的 `0xee00` 外设。

当前产品 `board.dtb` 已保留上述节点和依赖，节点未写 status，按 DT 规则启用。当前配置已有 `LEDS_QCOM_LPG=m`、`LEDS_CLASS_MULTICOLOR=m`、`NVMEM_SPMI_SDAM=m`；[上游 LPG 驱动](https://github.com/torvalds/linux/blob/master/drivers/leds/rgb/leds-qcom-lpg.c)已经支持该 compatible、通道和标准 LED class，[绑定说明](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/leds-qcom-lpg.yaml)也接受这些字段。**无需再加一个重复 RGB overlay。**

Linux起初没有RGB，LPG的deferred-probe原因为未取得LPG chan SDAM。正确提供者模块名为 `nvmem_qcom_spmi_sdam`；加载它及 `leds_qcom_lpg` 后，platform device绑定 `qcom-spmi-lpg`，出现 `red`、`green`、`blue`，初始brightness为0、max_brightness为255。没有读取SDAM内容或写brightness，尚不证明可见灯的位置或用途。

LPG 和 SDAM 通过设备树的硬件别名自动加载，不需要模块列表或守护进程。`sdam@7000/sm1510_present@5d` cell 的无效位切片由打包器移除，保留原始字节，详见[启动设备树处理](../linux/kernel.md#piano-启动配置)。LPG 使用的是 8400/8500 两个 SDAM。

## 红外发射

Piano 的发送器通过标准 rc-core/LIRC 接口接收脉冲与间隔，设备为
`/dev/lircN`。无需 Android HAL 或常驻服务，`ir-ctl` 由 v4l-utils 提供。
发送时可用 `ir-ctl -d /dev/lirc0 --carrier=38000 --send=remote.txt`，文件使用
ir-ctl 的 `pulse` / `space` 文本格式。接口注册和编译不代表光学输出已经测量；
首次上机需要核对实际载波、包络以及发送后的熄灭状态。

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

Linux 原生客户端已完成独立凭据创建、指纹录入、模板保存和重新打开后的验证。
实机录入右食指后，关闭并重新打开客户端仍能匹配；未录入的左食指返回不匹配，
不是等待超时。Linux 使用自己的凭据和模板目录，不复制 Android 已录入的指纹。

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
原生客户端完成操作后关闭传感器及安全程序。桌面接口范围见下文的
[Gatekeeper 与桌面认证](#gatekeeper-与桌面认证)。

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
使用固定版本的 Qualcomm MinkIPC／quic-teec 与 QCBOR，由四个发行版
共用的运行时构建器静态编译；来源记录在
[`config/fingerprint.json`](../../../config/fingerprint.json)。
执行 `sudo piano-fingerprint-ctl info` 可查询设备信息，`--help` 不访问设备。
签名程序按固件清单随镜像提供，不包含用户模板或密码。

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
来源库 SHA256、符号、文件偏移、大小和输出校验值全部记录在
[`固件清单`](../../../vendor/firmware/hyperos4-fingerprint/manifest.json)，
不会安装原厂可执行库。

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

[`MinkIPC 补丁系列`](../../../patches/minkipc/0.1.0/series.json)修正回调输出缓冲区的
重复释放与异常回收，并避免工作线程释放最后一个 root object 时自等待。
构建器在固定上游提交的副本中应用补丁，记录补丁及存储源码的摘要；原始源码缓存保持不变。

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

原有[旧 GPFS 协议记录](../../../linux/bsp/patches/qtee/README.md)保留厂商接口说明；
持久存储和 RPMB 实现由原生客户端管理。

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

[标准 fprintd 录入接口](https://fprint.freedesktop.org/fprintd-dev/Device.html)
只接收手指名称，由 Polkit 授权，不提供 Gatekeeper 凭据或 HAT。
[`libfprint 后端`](../../../patches/libfprint/1.94.100/0001-piano-goodix-uio.patch)
直接链接同一原生 API，提供录入、单手指验证、枚举、按 UID 删除和取消；
后端的设备私有数据只保存 Linux UID 和手指名称。fprintd 管理标准用户记录，
密封模板仍由设备端存储管理。
GNOME 账户设置通过标准 fprintd 接口录入，锁屏认证使用标准 PAM 链路。

后端不提供 libfprint `IDENTIFY`。QTEE 每次只选择一个 Linux 用户的分组，而
libfprint 的识别图库可能跨用户，包括 fprintd 的录入查重，因此不能声称支持该能力。
`fprintd` 的 `any` 验证请求会由它选择并提示一个已登记手指，再执行单手指 `VERIFY`；
原生 `verify <uid> any` 则只在该 UID 已登记的手指范围内匹配，不提供跨用户识别。
