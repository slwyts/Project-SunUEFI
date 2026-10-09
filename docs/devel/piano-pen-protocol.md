# Piano 触控笔协议与原厂算法

2026-10-09。已从同版本原厂 `OS3.0.309.0.WPYCNXM` ROM 定向提取触控 HAL，并做静态核对。屏幕笔输入仍未实现，不因 BLE 配对、HAL 文件存在或符号解析成功而报告可用。

## 屏幕数据与 BLE 分开处理

现有 NT36532e 内核驱动将完整 THP 帧送到 `/proc/nvt_thp_stream`。当前 `piano-touch-view` 只生成手指 MT，选型时排除笔帧。原厂 [P81 parser](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.c#L2954) 明确区分类型6/7/9与17/29：payload `+64` 包含压力、按钮、悬浮状态和 Tip/Ring 原始阵列，阵列起点分别为 `+26` 与 `+36`。这里没有最终 X/Y、倾角。

原厂 HAL 读取 `/dev/xiaomi-touch` 的 frame/raw 共享区。最终点有两条上报路径：v1 写 Linux `input_event`；v2 写 point 共享区并以 `UPDATE_REPORT_POINT` 触发上报。[mmap/ioctl](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_operations.c#L169)、[内核 point receiver](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_device.c#L41)。两份 Piano ini 配置 v1，具体依据见下文。不能把 HAL 输出结构按偏移当作 SPI 帧来读。

已保存的 Focus Pen Pro BLE descriptor 只有 mouse/keyboard/sensor Input，没有 Digitizer、Output、Feature 或标准 FF。Android 已确认 Report5 携带真实笔尖压力，Report2 的 `02 6e`／`02 00` 对应轻捏 `KEY_F19` DOWN／UP；Report6 是独立姿态数据。Linux 已实机收到 Report2 轻捏和 Report6 姿态报告，但尚未收到 Report5 压力或屏幕 type29，因此目前不能绘画。屏幕位置与 hover 仍需真实屏幕矩阵和事件对照；轻捏协议已识别不代表桌面快捷键或笔端振动联动已完成。

## IC 扫描模式与原厂开关条件

2026-10-09补充：模式1和3表示不同笔型号，并非普通与高性能扫描档位。[原厂枚举](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.h#L245)定义 `SUPPORT_M80P=1`、`SUPPORT_N83P=2`、`SUPPORT_P81C=3`。同机已提取的 `nt36532_touch.ko`（SHA `d3ef0e85f98147d50784fc6b32d1d55e53834e8aa406e682e86c048ef663bac0`）也在 `nvt_set_cur_value` 的 `0xb038/0xb4a4..0xb4c8` 将笔ID8对应到模式3；同版HAL在 `0x29fc0..0x2a220` 选择内部profile3，型号日志为 `xiaomi_p81c`。

原厂的[连接处理](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.c#L3753)接收 `DATA_MODE_20`：高四位表示连接，低四位表示笔ID；P81C连接值为 `0x18`，断开值为 `0x08`。[扫描策略](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.c#L3571)要求屏幕醒着、笔已连接且没有磁吸充电；游戏模式会额外限制扫描，白名单可放行。连接或充电状态变化、固件恢复和屏幕恢复都会重新应用策略。连接条件本身不能证明压力来源；同机原生 Bluetooth 指令和真实 Report5 记录已经另外确认了压力传输路径。

实际开启分为两步：[扩展命令04](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx_ext_proc.c#L1356)发送 `50 BF 04 00 01 00`，使能笔扫描；随后[型号命令](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx_ext_proc.c#L663)发送 `50 7B 03` 选择P81C，并等待控制器ACK。关闭扫描将扩展命令04的值改为0。当前Linux的 `/proc/nvt_thp_stylus` 已包含这组命令、ACK和恢复路径，但默认启动没有请求开启扫描；读取该节点得到的是已成功请求的缓存值，不是控制器硬件状态查询。

后续应在现有触控进程内按真实连接、充电和屏幕状态管理P81C扫描，不新增服务或第二个FIFO读取者。开启扫描也不等于笔输入可用：Linux 已有 Report2／Report6，但仍没有 Report5 压力与 type29，不能绘画。`0x440` 的真实发送者已定位为 `com.android.bluetooth` 的原生 HID 处理，开源离线核心已能读取实际校准和压力报告；还需打通 Linux 矩阵输入与同源时序，再接标准 pen 输入，不能填猜测坐标或压力。

## 实际提取的 HAL 链

从[官方同版本 ROM](https://bigota.d.miui.com/OS3.0.309.0.WPYCNXM/piano-ota_full-OS3.0.309.0.WPYCNXM-user-16.0-6672ba521d.zip)读取 ZIP 目录与 payload manifest，核验官方 metadata SHA，并逐操作核验数据 SHA。为避免下载完整分区，构造了拒绝读取未知区间的只读稀疏 EROFS 视图，按 inode/pcluster 定向提文件。原始 metadata 保持不变；**部分镜像没有通过最终分区 SHA，不可刷写**。没有运行 HAL 或访问设备。

实际路径为 `/odm/bin/hw/vendor.xiaomi.hw.touchfeature-service`，它加载 `/odm/lib64/libtouchreport.so`，后者再加载同目录的 `libtouchreport_alg.so`、`libtouchreport_hal.so`、`libtouchreport_sensor.so`。五个 ARM64 文件及两份 Piano ini 已完整读取；各文件 SHA、操作验证和工具固定提交记录在本地 `private/captures/rom-OS3.0.309.0/selected-files.json`，这些原厂二进制不随源码提交。

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

本机 Android 的 `getevent -lp` 已确认 `NVTCapacitivePenP81c` 注册范围：X 0..213599、Y 0..319999、pressure 0..16383、distance 0..1、tilt X/Y ±60，另有 ABS_BRAKE 0..360。两份实际 ini 已提取，BOE/CSOT 的 hw/stylus 内容相同，project vendor ID 分别26/4，但 mapping 位置表不同。配置是 portrait `2136×3200`、三个轴翻转标志均0、project super-resolution100，stylus normal report factor10；Tip/Ring几何为 `12×40` 与 `60×8`。角度表为 `{0,1500,3000,4500,6000,7000}`。P81c id8/vendor3 选择默认 `stylus` 槽，差值表 `{0,28,51,67,81,87}`、校正阈值30/rate6；`stylus_2`／`stylus_3` 分别对应备选 vendor1/2，差值表 `{0,37,81,103,121,132}`、阈值37/rate8。controller mode3 不等于配置槽 `stylus_3`。

中间 hardware-info 构造已追明：`alg_read_config_param_core` 在 `0x1f88c..0x1f8ec` 读取完整键 `project_infor.super_resolution`，与内核 hardware 参数 `+8` 交叉核对，最终在 `0x1fbb0` 写入扩展 hwinfo `+0x24` 的 u16。`+0x14/+0x18` 是显示尺寸乘该因子，`+0x26/+0x27/+0x28` 是 x/y/xy flip。`alg_pass_hwinfo_core` 将此配置复制后，`alg_init_param` 在 `0x4385c/0x43868` 再将 resolution 写到 context `+0x2c`。这里的100与 stylus report factor10 是不同参数。

2026-10-08在 Android 触控服务仍运行时，核验本机五个 ELF SHA 和 PID/starttime，再按 maps 仅两轮读取各155字节配置。实际硬件头为 `2136×3200`、rx40/tx60、factor100；两个41字节 hwinfo 配置副本与48字节 context 配置均稳定，三个阶段的resolution都是100，scaled尺寸 `213600×320000`、三个 flip 均0；真实 report 方法指针为v1。context还按columns60/rows40选择了column/row extent `320000/213600`，不是固定min/max。没有停服务、操作笔、读取触点/FIFO/矩阵或运行额外厂商程序。原始记录在本地 `private/analysis/piano-pen-runtime-plan-20261008/`；这确认了当前配置与选路，仍不证明笔事件与图像坐标的单位已验证。

独立开源核心已经读取实际 mapping、选择 vendor3 默认参数，并独立处理真实 Report5 压力，主路线不再依赖补齐闭源 `stylus_total_data`／SC context。下面的原厂对象、回调和 BTF 记录保留用于对照；Linux 仍需取得真实 type29 后才能验证坐标、倾角和最终桌面事件。

## 最终36字节点与64字节 HAL 内部点

`stylus_point` 的动态对象大小确为36字节，地址 `0x15ca14`，GOT `0x913d0`。按实际写入和消费者核对：

| 偏移 | 已确认含义与依据 |
| --- | --- |
| `0x10` | 当前状态；`stylus_hover_pressure_judge` 在 `0x866e0/0x86740/0x867c4/0x867d0` 写入0..3 |
| `0x14` | 上一状态；`stylus_sts_bak` 在 `0x86b04` 复制当前状态 |
| `0x18` | 最终 callback gate；`0x88914..0x88918` 为0则不调用上报回调，不是直接 proximity |
| `0x1c` | 处理后压力；`0x866e4..0x866f4` 从 `stylus_total_data+4` 读取u16，经条件最小值/有符号扩展写入；其他路径明确清零。其输入还可能来自 `0x88448` 环形压力缓冲，不能把它称作原始压力直接透传 |
| `0x20` | distance布尔值；`0x88218..0x88228` 将内部输入 `+0x11d` 的非零判断写到total_data+7，再由 `0x882a4/0x882b8` 写入最终对象 |

`0x888e0` 的最终回调构造器读取真实 rodata `0x11240` 的四项表 **`{2,0,1,3}`**，把状态0/1/2/3分别映射为 action UP/DOWN/MOVE/HOVER。这与[公开 action 枚举](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_device.c#L12)数值一致；不是依据函数名推断。hover样本仍须对照evdev，不能仅以gate或压力生成BTN_TOUCH。

同一构造器产生的是**64字节** HAL 点：input style在0，坐标在8/12，tilt在24/28，distance在32，pressure在36，action在60。它将最终对象的压力/distance两项交换后写入。其余未明确初始化或解释的字保留未知，不补零生成事件。

共同 callback/GOT 链为：算法 `0x889fc` → libtouchreport 的 alg callback[0] `0x83ec` → 活动HAL子模块 `+0x80`（`register_hal_module` 在 `0x11c90..0x11c98` 安装）→ report interface `+0x48=0x21ae4` → `0x21c70` 按64字节点遍历 → 所选 report 方法 `+0x30`。

两份实际 ini 的 `input_device.report_version` 都为1，本次进程配置读取也确认实际选用v1。HAL `0x219bc..0x219f0` 读取这个完整键名，仅值2选v2，其余选v1；v2初始化报错也可在 `0x21304..0x21350` 回退v1。v1方法 `+0x30=0x25f9c` 在 `0x260a0..0x26118` 将上述内部点构造为24字节 `input_event`：BTN_TOUCH由pressure非零决定，BTN_TOOL_PEN由 `(distance | pressure)` 非零决定；X/Y、pressure、distance、tilt分别直接取已列字段。action UP分支清零这些键和轴。`0x2613c` 调用 `write`，按配置写所选输入fd。这条路径没有把64字节点交给mmap receiver。

v2方法 `+0x30=0x26dec` 是另一条路径：HAL `0x26804..0x26834` 以cmd2/arg4选择point共享区并mmap4096字节；`0x26eb4..0x26ec8` **直接复制64字节** 到映射；`0x27018..0x27024` 再调用cmd6 `UPDATE_REPORT_POINT`。这里仍未找到64→56重排，不能启用这条路径后假定布局相同。

2026-10-08从本机只读导出的 `/sys/kernel/btf/xiaomi_touch`，配合实际base `/sys/kernel/btf/vmlinux`，确认 `hal_report_piont_t` 指向**56字节**结构：input_style0，x4/y8，prop[10]12..51，action52。匿名union的finger/stylus布局相同；实际ACTION枚举为DOWN0/MOVE1/UP2/HOVER3，与公开固定MiCode一致。BTF还确认 `report_touch_event(s8 touch_id, u8 event_count)`；`point_data` 并非这份BTF中的独立类型。BTF只能证明结构和函数类型，不能代替正在运行的选路证据。实际56字节receiver与HAL64字节内部点的差异因此已经确认；两份v1配置解释了原厂不必使用该mmap接口。泛用receiver pressure上限8191也不能代替本机P81c已注册的16383。

结构化事实、原厂库/BTF SHA、GOT、实际地址和剩余项保存在 `private/analysis/piano-pen-final/facts.json`。本次新增证据只有内核BTF与输入能力声明的只读导出；静态分析未执行厂商ELF、采笔触点或创建input设备。

完成这些字段与计算链的还原后，用一次短的 raw17/29与Android evdev同步记录检查比例、proximity和tip边界，再接入现有触控进程的独立 pen uinput，保留手指 MT与唯一 FIFO。使用 libinput/Wayland tablet-tool接口；悬浮时工具在范围内而笔尖未触地，不能照抄泛用原厂 reporter 将所有非UP动作都设为 `BTN_TOUCH=1`。未知倾角、按钮语义或姿态不填零宣称支持。

## 笔的磁吸检测与无线充电

SC96231 在 Android 的 `i2c9-0038` 已绑定，MCA 创建的属性组为 `/sys/class/xm_power/charger/wls_rev_charge/`。原厂模块 show 指令已确认 `wls_fw_state`、`reverse_chg_mode`、`reverse_chg_state`、`pen_ss_voltage` 直接返回缓存值；仅按这四项观察，不根据文件可读权限批量读取整组。`pen_soc` 在某些连接/hall状态会通过 SC96231 的 `regmap_raw_read` 读取芯片并更新缓存，本轮排除它，不能宣称已测笔电量或充电。[固定 MiCode 的 MCA 路径](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/power/supply/mca)只是指向 `vendor/xiaomi/proprietary/mca/driver/mca` 的链接，没有可直接移植的完整实现；上述性质来自本机固定模块静态核对。

2026-10-09的原厂最终DTB（本地 `private/analysis/piano-camera-privacy-led-20261009/android-live.dtb`，SHA `67518975f4a464ecc8c330373b5d6ac2cb7724fceba3481051cfbba418f4df95`）将芯片放在 `/soc/qcom,qupv3_2_geni_se@8c0000/i2c@894000/sc96231@38`：硬件为SE13、I²C地址 `0x38`，Android的适配器编号9不能作为Linux的固定总线号。GPIO来自TLMM，具体连接如下。

| 原厂属性 | TLMM GPIO | 用途与已知配置 |
| --- | --- | --- |
| `rx-int` | 78 | 芯片中断；上拉，模块请求下降沿和oneshot中断 |
| `reverse-txon-gpio` | 14 | TXON控制；DT有 `reverse-txon-low`，默认输出低 |
| `reverse-boost-gpio` | 120 | Boost控制，默认输出低 |
| `hall3-int` / `hall4-int` | 200 / 196 | 两路Hall，上拉输入，模块请求双沿和oneshot中断 |
| `hall3-s-int` / `hall4-s-int` | 201 / 197 | 两路补充Hall，上拉输入，双沿中断 |
| `hall-ppe-int` / `hall-ppe-s-int` | 55 / 59 | PPE Hall，上拉输入，双沿中断；与普通Hall走不同事件 |
| `lp-nen-gpio` | 99 | 低功耗控制，默认输出高 |
| `keyboard-front-gpio` / `keyboard-back-gpio` | 80 / 11 | 原厂还读取键盘位置；移植时须避免与现有键盘驱动重复占用 |

该节点没有 `*-supply` 属性，原厂模块也没有regulator或标准 `power_supply_register` 调用，不能由此认定芯片无需供电，或已有标准充电接口。普通Hall工作函数 `0x18e4..0x18f4` 发MCA事件78，后者在 `0x2698..0x26ac` 将值非零通知触控模块；这是磁吸位置，不是充电电流或充满状态。PPE Hall发事件79，必须另行核对其组合判定，不能照搬事件78或用 `reverse_chg_mode=0` 表示笔未吸附。

当前Linux实际DT仍保留这个子节点，但父节点的原厂 `qcom,i2c-geni` 与主线驱动要求的 `qcom,geni-i2c` 不匹配，`894000.i2c` 未绑定，尚未产生该总线的 `0x38` I²C设备；SC96231充电驱动仍未实现。磁吸 Hall 输入不依赖该 I²C 总线，已有独立标准输入候选；充电功能则仍需按主线 `i2c13` 定义转换总线及其时钟、DMA、引脚等供应者。无线充电、电量和标准power_supply属性还需要芯片协议、固件及供电流程，不能用虚拟设备代替。状态缺失时保留unknown，不默认填“未充电”。本节原厂充电属性核对没有修改充电状态。

磁吸输入可以与充电总线分开实现。普通 Hall3/4 的总体存放状态为 `!raw_gpio200 || !raw_gpio196`，对应一个标准 `EV_SW/SW_PEN_INSERTED`；两路 `gpio-keys` 使用同一个 code 不会计算 OR，后来的值会覆盖前一路。PPE 是不同的算法，读取四路 Hall 和两个键盘位置，不能合并成六路 OR。目前尚未动态确认 Focus Pen Pro 使用哪条路径。

原厂 Hall3/4 pinctrl 明确使用 GPIO 输入、2 mA、上拉和 `qcom,apps`；主线的正常 GPIO 请求会取得 AP 引脚所有权。Linux 当前显示 EGPIO 仅描述尚未接管的状态，不能由此推导引脚禁止使用。模块中的 `power_on_pen_check` 是软件开机延迟门：初始为 0，probe 延迟 3250 jiffies 后直接设为 1，再延迟 250 jiffies 排 Hall 扫描；该 worker 不查询供电、I²C 或固件状态，因此不能把它当作硬件就绪接口。PPE 路径不使用这个门。

[磁吸输入设备树候选](../../linux/dts/drafts/piano-pen-dock.dtso)描述普通两路检测，引用真正的 `piano_tlmm_ml`；旧的 `tlmm` 标签仍指向旧 provider，不能使用。候选已编译并合并检查，节点在默认产品中保持 disabled；下述动态覆盖已实机绑定。无线充电和电量仍是独立缺项。

[输入驱动候选](../../patches/linux/7.2.9/drafts/0018-piano-pen-dock.patch)只注册一个存放开关，按设备树极性计算两路 OR，在初始化、双沿中断和恢复时读取真实状态；读取失败不报告“已取出”。共享锁串行两路线程化中断，卸载时先同步释放中断再注销输入设备。驱动通过当前 590 内核的 ARM64 编译；不提供磁吸唤醒、PPE 路径或充电属性。它仍是候选，实机结果如下，尚不改变功能支持表。

随后使用当前内核和 `Module.symvers` 生成可加载模块，并通过一次性 OF overlay 在实机绑定。GPIO200 实际为低、GPIO196 为高，标准 `EVIOCGSW` 查询返回 `SW_PEN_INSERTED=1`，与两路低有效 OR 一致。完成后覆盖和两个模块已卸载，输入设备与新增节点均移除；GPIO 保持普通 AP 输入模式，没有声称恢复为加载前的 EGPIO 复用状态。内核在动态覆盖时记录了现有 lid-switch 和 timer 的 device-link 警告，内核 taint 值没有改变，桌面、触屏和相机服务仍正常。

首次绑定记录在 `private/analysis/piano-pen-dock-live-20261009/`，模块构建记录在 `private/analysis/piano-pen-dock-modules-590-20261009/`。之后取下笔再绑定候选，两路 GPIO 均为高，`EVIOCGSW` 读取 `SW_PEN_INSERTED=0`，与首次吸附状态的读数不同。尚未验证双沿中断连续上报；本轮采集脚本按这一真实开关值转发状态，公开工具 [`piano_pen_control.py`](../../tools/host/piano_pen_control.py) 也提供同一读取路径。取出命令 `51 01 01` 收到 `d1 00`，另一控制命令 `5b 01 01` 收到 `db 01 01`。这些结果证明候选位置检测与控制回应，不能据此报告无线充电或笔输入可用。候选仍未接入默认内核。

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

Linux唯一流读取者 `piano-touch-view` 现有可选 [`--capture FILE --capture-seconds N`](../../tools/patches/piano-touch-view-raw-capture.patch)：默认关闭，N为1..10秒、默认5秒，内存上限16MiB。它保存自己已读到的完整NTP1记录，包含原内核头、序列、时间戳、epoch与所有普通/SC/17/29帧；不只保留笔帧，不另开FIFO。窗口结束或达到字节上限后一次写出新文件并继续原有输入，stderr给出实际字节数、错误和帧类型统计；应选择本地tmpfs新路径，不能覆盖旧捕获。后续调试应让现有owner带此选项启动，不能在服务旁再运行第二个reader。两个runtime builder通过同一 `derive_touch_source` 应用这个默认关闭补丁。此捕获入口已在现有触控进程中实机使用，最新结果见下文。

当前 `nt36532e` 的 `nvt_thp_read_frame` 原样读SPI，`nvt_thp_publish_frame` 检查补码后把原buffer直接送同一FIFO；没有原厂Android的checksum→CRC32重写。因此上述Linux捕获仍保留原始双校验字段，但内核valid flag只检查头补码，不能代替CPU适配器的两层校验。当前驱动没有1032B common7/0x440接口，但下述实际writer已经证明这条压力路径绕过kernel queue；不再把新增kernel读取API作为前提。不能从raw29 pressure拼接一个common记录，也不能把HAL发送sysfs `touch_thp_ic_cmd_data` 当作压力读取来源。

Python 调用可使用 `parse_metadata(data)`、`read_config(path)`、`profile(config, vendor_id)` 和 `factory_tilt_component(dx, dy, resolution, calibration)`。最后一个函数没有 CLI 事件输出，要求真实的已算 Tip/Ring 差值和已确认的 runtime resolution；配置 reader 返回的 vendor profile 只是配置映射，不证明当前连接的笔采用哪一个 profile。参数单位与最终 input 上报仍按上面的缺项处理。

`parse_stylus_point(data)`、`parse_factory_hal_point(data)`、`parse_kernel_report_point(data)` 分别读取调用者提供的精确36/64/56字节dump，返回已确认布局；64字节解析额外列出v1的静态键值判断，56字节解析保留prop[]原字。它们不接受SPI帧、不转换坐标单位、不归一化压力、不产生uinput/libinput事件。`config` 现在也输出 `input_device` 中的report版本。没有实测dump时，静态布局不能当作笔输入已可用。

`parse_runtime_hw_header(data)`、`parse_runtime_hwinfo_prefix(data)`、`parse_runtime_context_config(data)` 分别解析精确9/41/48字节的配置片段。context片段从已核对指针的 `+0x10` 开始；解析器只读调用者提供的文件，不跟随指针、不读进程。byte8保留原字，不作为ready标志。配置解析不能替代最终坐标、压力、hover与按钮事件的实测。

## 原厂算法 worker

固定 ALG 只有 `libdl/liblog/libc/libm` 四项依赖、32个 Bionic 导入，没有 libc++、Binder、属性或设备 ioctl 导入；普通AArch64 RELA也无需 Android packed-reloc loader。[GNU worker 源码](../../linux/userspace/pen/)已实际完成隔离的配置/init：派生库只修改链接元数据，原库与指令区不变；真实 GNU mutex/标准流适配处理 Bionic mutex 存储与 `__sF` 差异。2026-10-08在无网络、只读文件系统、空 `/dev` 的 Bubblewrap/QEMU 中，用真实原厂ini和9字节硬件头执行normal/stylus配置reader、构造context及stylus init/exit，exit0；resolution100、scaled尺寸213600×320000、columns60/rows40和stylus enabled1与已有配置一致。未加载HAL startup、执行帧处理或生成uinput。运行暴露并修复了GNU版本表标签残留和ALG level0空日志回调两处故障，没有填充假callback。

实际 stylus 接口的slot0/1是init/exit，slot8 `0x881e0` 将内部frame的四组signed16矩阵扩展到算法缓冲，slot7 `0x87fcc` 执行坐标/压力/倾角处理并在尾部调用 `0x888e0` 报告构造器；后者明确检查callback是否存在。输入需要HAL内部frame及非对齐指针，并非SPI payload。原厂v2 decoder只在真实类型29时复制笔metadata和四矩阵；类型17保持17，ALG再发外部command23/value2，不能强制改成29。

[CPU适配器](../../linux/userspace/pen/piano-pen-frame.c)已按同版HAL的 `0x2fe44/0x30718` 实现类型29的笔输入前缀：内部跨度1297B，header在0x3c、36B metadata在0x10d、四个pointer在0x131/139/141/149，20B trailer在0x151。矩阵长度来自实际ini的12×40、60×8，每组480个int16，四组共3840B。它核验原始包的外层与笔尾层additive checksum/补码/边界；手指四分包位图齐全且累计和为10时才发布完整矩阵，重复分包不能替代缺失分包。频率跳转请求保留原字，不在CPU worker发硬件指令或返回假ACK。Android mmap中被内核改写成CRC32的包不属于此输入格式。

worker的 `--decode29` 接受已捕获的原始包；`--prepare29` 还需要真实外部1032B common记录，byte1=7、u16+2=0x440，原厂slot2读取body中的两个u32 `+0xc/+0x10` 来更新压力ring。真实本机BTF确认common_data是8B头加256个s32 body，`+8` 是data_buf[0]，不是长度；u16+4的data_len是s32数量，此入口要求3..256个实际body字。HAL的另一条common读取路径已追明为 `0x11dc8` 打开 `/dev/xiaomi-touch`，touchid/hardware ioctl后在 `0x11fdc..0x12000` 把同一FD交给getcmd等子模块，再由 `0x160f8/0x16390` 原样读取/转发；这不证明压力记录由kernel生产。

同版ROM service SHA `3e4549b2…271f42` 已确认真正的0x440 writer：外部长vector入口 `0x16b88` 在 `0x16c70/0x16cac` 特判mode0x440，调用 `0x19df8`；后者在 `0x19e98..0x19eac` 写入touchid/cmd7/mode0x440/data_len，把外部s32数组原样复制到body。`0x19eec→0x1df40` 通过实际dlsym安装的 `thp_daemon_cmd_process` 直接进入libtouchreport与ALG压力ring，绕过kernel common queue。同版 `/vendor/lib64/vendor.xiaomi.hw.touchfeature-V1-ndk.so` SHA `42aac267…c5e00d` 已明确映射该方法为 **`ITouchFeature::setModeLongValue(touch_id, mode, length, values)`，Binder transaction8**：proxy `0x9e48` 依次写三个int32与int32数组，vtable `+0x68` 对应service `0x400e0→0x16b88`。同机动态记录和原生客户端指令已确认外部发送者为 `com.android.bluetooth` 的 `bt_main_thread`：`libbluetooth_jni.so` 接收真实15字节 BLE Report5，把每个字节零扩展为 int32，再调用 `setModeLongValue(0,1088,15,values)`。压力取报告ID之后的两个小端字节，不来自 raw29 的 metadata。固定MiCode中cmd7是SET_LONG_VALUE、0x440是DATA_MODE_141；普通ioctl的long-value只处理mode15，不能用这个泛用入口解释当前pressure writer。Android 的专用 HID 消费分支成功后跳过普通 UHID 转发，解释了原厂采集时 hidraw 报告数为0；Linux BlueZ 的路径仍按 Linux 实际报告判断。worker继续接收真实外部1032B记录，由现有唯一owner转交；不增加假kernel API或第二FIFO。

2026-10-09已完成原厂 Binder／uprobe 定向记录：先捕获175次 mode1088、length15的真实调用，再采集651个完整 Report5 压力报告。离线核心逐条读取真实15字节数据，记录中的压力范围为0..12288，包括32次真实零值；没有构造替代报告。原始记录在本地 `private/analysis/piano-pen-live-20261009/android-pressure2/`，原生发送者指令与字段依据在 `private/analysis/piano-pen-producer-20261009/`。临时追踪工具只用于开发，不加入产品。

CPU前缀仍不能交给完整 `parse_data_package`：该函数还读取真实主触摸/SC缓冲与噪声状态。独立开源核心不依赖这一闭源入口。最新 Linux 操作窗口内收到496条 HID 报告：488条长度21的 Report6 姿态，以及8条长度2的 Report2（`02 6e`／`02 00`，四对轻捏按下／松开）；Report5 为0，同步捕获的1728条 NVT记录全部为 type3。具体结果保存在本地 `private/analysis/piano-pen-live-20261009/linux-capture5-result.json`。这证明蓝牙 HID 并非整条不通，当前缺口是压力报告与屏幕笔矩阵，尚未生成绘画输入。轻捏报告已收到，但还没有映射桌面操作或笔端振动。

## 独立开源核心与打包入口

[`linux/userspace/pen/`](../../linux/userspace/pen/README.md) 的 C ABI 核心基于 Apache-2.0 开源项目 [xiaomi-sheng-thp 的固定提交](https://github.com/ianchb/xiaomi-sheng-thp/tree/34046210932d654a4c0df0121ecc31c008f8148c)，使用 Piano 的真实尺寸、原厂 mapping 和 P81c 默认参数，不沿用 Sheng 的位置表。核心校验两层原始 checksum与四分包位图，再处理 Tip/Ring 矩阵、倾角和真实 Report5 压力；当前已编译并读取实际 BOE／CSOT 配置，但尚无实际 type29 用来验证最终坐标。

在仓库根目录运行 `make -C linux/userspace/pen`，生成 `build/pen-core/libpiano-pen-core.a` 与 `build/pen-core/piano-pen-offline`。C ABI 用于随后接入已有触控进程，离线工具不创建输入设备。BLE 时间戳必须采用 `CLOCK_BOOTTIME`，与 NTP记录的 `ktime_get_boottime()` 同源；不能在睡眠后混用 MONOTONIC 时间。选择原厂校准应以触控控制器的 LCD ID／实际工厂配置为依据，不能根据显示面板标签代选。当前触控 LCD ID为1，加载 BOE触控固件，即便显示链路采用 CSOT配置。

两个 runtime builder 已通过 `tools/build_piano_runtime_helpers.py` 中的 `build_pen_core()` 共用上述 Makefile。默认 native runtime bundle安装 `/usr/bin/piano-pen-offline`、Apache-2.0 许可与来源说明；manifest记录多文件源码、头文件、实际编译器和目标静态 C++ 库，`runtime.stage()`把来源记录一并放入 rootfs。构建不读取 ini或设备。Clang使用目标 GNU sysroot 的 C++ 开发文件；GCC优先使用配套 G++，缺失时使用已有 Clang与同一目标 sysroot。

静态库作为构建输出保留，后续才能链接到原有触控进程；当前没有改变该进程的默认行为或启用绘画输入，也没有新增后台服务或第二个 THP reader。`tools/stage_piano_ram_hardware.py` 继续只处理已有脚本与配置，不承担 C++编译。

`tools/package_bsp.py` 当前是架构无关配置包，拒绝 ELF程序，原生程序应留在 runtime层。原厂 ini是架构无关数据，但当前公开 `piano-firmware` 只有触控固件 bin，没有这两份校准文件。构建应显式接收 BOE／CSOT校准输入及 ROM来源、文件摘要，安装到 `/usr/share/piano/pen/calibration/`，本地和 CI使用同样输入。不能默认取某台开发电脑的 `private/` 文件，或在缺失时静默套用另一面板的位置表。校准数据的自动提取与打包仍待接入，离线工具可以先由调用者提供实际 ini使用。
