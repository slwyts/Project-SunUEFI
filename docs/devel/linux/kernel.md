# 内核来源与配置

Debian GNOME、Debian KDE、Nyarch GNOME 和 SteamOS 共用同一套设备内核。
默认构建使用正式版 Linux 7.3，版本名为 `7.3.0-piano-sunuefi-cachy`。
桌面名称属于 root 镜像，内核名称不带 `gnome` 或构建目录的 Git 后缀。

## 源码与硬件适配

Linux 基线为上游 `v7.3`，提交
`c03c1a14507dfaf8b856c2608f9ac216021fc222`。Piano 的公开硬件适配来自
`blu-sharky/linux-piano`，在新基线上继续保留本项目的显示、音频、输入、
充电、睡眠和编解码修正。

补丁按平台、显示、输入与无线、媒体、音频、Gunyah、BORE 分组，
后续接口修正单独追加。原始来源、补丁摘要、应用顺序与每一步的源码树
记录在 [`series.json`](../../../patches/linux/7.3/series.json)；
旧版本补丁保留作来源参照，不会再叠加到新版本上。

`./build.sh linux` 从固定上游提交准备源码，在 `build/kernel-worktrees/`
中应用补丁，使用独立编译副本生成 `artifacts/kernels/release/`。
`upstream/` 中的工作区不被改写。本地与 CI 使用同一入口和配置。

7.3 改动了 DRM 原子提交、显存分配、内存反向映射等内部接口，因此升级
包含的驱动移植。保留旧功能代码并不保证运行行为完全相同，升级后
仍需检查实机显示、输入、无线、音频、相机和电源。更新时必须同时安装
新内核的模块、`v4l2loopback` 相机模块和 initramfs，不能只替换 `Image`。

## CachyOS 与通用配置

* 引入 CachyOS 为 Linux 7.3 整理的 **BORE 6.8.0**，保留其默认参数。
  它在公平调度器基础上根据任务的运行特征调整调度，目标是改善负载下的
  交互响应；具体帧率、功耗和温度收益需要测量，不能仅凭启用选项判断。
* 启用 **MGLRU**，让内核按多代页面访问情况回收内存。既有 Zstd ZRAM
  继续保留，这两个功能属于 Linux 本身。
* 使用 **300 Hz** 定时配置、动态抢占并默认完整抢占，保留空闲时停用
  周期时钟的 `NO_HZ_IDLE` 和 `schedutil` 动态调频。
* 不另设 CPU 频率上限，不引入 x86 专用优化或已经不适用于当前 BORE 的
  sysctl 参数。300 Hz 也不等于屏幕刷新率或输入采样率。

具体配置位于 [`piano-platform.config`](../../../linux/configs/piano-platform.config)，
来源及许可见 [THIRD_PARTY.md](../../../THIRD_PARTY.md)。

## Gunyah

原厂启动链已由 Gunyah 占用 EL2。这里接入其资源管理器和虚拟机接口，
不会替换原厂 Hypervisor，也不等于直接提供 KVM。
驱动编进内核，以保留借出内存时取消内核直接映射的处理。

原厂设备树中的资源句柄只对测试启动有效。UEFI 在早期保留这些数据，
在 raw 和 EFI 两条 Linux 启动交接路径中更新目标设备树。没有取得有效数据
时只禁用 Gunyah 资源管理器，Linux 仍可正常启动。
源码位于 `uefi/handoff/early-memory/PianoColdBootObjects.c` 和
`uefi/core/PianoProductBootObjects.c`。

GNOME 的“关于”页面对高通主系统域省略通用的 `vm-other` 行，
底层 systemd 检测与 Gunyah 接口保留。桌面补丁与打包状态见
[系统信息适配](../../../linux/desktops/gnome/patches/control-center/README.md)。
Gunyah 虚拟机运行仍需实机验证，不能以设备节点出现代替。


## Piano 启动配置

打包时由 `tools/package_release.py` 将折叠后的原厂 DT 转成主线使用的最终设备树。
旧 SDE／CPAS／VIDC／SPF 节点由主线显示、CAMSS、Iris 和音频节点接管；旧 ICC
参数和被误识别为 GPIO 的厂商标志不再作为标准绑定传给内核。ADSP 的固件、
内存和电源资源保留，只删除主线 PAS 驱动不使用的旧 ICC 属性。

显示和相机的旧 GPIO 稳压器、CCI、调试 UART，以及没有消费者的旧触控供电
和 USB 空 PHY 节点也不参与探测。显示、相机和蓝牙使用各自的主线节点，
不再等待 Android 的 `pinctrl@f000000`。主线替代节点必须存在且启用，才停用
对应的旧显示／相机节点。Demura 的零地址、零长度占位项禁用，有效的内存
预留保持不变。

原厂 `sdam@7000/sm1510_present@5d` 在一字节内声明了偏移 5、长度 5 的
无效位范围。主线没有该厂商标志的消费者，因此保留完整原始字节，去掉无效的
位切片，不猜测其正确位宽。这样 SDAM 提供者及同区的 OCP 记录可以正常注册。

