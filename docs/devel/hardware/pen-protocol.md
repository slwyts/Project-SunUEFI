# Piano 触控笔协议与原厂算法

更新：2026-10-25。默认 120 Hz，定位、压感绘画、144 Hz 笔输入、轻捏、震动和磁吸连接恢复可用，首次连接延迟已解决。掌压按接触面积识别并与笔同组交给 libinput 仲裁，阈值仍需用真实手掌数据调整，见[笔目录说明](../../../linux/userspace/pen/README.md)；轻捏、双击和笔身滑动的格式与 Linux 输出见[笔身手势](#笔身手势)。

下文保留移植过程的协议记录。原厂依据来自同版本 `OS3.0.309.0.WPYCNXM` ROM 的触控 HAL、公开驱动和操作采集。

## 屏幕数据与 BLE 分开处理

现有 NT36532e 内核驱动将完整 THP 帧送到 `/proc/nvt_thp_stream`。当前 `piano-touch-view` 只生成手指 MT，选型时排除笔帧。原厂 [P81 parser](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.c#L2954) 明确区分类型6/7/9与17/29：payload `+64` 包含压力、按钮、悬浮状态和 Tip/Ring 原始阵列，阵列起点分别为 `+26` 与 `+36`。这里没有最终 X/Y、倾角。

原厂 HAL 读取 `/dev/xiaomi-touch` 的 frame/raw 共享区。最终点有两条上报路径：v1 写 Linux `input_event`；v2 写 point 共享区并以 `UPDATE_REPORT_POINT` 触发上报。[mmap/ioctl](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_operations.c#L169)、[内核 point receiver](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_device.c#L41)。两份 Piano ini 配置 v1，具体依据见下文。不能把 HAL 输出结构按偏移当作 SPI 帧来读。

已保存的 Focus Pen Pro BLE descriptor 只有 mouse/keyboard/sensor Input，没有 Digitizer、Output、Feature 或标准 FF。Android 已确认 Report5 携带笔尖压力，Report2 的 `02 6e`／`02 00` 对应轻捏 `KEY_F19` DOWN／UP；Report6 是独立姿态数据。Linux 在120Hz冷启动下已经收到 Report5 压力和 type29，并通过标准手写笔设备完成定位与压感绘画。悬停、倾角方向和完整掌压仍需进一步对照。轻捏、双击和笔身滑动同在 Report2，格式见[笔身手势](#笔身手势)。

## IC 扫描模式与原厂开关条件

2026-10-09补充：模式1和3表示不同笔型号，并非普通与高性能扫描档位。[原厂枚举](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.h#L245)定义 `SUPPORT_M80P=1`、`SUPPORT_N83P=2`、`SUPPORT_P81C=3`。同机已提取的 `nt36532_touch.ko`（SHA `d3ef0e85f98147d50784fc6b32d1d55e53834e8aa406e682e86c048ef663bac0`）也在 `nvt_set_cur_value` 的 `0xb038/0xb4a4..0xb4c8` 将笔ID8对应到模式3；同版HAL在 `0x29fc0..0x2a220` 选择内部profile3，型号日志为 `xiaomi_p81c`。

原厂的[连接处理](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.c#L3753)接收 `DATA_MODE_20`：高四位表示连接，低四位表示笔ID；P81C连接值为 `0x18`，断开值为 `0x08`。[扫描策略](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.c#L3571)要求屏幕醒着、笔已连接且没有磁吸充电；游戏模式会额外限制扫描，白名单可放行。连接或充电状态变化、固件恢复和屏幕恢复都会重新应用策略。连接条件本身不能证明压力来源；同机原生 Bluetooth 指令和 Report5 记录已经另外确认了压力传输路径。

开启分为两步：[扩展命令04](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx_ext_proc.c#L1356)发送 `50 BF 04 00 01 00`，使能笔扫描；随后[型号命令](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx_ext_proc.c#L663)发送 `50 7B 03` 选择P81C，并等待控制器ACK。关闭扫描将扩展命令04的值改为0。当前Linux的 `/proc/nvt_thp_stylus` 已包含这组命令、ACK和恢复路径，但默认启动没有请求开启扫描；读取该节点得到的是已成功请求的缓存值，不是控制器硬件状态查询。

后续应在现有触控进程内按连接、充电和屏幕状态管理P81C扫描，不新增服务或第二个FIFO读取者。开启扫描本身不是绘画支持的证明：随后120Hz冷启动已取得 Report5与type29，标准输入也已完成绘画。`0x440` 的发送者已定位为 `com.android.bluetooth` 的原生 HID 处理，开源离线核心已能读取校准和压力报告；Linux矩阵与压力现由同一进程处理，并接标准pen输入；未知值仍不补造坐标或压力。

## 提取的 HAL 链

从[官方同版本 ROM](https://bigota.d.miui.com/OS3.0.309.0.WPYCNXM/piano-ota_full-OS3.0.309.0.WPYCNXM-user-16.0-6672ba521d.zip)读取 ZIP 目录与 payload manifest，核验官方 metadata SHA，并逐操作核验数据 SHA。为避免下载完整分区，构造了拒绝读取未知区间的只读稀疏 EROFS 视图，按 inode/pcluster 定向提文件。原始 metadata 保持不变；**部分镜像没有通过最终分区 SHA，不可刷写**。没有运行 HAL 或访问设备。

路径为 `/odm/bin/hw/vendor.xiaomi.hw.touchfeature-service`，它加载 `/odm/lib64/libtouchreport.so`，后者再加载同目录的 `libtouchreport_alg.so`、`libtouchreport_hal.so`、`libtouchreport_sensor.so`。五个 ARM64 文件及两份 Piano ini 已完整读取；各文件 SHA、操作验证和工具固定提交记录在本地 `内部记录目录

## 已还原的算法边界

算法库 SHA 为 `26f86f74781e70d03958271b100b31865d3eb80b69f30774ce1b1e25cb24f1c9`。动态符号与 ARM64 指令、GOT relocation 已交叉核对：

| 函数/对象 | 可确认的行为 |
| --- | --- |
| `stylus_tip_coor_cal_barycenter` `0x82660` | 找峰值，在最多五点邻域内取权重；只纳入权重≥101的样本，随后有边缘修正 |
| `stylus_coor_cal` `0x82e18` | 求两路 tip 坐标；working-state为1时再求两路 ring 坐标 |
| `calculate_tilt` `0x83278` | 从运行参数读取查找表、分段阈值和范围，写入 working-state的 `0x60/0x64`；独立于坐标倾角校正步骤 |
| `calibrate_coordinate_tilt` `0x831e4` | 根据 enable、threshold、rate 参数校正 tip 坐标；Python解析工具不执行此步骤，独立C++核心另行实现 |
| `stylus_coordinate_flip` `0x834cc` | 按上下文中的翻转/交换标志改变坐标 |
| `stylus_report` `0x86ab0` | 将两路坐标交换写入最终对象 `0/4`；working-state为1时交换写入 tilt `8/12` |
| `get_stylus_data` `0x859e0` | 返回 `stylus_total_data`（60096字节），不是最终 `stylus_point`（36字节） |

公开模块 `tools/piano_pen_protocol.py` 解析源码已定义的 raw metadata/矩阵、核验外层 checksum，并读取外部 ini；不内嵌原厂文件，也不生成坐标或 input 设备。`factory_tilt_component` 只还原接收已算 Tip/Ring 差值的 `calculate_tilt` 分段计算，要求调用者显式提供 runtime resolution，拒绝超出原函数 int32 中间值范围的输入。它没有应用配置里的 `tilt_calibration_enabled/threshold/rate`，没有实现 `calibrate_coordinate_tilt`、坐标解算或最终 report，因此不能当作最终笔 tilt；尚未与原厂执行结果对照。反汇编与字段记录保存在同一 private 目录。

## 标准输入接入前的缺项

本机 Android 的 `getevent -lp` 已确认 `NVTCapacitivePenP81c` 注册范围：X 0..213599、Y 0..319999、pressure 0..16383、distance 0..1、tilt X/Y ±60，另有 ABS_BRAKE 0..360。两份 ini 已提取，BOE/CSOT 的 hw/stylus 内容相同，project vendor ID 分别26/4，但 mapping 位置表不同。配置是 portrait `2136×3200`、三个轴翻转标志均0、project super-resolution100，stylus normal report factor10；Tip/Ring几何为 `12×40` 与 `60×8`。角度表为 `{0,1500,3000,4500,6000,7000}`。P81c id8/vendor3 选择默认 `stylus` 槽，差值表 `{0,28,51,67,81,87}`、校正阈值30/rate6；`stylus_2`／`stylus_3` 分别对应备选 vendor1/2，差值表 `{0,37,81,103,121,132}`、阈值37/rate8。controller mode3 不等于配置槽 `stylus_3`。

中间 hardware-info 构造已追明：`alg_read_config_param_core` 在 `0x1f88c..0x1f8ec` 读取完整键 `project_infor.super_resolution`，与内核 hardware 参数 `+8` 交叉核对，最终在 `0x1fbb0` 写入扩展 hwinfo `+0x24` 的 u16。`+0x14/+0x18` 是显示尺寸乘该因子，`+0x26/+0x27/+0x28` 是 x/y/xy flip。`alg_pass_hwinfo_core` 将此配置复制后，`alg_init_param` 在 `0x4385c/0x43868` 再将 resolution 写到 context `+0x2c`。这里的100与 stylus report factor10 是不同参数。

2026-10-08在 Android 触控服务仍运行时，核验本机五个 ELF SHA 和 PID/starttime，再按 maps 仅两轮读取各155字节配置。硬件头为 `2136×3200`、rx40/tx60、factor100；两个41字节 hwinfo 配置副本与48字节 context 配置均稳定，三个阶段的resolution都是100，scaled尺寸 `213600×320000`、三个 flip 均0； report 方法指针为v1。context还按columns60/rows40选择了column/row extent `320000/213600`，不是固定min/max。没有停服务、操作笔、读取触点/FIFO/矩阵或运行额外厂商程序。原始记录在本地 `内部记录目录

独立开源核心已经读取 mapping、选择 vendor3 默认参数，并独立处理 Report5 压力，主路线不再依赖补齐闭源 `stylus_total_data`／SC context。下面的原厂对象、回调和 BTF 记录保留用于对照；Linux 仍需取得 type29 后才能验证坐标、倾角和最终桌面事件。

## 最终36字节点与64字节 HAL 内部点

`stylus_point` 的动态对象大小确为36字节，地址 `0x15ca14`，GOT `0x913d0`。按写入和消费者核对：

| 偏移 | 已确认含义与依据 |
| --- | --- |
| `0x10` | 当前状态；`stylus_hover_pressure_judge` 在 `0x866e0/0x86740/0x867c4/0x867d0` 写入0..3 |
| `0x14` | 上一状态；`stylus_sts_bak` 在 `0x86b04` 复制当前状态 |
| `0x18` | 最终 callback gate；`0x88914..0x88918` 为0则不调用上报回调，不是直接 proximity |
| `0x1c` | 处理后压力；`0x866e4..0x866f4` 从 `stylus_total_data+4` 读取u16，经条件最小值/有符号扩展写入；其他路径明确清零。其输入还可能来自 `0x88448` 环形压力缓冲，不能把它称作原始压力直接透传 |
| `0x20` | distance布尔值；`0x88218..0x88228` 将内部输入 `+0x11d` 的非零判断写到total_data+7，再由 `0x882a4/0x882b8` 写入最终对象 |

`0x888e0` 的最终回调构造器读取 rodata `0x11240` 的四项表 **`{2,0,1,3}`**，把状态0/1/2/3分别映射为 action UP/DOWN/MOVE/HOVER。这与[公开 action 枚举](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_device.c#L12)数值一致；不是依据函数名推断。hover样本仍须对照evdev，不能仅以gate或压力生成BTN_TOUCH。

同一构造器产生的是**64字节** HAL 点：input style在0，坐标在8/12，tilt在24/28，distance在32，pressure在36，action在60。它将最终对象的压力/distance两项交换后写入。其余未明确初始化或解释的字保留未知，不补零生成事件。

共同 callback/GOT 链为：算法 `0x889fc` → libtouchreport 的 alg callback[0] `0x83ec` → 活动HAL子模块 `+0x80`（`register_hal_module` 在 `0x11c90..0x11c98` 安装）→ report interface `+0x48=0x21ae4` → `0x21c70` 按64字节点遍历 → 所选 report 方法 `+0x30`。

两份 ini 的 `input_device.report_version` 都为1，测试进程配置读取也确认选用v1。HAL `0x219bc..0x219f0` 读取这个完整键名，仅值2选v2，其余选v1；v2初始化报错也可在 `0x21304..0x21350` 回退v1。v1方法 `+0x30=0x25f9c` 在 `0x260a0..0x26118` 将上述内部点构造为24字节 `input_event`：BTN_TOUCH由pressure非零决定，BTN_TOOL_PEN由 `(distance | pressure)` 非零决定；X/Y、pressure、distance、tilt分别直接取已列字段。action UP分支清零这些键和轴。`0x2613c` 调用 `write`，按配置写所选输入fd。这条路径没有把64字节点交给mmap receiver。

v2方法 `+0x30=0x26dec` 是另一条路径：HAL `0x26804..0x26834` 以cmd2/arg4选择point共享区并mmap4096字节；`0x26eb4..0x26ec8` **直接复制64字节** 到映射；`0x27018..0x27024` 再调用cmd6 `UPDATE_REPORT_POINT`。这里仍未找到64→56重排，不能启用这条路径后假定布局相同。

2026-10-08从本机只读导出的 `/sys/kernel/btf/xiaomi_touch`，配合base `/sys/kernel/btf/vmlinux`，确认 `hal_report_piont_t` 指向**56字节**结构：input_style0，x4/y8，prop[10]12..51，action52。匿名union的finger/stylus布局相同；ACTION枚举为DOWN0/MOVE1/UP2/HOVER3，与公开固定MiCode一致。BTF还确认 `report_touch_event(s8 touch_id, u8 event_count)`；`point_data` 并非这份BTF中的独立类型。BTF只能证明结构和函数类型，不能代替正在运行的选路记录。56字节receiver与HAL64字节内部点的差异因此已经确认；两份v1配置解释了原厂不必使用该mmap接口。泛用receiver pressure上限8191也不能代替本机P81c已注册的16383。

结构化事实、原厂库/BTF SHA、GOT、地址和剩余项保存在 `内部记录目录

完成这些字段与计算链的还原后，用一次短的 raw17/29与Android evdev同步记录检查比例、proximity和tip边界，再接入现有触控进程的独立 pen uinput，保留手指 MT与 FIFO。使用 libinput/Wayland tablet-tool接口；悬浮时工具在范围内而笔尖未触地，不能照抄泛用原厂 reporter 将所有非UP动作都设为 `BTN_TOUCH=1`。未知倾角、按钮语义或姿态不填零宣称支持。

## 笔的磁吸检测、供电与连接恢复

默认内核的 [`0041-power-sc96231-pen-charger.patch`](../../../patches/linux/7.2.9/0041-power-sc96231-pen-charger.patch)驱动 SC96231 发射器；[设备树覆盖](../../../linux/dts/piano-pen-charger.dtso)将 QUP2 SE5 转换为主线 I²C，总线地址为 `0x38`。不再保留两个同时占用 Hall GPIO 的驱动：充电驱动接管同一个 `Xiaomi Piano Pen Dock`，上报标准 `SW_PEN_INSERTED`。

实现依据是同机 HyperOS 4 的最终 DTB、`sc96231_tx.ko`／`mca_wireless_revchg.ko` 的反汇编，以及一次原厂启停的函数参数采集。原厂的独立笔供电通过 ADSP HBOOST 完成：属性 `0x2010` 选择源 1，按 5.0、5.5、6.0、6.5 V 升压，间隔 20 ms；`0x200f` 的客户端 2 控制启停。该电源以标准 regulator 提供给 SC96231，不改 USB VBUS。 Piano 路径使 GPIO14 高电平启用，不能单凭旧节点的 `reverse-txon-low` 属性推断极性。

| 信号 | TLMM GPIO | Linux 用途 |
| --- | --- | --- |
| I²C SDA／SCL | 20／21 | 主线 `qcom,geni-i2c` 与 GPI DMA |
| TX enable | 14 | 升压完成后拉高，停止发射后拉低 |
| LP enable | 99 | 保持原厂运行电平 |
| IRQ | 78 | 下降沿，读取并清除中断 |
| Hall3／Hall4 | 200／196 | 低有效 OR，双沿中断与唤醒 |

芯片命令 `0x0100` 的 `0x80000`／`0x40000` 控制发射启停；中断来自 `0x0108`，写入 `0x010c` 清除。电量来自 `0x01d0`，低七位为百分比、高位为充满；蓝牙地址来自 `0x01de..0x01e3`，字节序与原厂上报一致。出现 FOD、EPT、通信超时或移除时停止发射。按照原厂行为，明确充满或连续 180 秒报告 100% 后关闭升压。驱动不下载或更新芯片 MTP 固件。

设备自动绑定驱动，标准节点为 `/sys/class/power_supply/sc96231-pen`，作用域为外设；没有有效报告时返回未知或 `ENODATA`。跨系统切换会自动恢复配对、GATT 服务和笔初始化。首次点击延迟与睡眠期间取放已解决；低电量充电速度不在本文提供量化数据。

原厂 `MiuiBleOobHelperService` 在磁吸链路给出笔地址后主动连接，未配对时调用 `createBond`。Linux 伴随程序使用相同的物理识别结果，通过 BlueZ 接口处理配对与控制服务。加密失败由 `org.bluez.Device1.Disconnected` 的 `org.bluez.Reason.Authentication` 判断，只移除该笔失效的配对并重新配对；不会清空其他设备。配对代理只服务测试磁吸识别的笔，不注册为系统默认代理。

一次故障期间不会反复删除同一配对；成功建立新配对后会清除测试重试状态。之后若再次发生密钥失效，仍可自动恢复，无需重启伴随程序。

## 刚连接后点击延迟

悬停位置来自屏幕矩阵，笔尖压力来自 BLE HID Report5；因此悬停可用不代表点击通道已经准备好。压力设备现在通过 inotify 创建事件立即接入，不再等待一秒扫描周期；BlueZ 控制服务就绪后也会立即唤起初始化，不再固定等待两秒重试。记录第一份压力报告和无线参数设置的单调时钟，用于区分设备连接、初始化及点按的延迟，不伪造压力或提前发送点击。屏幕状态从内部 DRM 连接器读取，并同步原厂 `0x61` 命令；不再固定向笔报告屏幕常亮。若在同一 BLE 连接中重启助手、笔返回未分配的 `0xff` ID，则重新建立该笔连接再初始化，不使用该 ID 配置无线通道。

`0042-input-nvt-defer-pen-mode-while-asleep.patch` 在面板休眠时保存请求的笔扫描模式，随固件恢复应用。触控服务启动不再强制解绑／重绑内核设备，面板复位和固件恢复统一由内核 panel follower 处理。

## 公开解析模块

使用调用者提供的文件，不随源码分发原厂 HAL 或 ini：

```sh
python3 tools/piano_pen_protocol.py metadata /path/to/original-spi-payload.bin
python3 tools/piano_pen_protocol.py config /path/to/piano_nova_csot_thp_config.ini
python3 tools/piano_pen_protocol.py stylus-point /path/to/36-byte-solved-object.bin
python3 tools/piano_pen_protocol.py hal-point /path/to/64-byte-factory-point.bin
python3 tools/piano_pen_protocol.py kernel-point /path/to/56-byte-kernel-point.bin
python3 tools/piano_pen_protocol.py runtime-hwinfo /path/to/41-byte-hwinfo-prefix.bin
python3 tools/piano_pen_protocol.py runtime-context /path/to/48-byte-context-config.bin
```

`metadata` 的输入从 `frame_data_packet` 开始。Linux `/proc/nvt_thp_stream` 每条记录的32字节 record header 和257字节 SPI/event前缀须先去掉，不能直接传整个流；当前捕获路径的最大 payload 是7934字节。解析器核验外层 additive checksum 与长度补码，且要求 metadata 和全部 Tip/Ring 矩阵位于该校验范围内。另一个尾部 pen checksum、hand packet 暂不解析。Android 内核可能已将 additive checksum 字段改写为 CRC32，不能把这样的 HAL mmap 帧混入这个输入格式。

Linux流读取者 `piano-touch-view` 现有可选 [`--capture FILE --capture-seconds N`](../../../tools/patches/piano-touch-view-raw-capture.patch)：默认关闭，N为1..10秒、默认5秒，内存上限16MiB。它保存自己已读到的完整NTP1记录，包含原内核头、序列、时间戳、epoch与所有普通/SC/17/29帧；不只保留笔帧，不另开FIFO。窗口结束或达到字节上限后一次写出新文件并继续原有输入，stderr给出字节数、错误和帧类型统计；应选择本地tmpfs新路径，不能覆盖旧捕获。后续调试应让现有owner带此选项启动，不能在服务旁再运行第二个reader。两个runtime builder通过同一 `derive_touch_source` 应用这个默认关闭补丁。此捕获入口已在现有触控进程中实机使用，最新结果见下文。

当前 `nt36532e` 的 `nvt_thp_read_frame` 原样读SPI，`nvt_thp_publish_frame` 检查补码后把原buffer直接送同一FIFO；没有原厂Android的checksum→CRC32重写。因此上述Linux捕获仍保留原始双校验字段，但内核valid flag只检查头补码，不能代替CPU适配器的两层校验。当前驱动没有1032B common7/0x440接口，但下述writer已经证明这条压力路径绕过kernel queue；不再把新增kernel读取API作为前提。不能从raw29 pressure拼接一个common记录，也不能把HAL发送sysfs `touch_thp_ic_cmd_data` 当作压力读取来源。

Python 调用可使用 `parse_metadata(data)`、`read_config(path)`、`profile(config, vendor_id)` 和 `factory_tilt_component(dx, dy, resolution, calibration)`。最后一个函数没有 CLI 事件输出，要求的已算 Tip/Ring 差值和已确认的 runtime resolution；配置 reader 返回的 vendor profile 只是配置映射，不证明当前连接的笔采用哪一个 profile。参数单位与最终 input 上报仍按上面的缺项处理。

`parse_stylus_point(data)`、`parse_factory_hal_point(data)`、`parse_kernel_report_point(data)` 分别读取调用者提供的36/64/56字节dump，返回已确认布局；64字节解析额外列出v1的静态键值判断，56字节解析保留prop[]原字。它们不接受SPI帧、不转换坐标单位、不归一化压力、不产生uinput/libinput事件。`config` 现在也输出 `input_device` 中的report版本。没有实测dump时，静态布局不能当作笔输入已可用。

`parse_runtime_hw_header(data)`、`parse_runtime_hwinfo_prefix(data)`、`parse_runtime_context_config(data)` 分别解析9/41/48字节的配置片段。context片段从已核对指针的 `+0x10` 开始；解析器只读调用者提供的文件，不跟随指针、不读进程。byte8保留原字，不作为ready标志。配置解析不能替代最终坐标、压力、hover与按钮事件的实测。

## 原厂算法 worker

固定 ALG 只有 `libdl/liblog/libc/libm` 四项依赖、32个 Bionic 导入，没有 libc++、Binder、属性或设备 ioctl 导入；普通AArch64 RELA也无需 Android packed-reloc loader。[GNU worker 源码](../../../linux/userspace/pen/)已完成隔离的配置/init：派生库只修改链接元数据，原库与指令区不变； GNU mutex/标准流适配处理 Bionic mutex 存储与 `__sF` 差异。2026-10-08在无网络、只读文件系统、空 `/dev` 的 Bubblewrap/QEMU 中，用原厂ini和9字节硬件头执行normal/stylus配置reader、构造context及stylus init/exit，exit0；resolution100、scaled尺寸213600×320000、columns60/rows40和stylus enabled1与已有配置一致。未加载HAL startup、执行帧处理或生成uinput。运行暴露并修复了GNU版本表标签残留和ALG level0空日志回调两处故障，没有填充假callback。

 stylus 接口的slot0/1是init/exit，slot8 `0x881e0` 将内部frame的四组signed16矩阵扩展到算法缓冲，slot7 `0x87fcc` 执行坐标/压力/倾角处理并在尾部调用 `0x888e0` 报告构造器；后者明确检查callback是否存在。输入需要HAL内部frame及非对齐指针，并非SPI payload。原厂v2 decoder只在类型29时复制笔metadata和四矩阵；类型17保持17，ALG再发外部command23/value2，不能强制改成29。

[CPU适配器](../../../linux/userspace/pen/piano-pen-frame.c)已按同版HAL的 `0x2fe44/0x30718` 实现类型29的笔输入前缀：内部跨度1297B，header在0x3c、36B metadata在0x10d、四个pointer在0x131/139/141/149，20B trailer在0x151。矩阵长度来自ini的12×40、60×8，每组480个int16，四组共3840B。它核验原始包的外层与笔尾层additive checksum/补码/边界；手指四分包位图齐全且累计和为10时才发布完整矩阵，重复分包不能替代缺失分包。频率跳转请求保留原字，不在CPU worker发硬件指令或返回假ACK。Android mmap中被内核改写成CRC32的包不属于此输入格式。

worker的 `--decode29` 接受已捕获的原始包；`--prepare29` 还需要外部1032B common记录，byte1=7、u16+2=0x440，原厂slot2读取body中的两个u32 `+0xc/+0x10` 来更新压力ring。本机BTF确认common_data是8B头加256个s32 body，`+8` 是data_buf[0]，不是长度；u16+4的data_len是s32数量，此入口要求3..256个body字。HAL的另一条common读取路径已追明为 `0x11dc8` 打开 `/dev/xiaomi-touch`，touchid/hardware ioctl后在 `0x11fdc..0x12000` 把同一FD交给getcmd等子模块，再由 `0x160f8/0x16390` 原样读取/转发；这不证明压力记录由kernel生产。

同版ROM service SHA `3e4549b2…271f42` 已确认真正的0x440 writer：外部长vector入口 `0x16b88` 在 `0x16c70/0x16cac` 特判mode0x440，调用 `0x19df8`；后者在 `0x19e98..0x19eac` 写入touchid/cmd7/mode0x440/data_len，把外部s32数组原样复制到body。`0x19eec→0x1df40` 通过dlsym安装的 `thp_daemon_cmd_process` 直接进入libtouchreport与ALG压力ring，绕过kernel common queue。同版 `/vendor/lib64/vendor.xiaomi.hw.touchfeature-V1-ndk.so` SHA `42aac267…c5e00d` 已明确映射该方法为 **`ITouchFeature::setModeLongValue(touch_id, mode, length, values)`，Binder transaction8**：proxy `0x9e48` 依次写三个int32与int32数组，vtable `+0x68` 对应service `0x400e0→0x16b88`。同机动态记录和原生客户端指令已确认外部发送者为 `com.android.bluetooth` 的 `bt_main_thread`：`libbluetooth_jni.so` 接收15字节 BLE Report5，把每个字节零扩展为 int32，再调用 `setModeLongValue(0,1088,15,values)`。压力取报告ID之后的两个小端字节，不来自 raw29 的 metadata。固定MiCode中cmd7是SET_LONG_VALUE、0x440是DATA_MODE_141；普通ioctl的long-value只处理mode15，不能用这个泛用入口解释当前pressure writer。Android 的专用 HID 消费分支成功后跳过普通 UHID 转发，解释了原厂采集时 hidraw 报告数为0；Linux BlueZ 的路径仍按 Linux 报告判断。worker继续接收外部1032B记录，由现有owner转交；不增加假kernel API或第二FIFO。

2026-10-09已完成原厂 Binder／uprobe 定向记录：先捕获175次 mode1088、length15的调用，再采集651个完整 Report5 压力报告。离线核心逐条读取15字节数据，记录中的压力范围为0..12288，包括32次零值；没有构造替代报告。原始记录在本地 `内部记录目录 `内部记录目录

CPU前缀仍不能交给完整 `parse_data_package`：该函数还读取主触摸/SC缓冲与噪声状态。独立开源核心不依赖这一闭源入口。早期144Hz Linux 操作窗口内收到496条 HID 报告：488条长度21的 Report6 姿态，以及8条长度2的 Report2（`02 6e`／`02 00`，四对轻捏按下／松开）；Report5 为0，同步捕获的1728条 NVT记录全部为 type3。具体结果保存在本地 `内部记录目录 HID 并非整条不通，当时仍缺压力报告与屏幕笔矩阵；后续120Hz结果见文末。轻捏报告已收到，但还没有映射桌面操作或笔端振动。

## 独立开源核心与打包入口

[`linux/userspace/pen/`](../../../linux/userspace/pen/README.md) 的 C ABI 核心基于 Apache-2.0 开源项目 [xiaomi-sheng-thp 的固定提交](https://github.com/ianchb/xiaomi-sheng-thp/tree/34046210932d654a4c0df0121ecc31c008f8148c)，使用 Piano 的尺寸、原厂 mapping 和 P81c 默认参数，不沿用 Sheng 的位置表。核心校验两层原始 checksum与四分包位图，再处理 Tip/Ring 矩阵、倾角和 Report5 压力；当前已编译并读取 BOE／CSOT 配置，但尚无 type29 用来验证最终坐标。

在仓库根目录运行 `make -C linux/userspace/pen`，生成 `build/pen-core/libpiano-pen-core.a` 与 `build/pen-core/piano-pen-offline`。C ABI 用于随后接入已有触控进程，离线工具不创建输入设备。BLE 时间戳必须采用 `CLOCK_BOOTTIME`，与 NTP记录的 `ktime_get_boottime()` 同源；不能在睡眠后混用 MONOTONIC 时间。选择原厂校准应以触控控制器的 LCD ID／工厂配置为依据，不能根据显示面板标签代选。当前触控 LCD ID为1，加载 BOE触控固件，即便显示链路采用 CSOT配置。

两个 runtime builder 已通过 `tools/build_piano_runtime_helpers.py` 中的 `build_pen_core()` 共用上述 Makefile。默认 native runtime bundle安装 `/usr/bin/piano-pen-offline`、Apache-2.0 许可与来源说明；manifest记录多文件源码、头文件、编译器和目标静态 C++ 库，`runtime.stage()`把来源记录一并放入 rootfs。构建不读取 ini或设备。Clang使用目标 GNU sysroot 的 C++ 开发文件；GCC优先使用配套 G++，缺失时使用已有 Clang与同一目标 sysroot。

静态库作为构建输出保留，后续才能链接到原有触控进程；当前没有改变该进程的默认行为或启用绘画输入，也没有新增后台服务或第二个 THP reader。`tools/stage_piano_ram_hardware.py` 继续只处理已有脚本与配置，不承担 C++编译。

`tools/package_bsp.py` 当前是架构无关配置包，拒绝 ELF程序，原生程序应留在 runtime层。原厂 ini是架构无关数据，但当前公开 `piano-firmware` 只有触控固件 bin，没有这两份校准文件。构建应显式接收 BOE／CSOT校准输入及 ROM来源、文件摘要，安装到 `/usr/share/piano/pen/calibration/`，本地和 CI使用同样输入。不能默认取某台开发电脑的 `内部记录目录 文件，或在缺失时静默套用另一面板的位置表。校准数据的自动提取与打包仍待接入，离线工具可以先由调用者提供 ini使用。


## 本机原厂无线初始化对照

2026-10-09通过普通启动请求切回 Android，原厂服务自己的 `E4()` 日志已经记录每条 FE11控制字节和初始化确认结果，无需额外探针、全量HCI日志或重新配对。运行时 `TIME_STAMP=[1745566508]`，MTU512，两个分配ID为0和1。过滤后的请求序列保存在本地 `内部记录目录

流程是 `52 → 00/01/02/03 → 04/30/31/32 → 04/30/31/32 → 33 → 51 → 5F/5A/5C/61`。参数30含本机适配器地址和当次笔ID，第二轮的时序槽值由3变1；频率31使用原厂默认表，电压32为`2800`；时间戳33是Unix秒数，与输入采集的BOOTTIME时基不同。不能静态重放另一台机器的适配器地址或旧的笔ID。

[`piano-pen-bluetooth.py`](../../../linux/userspace/pen/piano-pen-bluetooth.py) 已实现该序列，每步等待回应，并按真正的 `SW_PEN_INSERTED` 发送Hall状态。无线参数和解算校准是两类数据；本机无线参数放在BSP的 `p81c-radio.json`，BOE／CSOT位置校准仍来自原厂ini。程序通过统一runtime安装，参数通过BSP和硬件配置组装安装，不新增服务或第二个THP流读取者。

原厂还有一条独立的地址获取通路。固定设备模块 `sc96231_tx.ko` 的 `sc96231_get_pen_mac` 从 `0x200001de` 读取两字节、从 `0x200001e0` 读取四字节；MCA 的 `mca_wireless_rev_pen_data_handle_work` 调用该函数后发送 uevent。原厂蓝牙扩展的 `oobhelper/f.java::onUEvent` 处理 `POWER_SUPPLY_PEN_MAC`，获取指定地址的 `BluetoothDevice`，按绑定状态进入连接或配对流程。这解释了原厂并非仅依赖用户打开扫描列表。该静态调用链不证明充电芯片主动唤醒 BLE 的电气时序，不能把上述地址读取直接当作唤醒命令。

Linux 的伴随进程现在只在断连时进行有间隔的扫描，并直接重连 BlueZ 中已经配对且受信任的 P81C；已连接后停止自己的扫描。适配器由 BlueZ 枚举，不固定使用 `hci0`。充电芯片的供电、地址获取及可能的无线唤醒仍未接入，跨系统切换后笔不广播的问题尚未解决。

普通重启返回Linux后，完整序列收到所有查询和设置回应。随后按平板按钮倒计时做悬停和划线，得到4条Report2、Report5为0，1728条THP记录全部为type3。结果保存在同目录 `result.json`。完整无线初始化已经完成，绘画数据仍未产生；这条差异不能继续当作已证明的根因。

下一步核对原厂笔扫描与显示时序。Linux控制器读取为game_mode0、hand_scan_rate180、pen_scan_rate240；不能归因为游戏模式1。原厂扩展命令0x0f是游戏模式布尔量，不能猜成LCD刷新率枚举。同机原厂笔服务接收PEN_FPS事件并映射到60／90桶，用于蓝牙时序；该处理本身未调用显示服务切换到60／120Hz。是否由其他原厂显示／触控模块协调，以及IC通知命令，仍需进一步核对。


## 原厂用笔时的扫描状态

2026-10-09在原厂Android保持当前显示设置时，用 `NVTCapacitivePenP81c` 的 `BTN_TOOL_PEN` 事件触发一次只读快照。显示activeMode1为120Hz，控制器hand_scan_rate60、pen_scan_rate240、op_mode1；之前空闲状态读过hand120／op_mode2，唤醒后的另一份读数为hand144／op_mode2。扫描数值会随状态变化，不能只根据一个手指扫描值判断LCD模式。记录保存在本地 `内部记录目录

Linux之前显示144Hz、hand180、pen240。两边笔扫描率相同；这一次原厂在120Hz正常用笔的记录不能证明Piano在144Hz禁止笔输入。为进一步比较，需要让Linux原生KMS第一次启用就选择120，而不能在当前有黑屏问题的运行状态中热切。

当前590内核配置使用 `CONFIG_CMDLINE_FORCE=y`，因此只给ESP的boot header追加video参数不会生效；Mutter默认又选择DRM preferred模式。冷启动草案只交换Piano面板模式表的144／120顺序，使120成为preferred，并保留全部模式。当时的草案用于此项对照；2026-10-10 已将 120 Hz 纳入默认内核，保留其他模式；构建来源在 `build/panel-cold120-candidate/`。原厂触控portrait尺寸2136×3200与Linux原生显示模式3200×2136不同，不能用错video参数。

触控面板监听生命周期修复已经从draft转为正式 [`0020-nvt-panel-follower-lifetime.patch`](../../../patches/linux/7.2.9/0020-nvt-panel-follower-lifetime.patch)，纳入默认内核源码准备流程。它只修注销与释放顺序，不启用笔接口或改变刷新率；新源码树已生成，完整新内核部署仍需后续构建。


## 标准桌面输入已接通

120Hz冷启动后，cap7的10秒持久采集包含1423条type29及421条15字节Report5。剔除10条校验损坏包，并单列2条尚不支持的quarter5，开源核心处理1411条支持包，1177条有坐标/倾角，939条压力有效；原始时间间隔中位4.175ms。100ms压力有效期是当前调用者策略，不是原厂参数。

`piano-touch-view` 的同一poll线程接入只读HID压力与THP矩阵，创建标准独立tablet-tool输入节点。系统识别 `ID_INPUT_TABLET=1`；Gtk GestureStylus画板收到落笔/抬笔与压力，画出了粗细变化的21笔线条，操作者确认位置与笔尖一致。原始截图和输出保存在本地 `内部记录目录

当前仍不把type29的一份手指quarter伪装成完整type3矩阵；同时手指/笔与完整掌压需要继续接线。两份2KiB数字校准已进入BSP，按Touch LCDid选择，完整原厂ini与紧凑配置在同一序列上的输出逐字段相同。新runtime builder默认链接同一笔核心，源程序和桌面偏好不另分测试镜像。2026-10-10 已将 120 Hz 默认显示策略和笔输入统一到默认内核及运行时包，144 Hz 笔输入仍未修复。


## 加号与轻捏反馈修正

2026-10-10 的默认内核补丁 `0021-hid-xiaomi-p81c-pressure-not-keyboard.patch`
针对 P81C 的私有压力报告，阻止它被当作键盘加号输入；报告仍交给现有笔输入程序
读取。短按轻捏上报为笔尖位置的一次点击，按住 600 ms 以上上报为标准 `BTN_STYLUS`，不向普通键盘输入快捷字符。

现有触控服务同时跟随 BlueZ 连接，使用 BSP 中的 `p81c-radio.json` 完成
无线参数、轻捏阈值和振动等级初始化；断线重连后重新应用。它没有新增独立地址
服务或第二个 THP 读取者。增量运行时包也包含这些参数文件，避免只更新程序而遗漏配置。

当前内核 `7.2.9-piano-gnome-gd33a42990baf` 已能正常画线并显示压感变化，
碰屏不再输入加号，轻捏恢复笔端震动。自带 GTK 画板只在笔输入时隐藏指针，
鼠标和触控板的指针保持正常；其他应用的笔指针由各自应用与合成器决定。

2026-10-20 的连接抓包发现，笔会从 750 ms 省电间隔请求切到约 105 ms；内核在远端特征发现阶段因状态仍为 `BT_CONFIG` 而拒绝该请求。[`0043` 草案](../../../patches/linux/7.2.9/drafts/0043-bluetooth-early-le-parameters.patch)允许已有 LE handle 在此阶段协商，保留参数校验。草案已编译并加载；初始化仍可能等待数秒至十余秒，尚未作为默认补丁或完整延迟修复发布。


## 笔身手势

轻捏、双击笔身和笔身上下滑动都由笔自己识别，经 BLE HID Report2 发给平板。Report2 是两个字节 `02 value`：按笔的 Report Map，`value` 是一个键盘数组用法（Usage Page 7，Usage 与逻辑范围都是 `0x4B..0x6E`）。新值替换旧值，`00` 表示没有按键。P81C 0.0.32 固件只发送下表的值。

| Report2 | 手势 | 笔内时序 | 通用 HID 映射 | 原厂按键 |
| --- | --- | --- | --- | --- |
| `02 6e` … `02 00` | 轻捏 | 按下发 `6e`，松开发 `00` | `KEY_F19`（189） | `BUTTON_7` |
| `02 58`、`02 00` | 双击笔身 | 发 `58`，延时后发 `00` | `KEY_KPENTER`（96） | `BUTTON_8` |
| `02 61`、`02 00` | 上滑 | 发 `61`，延时后发 `00` | `KEY_KP9`（73） | `BUTTON_9` |
| `02 5b`、`02 00` | 下滑 | 发 `5b`，延时后发 `00` | `KEY_KP3`（81） | `BUTTON_10` |

笔身滑动只给出方向，每次滑动是一次完整的按下和松开，没有距离或速度字段。

### 笔固件中的依据

固件按 [P81C 固件分析](p81c-firmware.md) 的方式去掉 `0x400` 字节头、映射到 VA 0。下面的 VA 加 `0x400` 即 IMG 偏移。

- **Report Map**：VA `0x849A4..0x84A2C`，正好 137 字节，与平板读到的描述符相同。Report1 是鼠标（8 个按键、X、Y、滚轮），Report6 是传感器页的 20 字节数据，Report2 是上述单字节键盘数组，Report5 是压力。
- **发送函数**：VA `0x3D8E4` 把一个字节交给 HID 发送函数 VA `0x43B74`（第二个参数为 8，即 Report2），失败最多重试 3 次并记录 `touch send code error`。全镜像对 VA `0x3D8E4` 的调用只传入 `00`、`58`、`5B`、`61`、`6E`；以 8 调用 VA `0x43B74` 的也只有这个函数。
- **触摸手势任务**：事件位 `0x02` 是双击（VA `0x3E318` 发 `58`，日志 `double.`），`0x04` 是上滑（VA `0x3E3A2` 发 `61`，日志 `slide up`），`0x08` 是下滑（VA `0x3E42C` 发 `5B`，日志 `slide down`），`0x40` 是轻捏按下（VA `0x3E5A4` 发 `6E`），`0x80` 是轻捏松开（VA `0x3E524` 发 `00`）。双击和滑动在 VA `0x3E34C`、`0x3E3D6`、`0x3E460` 延时后发 `00`。
- **开关**：状态块位于 RAM `0x200699D8`。`+2` 是双击开关，`+3` 是滑动开关，`+1` 是总开关；三者开机默认值都是 1（RAM 初始化数据 VA `0xAD108`）。命令表 VA `0xA57BC..0xA58A3` 中，`5F` 的处理函数 VA `0x41674` 写 `+2` 并回复 `DF 01 01`，`63` 的处理函数 VA `0x41600` 写 `+3` 并回复 `E3 01 01`，`64` 的处理函数 VA `0x415C4` 写 `+1`。回复中的状态固定为 1。这些开关在 RAM 中，换一个主机连接后仍是上一个主机设置的值；是否另存到闪存尚未追查。
- **书写时不发手势**：每个手势都先调用 VA `0x3AB10`。它要求 RAM `0x200699B8` 已置位；VA `0x3B05C` 在笔尖有压力时清除该位，无压力的计数超过 39 后才重新置位，计数周期尚未确定。另外状态块 `+4` 必须非零，`+0x1B`（双击、滑动）或 `+0x1C`（轻捏）必须为零；这两个字节由谁写入尚未追到。被拦下时日志为 `slide up no allow.`、`double no allow.p:%d,sk:%d` 等。
- 命令 `6F` 的处理函数 VA `0x413B0` 回复后写 `AIRCR` 复位笔，不能发送。`63`、`64`、`6F` 与其他 FE11 命令列在 [P81C 控制命令表](p81c-ota-protocol.md#日常控制与升级协议要分开)。

### 原厂系统的处理

来源为同机 HyperOS `OS4.0.4.0.XPYCNXM` 的 `system_ext`。

- `etc/input/MIUIInput.kl` 的 `Vendor_0022_Product_5081` 把 189、96、73、81 映射为 `BUTTON_7`（轻捏）、`BUTTON_8`（双击）、`BUTTON_9`（上滑）、`BUTTON_10`（下滑）。Linux 内核 `hid_keyboard[]` 把用法 `0x6E`、`0x58`、`0x61`、`0x5B` 转成的正是这四个键码。
- `framework/miui-services.jar` 的 `MiuiStylusTouchFilmManager` 把这四个键称为 `KEY_PINCH`、`KEY_DOUBLE_TAP`、`KEY_SLIDE_UP`、`KEY_SLIDE_DOWN`。`interceptKeyBeforeQueueing` 在游戏模式或灭屏时不处理这些键。设置项关闭时直接吞掉：`stylus_slide_option` 默认 1，`stylus_double_click_status` 默认 1，`stylus_pinch_status` 默认 5（快捷面板）。当前应用不接受该手势时同样吞掉；否则在双击、滑动按下时让平板向笔发振动命令（类型 3、4），再把键交给后续处理。`updateHapticAndDispatchStateLocked` 决定哪些应用接受：滑动给 `com.miui.creation`、短视频应用和下一条列出的阅读办公应用，双击给 `com.miui.creation` 与登记了笔身手势的应用。
- `BaseMiuiPhoneWindowManager.interceptKeyBeforeQueueingInternal` 对 `NEED_TOUCHFLIM_KEY_APP`（微信读书、WPS、PowerPoint、腾讯文档、相册、浏览器、飞书）把下滑转成 `KEYCODE_PAGE_DOWN`、上滑转成 `KEYCODE_PAGE_UP`。
- `MiuiStylusShortcutManager.interceptKeyByRemoteControl` 对 `etc/input/stylus_page_key_bw_config.json` 白名单中的短视频应用（排除其活动黑名单、分屏和小窗），在下滑或 Page Down 抬起时调用 `next()`，上滑或 Page Up 抬起时调用 `previous()`，即在宽度 0.65 处、高度 0.75 与 0.25 之间注入一次 60 ms 的滑动。
- `app/BluetoothExtension` 的 `MiuiBleOobHelperService` 在连接初始化时按 `stylus_slide_option` 发送 `63 01 01` 或 `63 01 00`，回复以 `E3 01` 开头，第三个字节为 1 表示成功。

### Linux 输出

| 手势 | 输出 | 处理位置 |
| --- | --- | --- |
| 上滑 | 笔键盘节点的 `KEY_PAGEUP` | 内核 `hid-xiaomi` |
| 下滑 | 笔键盘节点的 `KEY_PAGEDOWN` | 内核 `hid-xiaomi` |
| 双击 | tablet-tool 节点的 `BTN_STYLUS2` 按下、抬起，仅在笔处于感应范围时 | `piano-pen-owner.c` |
| 轻捏 | 短按是笔尖位置的一次点击，按住 600 ms 以上是 `BTN_STYLUS`，仅在感应范围时 | `piano-pen-owner.c` |

内核补丁 [`0025-hid-xiaomi-p81c-body-gestures.patch`](../../../patches/linux/7.3/0025-hid-xiaomi-p81c-body-gestures.patch) 在 `xiaomi_input_mapping` 中按 Report ID、字段长度和逻辑范围识别 Report2，把 `0x61`、`0x5B` 映射为 `KEY_PAGEUP`、`KEY_PAGEDOWN`，忽略 `0x6E`、`0x58` 和笔从不发送的其余用法。滑动因此不依赖本项目的用户空间程序，换用其他发行版时仍是 Page Up／Page Down；双击不再变成回车，滑动也不随 Num Lock 输出数字。原始 Report2 仍经 hidraw 交给触控进程中的笔输入。

双击是笔的第二个按钮。tablet-tool 节点上的按钮必须有笔的位置，笔离开感应范围时双击不产生输入。按 Linux 笔输入的惯例，不支持笔的应用通常把 `BTN_STYLUS` 当作中键、`BTN_STYLUS2` 当作右键；Krita、Xournal++ 等绘图应用可以把它绑定到橡皮擦或其他工具。一个新手势替换仍按住的轻捏时，按 HID 数组语义结束轻捏，但不把被打断的轻捏当作一次点击。

`piano-pen-bluetooth.py` 按 `p81c-radio.json` 的 `double_tap` 与 `slide` 在初始化时发送 `5F 01 01` 和 `63 01 01`，避免笔沿用上一个主机关掉的开关；不回应 `63` 的固件保留自己的默认值，不影响其余初始化。Linux 不在手势时发送振动命令，双击和滑动没有原厂那样的笔端振动。

### 平板上的检查

1. 运行 `sudo libinput debug-events --show-keycodes`，在笔身上滑、下滑：笔的键盘设备应显示 `KEY_PAGEUP`／`KEY_PAGEDOWN`。如果显示 `KEY_KP9`、`KEY_KP3`，或双击时出现 `KEY_KPENTER`，说明运行的内核还没有 `0025` 补丁。
2. 笔悬停在屏幕上方双击笔身，`libinput debug-events` 在 `Piano Focus Pen Pro` 上显示 `TABLET_TOOL_BUTTON` 的 `BTN_STYLUS2` 按下和松开；`sudo evtest` 选择同一设备可看到 `BTN_STYLUS2` 1 和 0。笔远离屏幕时双击不应有任何输出。
3. 需要原始数据时，按 VID/PID `0022:5081` 找到 hidraw 节点，只打印 Report2：

   ```sh
   grep -l 00000022:00005081 /sys/class/hidraw/hidraw*/device/uevent
   sudo python3 -c 'import os,sys; f=os.open(sys.argv[1], os.O_RDONLY)
   while True:
       r = os.read(f, 64)
       if r[0] == 2: print(r.hex(), flush=True)' /dev/hidrawN
   ```

   hidraw 允许多个读者，这条命令不影响笔输入。书写后立即滑动不会发出报告，停笔片刻再试。
4. `journalctl -b -u piano-touch.service | grep -e 630101 -e slide_setting_unanswered` 应找到 `"hex": "630101"` 的请求；出现 `slide_setting_unanswered` 表示这支笔的固件不回应滑动开关。

把 `/usr/share/piano/pen/p81c-radio.json` 中的 `slide` 或 `double_tap` 改为 `false` 后执行 `sudo systemctl restart piano-touch.service`，可以在笔端关闭对应手势。