原厂在本板同样无法探测的四个可选 PMIC，以及不属于 AP 的两个硬件投票 RSC
节点，在最终树中禁用；AP 的 `rsc@16500000` 和外设供电保持启用。
`piano-platform.config` 排除 NXP 的 SCMI 扩展，保留 Qualcomm 使用的标准协议。
USB PHY 通过的 USB-C 连接器拓扑接入标准方向和模式切换接口。

这套配置已从正常启动的内核日志核对：上述引用、SPMI、SCMI、DRM 桥及 RSC
报错消失，ADSP、音频、键盘、蓝牙和 USB 调试连接保持工作。

磁盘启动统一使用 `linux/userspace/disk-bootstrap`，支持发布镜像的 LABEL 和
开发镜像的 PARTUUID。ext4 先正常回放日志，再转为读写；救援环境包含磁吸键盘
所需的 GENI I²C、GPI、Nanosic 驱动和 MCU RAM 固件。

相机和视频初始化脚本在内存映射已经为 `DMA` 或 `DMA-FQ` 时直接沿用，
可以正常重启服务。驱动绑定后重复写 IOMMU group 的类型会被内核拒绝，
不能把这类服务重启失败误判为相机硬件故障，更不能解绑正在使用的映射。

## USB Type-C

[`piano-usb-typec.dtso`](../../../linux/dts/piano-usb-typec.dtso) 连接 PMIC GLINK、
QMP PHY 和 DWC3 的标准端点，参照高通上游的
[`sm8750-qrd.dts`](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8750-qrd.dts)。
同机 HyperOS 4 的配置和引脚状态确认，GPIO61 是插头方向输入；
UCSI 读取该信号，通过 Type-C switch 控制 QMP，而不是在设备树中固定方向。
原厂 DWC3 的速率上限为 SuperSpeed，也就是 5 Gbit/s。

DWC3 使用双角色模式并注册 USB role switch，默认仍为设备模式，
供现有 USB 调试网络使用。固定为 `peripheral` 时，即使设备树声明了
`usb-role-switch`，驱动也不会创建角色切换提供者，UCSI 无法取得该接口。

PMIC GLINK 下同时存在 USB 连接器和手写笔稳压器。内核补丁 0019、0020
使 GLINK 的模式和 UCSI 驱动只枚举连接器；0021 使 UCSI 核心优先匹配明确的
`reg` 端口编号，随后才使用旧固件的顺序回退。否则排在前面的稳压器会被当成
端口，界面虽然能报告方向，却没有取得 PHY 的方向开关。
0022 补齐已在开机前连接的 USB 伙伴所需的初始 USB 模式设置，
使其与热插拔路径一致；模式设置先于方向和角色通知。

GLINK 在 ADSP 重启后会复用 UCSI 实例。0025 在注销端口并释放连接器数组后
清空指针；否则下一次初始化若提前失败，再次注销就会访问已经释放的内存，
使 USB-C 恢复流程触发内核错误。

充电固件在重新连接后可能仍把电源类型报告为 `Unknown`，导致 MCA 将输入
限制在 500 mA。0026 同时核对同一 PMIC GLINK 下 UCSI 电源报告的 PD 受电状态、
充电通路、原厂 `SNK_Ready` 状态和有效的固定 5 V PDO，再恢复普通 PD 分类。
电压、电流和温度限制继续由原有充电策略处理；读取失败跳过该轮策略。
类型恢复后的高压重试沿用单次连接的次数限制，UCSI 属性读取与电源注销通过
电源子系统现有信号量同步，避免 ADSP 重启时访问已释放的连接器。
普通 PD 沿用原厂 buck 策略的 `in_pd = 1600 mA`，并受电源 PDO 的电流上限约束。
这是普通 PD 通路的限制，不能按充电器的额定功率判断实际输入。
0027 在现有充电工作队列中完成小米 MiPPS 挑战应答，以标准电源属性
`authentic` 报告结果，无需认证服务。固件漏报或保留旧充电器 ID 时，依据
随机挑战的真实应答搜索并校验 ID；恢复搜索最多执行 65536 次，读到的有效
32 位 ID 直接校验。无法匹配就继续普通 PD/PPS，不根据铭牌或缓存身份放宽电流。

Piano 的原厂认证顺序是命令 7、命令 6，不发送会使固件后续 PD 请求失败的
反向认证命令 8。认证不切换 USB 数据角色；ADSP 重启后失效，每次连接只尝试
一次。UCSI 的连接状态用于区分认证后的短暂电压复位与拔线，快速拔插同样清除
旧认证，不沿用直充电流回退所用的 10 秒间隔。认证成功后重新读取开放的 PPS
档位，仍由既有充电策略选择降压或 SC8541 直充通路，保留电池电压、温度和
电流限制。

0028 对照原厂 `qcom_subpmic_sync_cfg_work()`，把已有设备树节点
`mca,qcom_subpmic` 的 OTG 配置通过 `0x201a` 同步给 ADSP。四个小端 32 位
字段依次为 `otg_boost_src`、`gpio_chip_type`、`vdd_boost_en`、`otg_ovp_en`；
Piano 的前两项为 0、1，后两项 GPIO 未配置，沿用原厂的 `-1`。
驱动只接受这组接线，每次 ADSP 启动后重新同步，失败时保留普通充电并重试。
漏掉初始化时，固件的 `mca_misc_enable_usb_otg()` 会报告 `gpio not init`，
重试超时返回 `FFFF`，在拔掉供电角色的拓展坞时导致 `TCPC_0` 崩溃。

ADSP 崩溃恢复还会同步注销 ASoC 声卡；音频进程保留已移除声卡的句柄时，
`snd_card_disconnect_sync()` 会等待句柄关闭，进而阻塞 PDR 与 ADSP 重启。
此时充电属性返回 `-ENODEV`，需要与正常的功耗和温度限流区分。0028 处理
上述 OTG 初始化触发源，没有改变声卡注销和 DMA 资源释放的等待条件。

Linux 的电池充电电流从 38°C 以上每升高 1°C 下调 300 mA，普通 PD 在
47°C 起降至 500 mA；降温后每 30 秒最多恢复一级，并留出 1°C 的回差。
这项调节不降低 USB 输入电流上限。判断断连需要结合输入电压、电流和
UCSI 连接状态，不能只看电池的 `Charging` / `Discharging`。使用中的现象和
处理办法见[电源和睡眠](../../user/known-issues.md#电源和睡眠)。

ADSP 初始化加载标准 `pmic_glink_altmode` 和 `ucsi_glink`，MCA 只处理
小米充电协议，其 `pan_ack` 关闭，避免两个驱动重复确认同一组通知。
现有 USB 网络服务由 Type-C 端口的标准 udev 就绪状态启动：方向有效且数据角色
为设备模式时启动，断开或切换为主机模式时停止。方向已经应用到 PHY 后才连接
调试设备，未插线开机也不等待 USB。服务名称保持 `piano-usb.service`，
没有新增常驻服务。

## UFS 多队列

[`piano-ufs-mcq.dtso`](../../../linux/dts/piano-ufs-mcq.dtso) 补齐同机原厂使用的
MCQ 寄存器窗口及 ITS 中断连接。普通控制器位于 `0x1d84000`，
MCQ 配置、厂商状态和队列门铃分别从 `0x1da0000`、`0x1da4000`、
`0x1da5000` 开始；主线驱动使用一个 28 KiB 的 `mcq` 窗口访问它们。
ITS 的 UFS 设备编号沿用原厂的 `0x60`，DMA 仍使用既有 SMMU 路由。

2026-10-22 正常重启后，驱动启用 8 个 I/O 队列、1 个轮询队列，
队列深度为 64；8 路 ESI 中断均有完成记录，SteamOS 从内部存储启动。
这解决了之前缺少 MCQ 资源而退回普通队列的问题。启动时写
`wExceptionEventControl` 仍被设备拒绝，属于另一项待处理的属性兼容问题。

## CDSP

[`piano-dsp-resources.dtso`](../../../linux/dts/piano-dsp-resources.dtso) 使用主线
NSP NoC 绑定、两单元 interconnect 参数和 AP 的 BCM voter，将 CDSP 到内存的
带宽路径交给 `qcom_q6v5`。驱动在启动签名固件前申请带宽，启动交接完成后释放；
不再依赖 Android 留下的总线状态。打包器保留这条已转换的路径。

[`piano-cdsp-fastrpc.dtso`](../../../linux/dts/piano-cdsp-fastrpc.dtso) 将原厂的
11 个非安全计算会话连接到 Linux 管理的 apps SMMU，保留每个会话的
stream ID 和 mask。原厂安全会话 9 不作为普通 Linux 计算会话使用。
正常 DMA 映射由内核完成，没有在用户空间写 SMMU 寄存器。

2026-10-22 正常重启后，CDSP 收到 ready／handover 中断并进入 running，
11 个计算会话均使用 DMA domain。`cdsprpcd` 已打开 DSP 端的
`adsp_default_listener`，无需重启服务，之前连续出现的能力查询错误消失。
这证明固件启动和 RPC 基础通信可用；具体神经网络、视觉及计算库仍需
对应用户空间和 DSP 程序支持，不能仅凭服务启动宣称这些应用全部完成。

随后一次 deep 挂起由 RTC 在约 22 秒后唤醒，ADSP、CDSP 继续 running，
`cdsprpcd` 的重启计数保持为零，Wi-Fi、蓝牙和 USB 恢复连接。
CPU 离线期间仍有 GPIO 中断亲和性告警，未把这项告警隐藏或标为已修复。
