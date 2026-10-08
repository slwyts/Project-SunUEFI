# Piano 触控笔协议与原厂算法

2026-10-08。已从同版本原厂 `OS3.0.309.0.WPYCNXM` ROM 定向提取触控 HAL，并做静态核对。屏幕笔输入仍未实现，不因 BLE 配对、HAL 文件存在或符号解析成功而报告可用。

## 屏幕数据与 BLE 分开处理

现有 NT36532e 内核驱动将完整 THP 帧送到 `/proc/nvt_thp_stream`。当前 `piano-touch-view` 只生成手指 MT，选型时排除笔帧。原厂 [P81 parser](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.c#L2954) 明确区分类型6/7/9与17/29：payload `+64` 包含压力、按钮、悬浮状态和 Tip/Ring 原始阵列，阵列起点分别为 `+26` 与 `+36`。这里没有最终 X/Y、倾角。

原厂 HAL 读取 `/dev/xiaomi-touch` 的 frame/raw 共享区。最终点有两条上报路径：v1 写 Linux `input_event`；v2 写 point 共享区并以 `UPDATE_REPORT_POINT` 触发上报。[mmap/ioctl](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_operations.c#L169)、[内核 point receiver](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_device.c#L41)。两份 Piano ini 配置 v1，具体依据见下文。不能把 HAL 输出结构按偏移当作 SPI 帧来读。

已保存的 Focus Pen Pro BLE descriptor 只有 mouse/keyboard/sensor Input，没有 Digitizer、Output、Feature 或标准 FF。轻捏、滑动和笔端震动的具体协议尚未确认；屏幕 hover 要从屏幕端工具位置与 proximity 得到。小米的[产品说明](https://www.mi.com/global/product/xiaomi-focus-pen-pro/)不能替代这些实际协议字段。

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
| `calibrate_coordinate_tilt` `0x831e4` | 根据 enable、threshold、rate 参数校正 tip 坐标；公开模块尚未实现此步骤 |
| `stylus_coordinate_flip` `0x834cc` | 按上下文中的翻转/交换标志改变坐标 |
| `stylus_report` `0x86ab0` | 将两路坐标交换写入最终对象 `0/4`；working-state为1时交换写入 tilt `8/12` |
| `get_stylus_data` `0x859e0` | 返回 `stylus_total_data`（60096字节），不是最终 `stylus_point`（36字节） |

公开模块 `tools/piano_pen_protocol.py` 解析源码已定义的 raw metadata/矩阵、核验外层 checksum，并读取外部 ini；不内嵌原厂文件，也不生成坐标或 input 设备。`factory_tilt_component` 只还原接收已算 Tip/Ring 差值的 `calculate_tilt` 分段计算，要求调用者显式提供 runtime resolution，拒绝超出原函数 int32 中间值范围的输入。它没有应用配置里的 `tilt_calibration_enabled/threshold/rate`，没有实现 `calibrate_coordinate_tilt`、坐标解算或最终 report，因此不能当作最终笔 tilt；尚未与原厂执行结果对照。反汇编与字段记录保存在同一 private 目录。

## 标准输入接入前的缺项

本机 Android 的只读 `getevent -lp` 已确认 `NVTCapacitivePenP81c` 注册范围：X 0..213599、Y 0..319999、pressure 0..16383、distance 0..1、tilt X/Y ±60，另有 ABS_BRAKE 0..360。这证明当前输入设备的声明，不证明笔事件已采到，也不能把 raw 压力直接冒充最终归一化输入。两份实际 ini 已提取。BOE/CSOT 的 hw/stylus 内容相同，project vendor ID 分别26/4。配置是 portrait `2136×3200`、三个轴翻转标志均0、project super-resolution100，stylus normal report factor10；Tip/Ring几何为 `12×40` 与 `60×8`。角度表为 `{0,1500,3000,4500,6000,7000}`；默认差值表 `{0,28,51,67,81,87}`，model2/3为 `{0,37,81,103,121,132}`，model2/3校正阈值37、rate8。这些参数不再是缺失项。

中间 hardware-info 构造已追明：`alg_read_config_param_core` 在 `0x1f88c..0x1f8ec` 读取完整键 `project_infor.super_resolution`，与内核 hardware 参数 `+8` 交叉核对，最终在 `0x1fbb0` 写入扩展 hwinfo `+0x24` 的 u16。`+0x14/+0x18` 是显示尺寸乘该因子，`+0x26/+0x27/+0x28` 是 x/y/xy flip。`alg_pass_hwinfo_core` 将此配置复制后，`alg_init_param` 在 `0x4385c/0x43868` 再将 resolution 写到 context `+0x2c`。这里的100与 stylus report factor10 是不同参数。

2026-10-08在 Android 触控服务仍运行时，核验本机五个 ELF SHA 和 PID/starttime，再按 maps 仅两轮读取各155字节配置。实际硬件头为 `2136×3200`、rx40/tx60、factor100；两个41字节 hwinfo 配置副本与48字节 context 配置均稳定，三个阶段的resolution都是100，scaled尺寸 `213600×320000`、三个 flip 均0；真实 report 方法指针为v1。context还按columns60/rows40选择了column/row extent `320000/213600`，不是固定min/max。没有停服务、操作笔、读取触点/FIFO/矩阵或运行额外厂商程序。原始记录在本地 `private/analysis/piano-pen-runtime-plan-20261008/`；这确认了当前配置与选路，仍不证明笔事件与图像坐标的单位已验证。

剩余静态工作集中在 raw2D阵列到 `stylus_total_data` 的预处理、`update_stylus_param` 的活动笔 profile 选择和压力环形缓冲预处理。本机内核 point 结构已用实际 BTF 核对；最终对象和两条 report 路径已得到下列实际指令依据，仍未做设备事件对照。

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

SC96231 在 Android 的 `i2c9-0038` 已绑定，MCA 创建的属性组为 `/sys/class/xm_power/charger/wls_rev_charge/`。原厂模块 show 指令已确认 `wls_fw_state`、`reverse_chg_mode`、`reverse_chg_state`、`pen_ss_voltage` 直接返回缓存值；仅按这四项观察，不根据文件可读权限批量读取整组。`pen_soc` 在某些连接/hall状态会通过 SC96231 的 `regmap_raw_read` 读取芯片并更新缓存，本轮排除它，不能宣称已测笔电量或充电。公开 MiCode 内核中的 MCA 路径只是指向 `vendor/xiaomi/proprietary/mca/driver/mca` 的链接，完整 vendor source仍未取得；上述性质来自本机固定模块静态核对。

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

Python 调用可使用 `parse_metadata(data)`、`read_config(path)`、`profile(config, vendor_id)` 和 `factory_tilt_component(dx, dy, resolution, calibration)`。最后一个函数没有 CLI 事件输出，要求真实的已算 Tip/Ring 差值和已确认的 runtime resolution；配置 reader 返回的 vendor profile 只是配置映射，不证明当前连接的笔采用哪一个 profile。参数单位与最终 input 上报仍按上面的缺项处理。

`parse_stylus_point(data)`、`parse_factory_hal_point(data)`、`parse_kernel_report_point(data)` 分别读取调用者提供的精确36/64/56字节dump，返回已确认布局；64字节解析额外列出v1的静态键值判断，56字节解析保留prop[]原字。它们不接受SPI帧、不转换坐标单位、不归一化压力、不产生uinput/libinput事件。`config` 现在也输出 `input_device` 中的report版本。没有实测dump时，静态布局不能当作笔输入已可用。

`parse_runtime_hw_header(data)`、`parse_runtime_hwinfo_prefix(data)`、`parse_runtime_context_config(data)` 分别解析精确9/41/48字节的配置片段。context片段从已核对指针的 `+0x10` 开始；解析器只读调用者提供的文件，不跟随指针、不读进程。byte8保留原字，不作为ready标志。配置解析不能替代最终坐标、压力、hover与按钮事件的实测。

## 原厂算法 worker

固定 ALG 只有 `libdl/liblog/libc/libm` 四项依赖、32个 Bionic 导入，没有 libc++、Binder、属性或设备 ioctl 导入；普通AArch64 RELA也无需 Android packed-reloc loader。[GNU worker 源码](../../linux/userspace/pen/)已实际完成隔离的配置/init：派生库只修改链接元数据，原库与指令区不变；真实 GNU mutex/标准流适配处理 Bionic mutex 存储与 `__sF` 差异。2026-10-08在无网络、只读文件系统、空 `/dev` 的 Bubblewrap/QEMU 中，用真实原厂ini和9字节硬件头执行normal/stylus配置reader、构造context及stylus init/exit，exit0；resolution100、scaled尺寸213600×320000、columns60/rows40和stylus enabled1与已有配置一致。未加载HAL startup、执行帧处理或生成uinput。运行暴露并修复了GNU版本表标签残留和ALG level0空日志回调两处故障，没有填充假callback。

实际 stylus 接口的slot0/1是init/exit，slot8 `0x881e0` 将内部frame的四组signed16矩阵扩展到算法缓冲，slot7 `0x87fcc` 执行坐标/压力/倾角处理并在尾部调用 `0x888e0` 报告构造器；后者明确检查callback是否存在。输入需要HAL内部frame及非对齐指针，并非SPI payload。原厂v2 decoder只在真实类型29时复制笔metadata和四矩阵；类型17保持17，ALG再发外部command23/value2，不能强制改成29。

[CPU适配器](../../linux/userspace/pen/piano-pen-frame.c)已按同版HAL的 `0x2fe44/0x30718` 实现类型29的笔输入前缀：内部跨度1297B，header在0x3c、36B metadata在0x10d、四个pointer在0x131/139/141/149，20B trailer在0x151。矩阵长度来自实际ini的12×40、60×8，每组480个int16，四组共3840B。它核验原始包的外层与笔尾层additive checksum/补码/边界；手指四分包实际齐全、原厂累计和为10时才发布完整矩阵。频率跳转请求保留原字，不在CPU worker发硬件指令或返回假ACK。Android mmap中被内核改写成CRC32的包不属于此输入格式。

worker的 `--decode29` 接受已捕获的原始包；`--prepare29` 还需要真实1032B common记录，byte1=7、u16+2=0x440，原厂slot2读取body中的两个u32 `+0xc/+0x10` 来更新压力ring。真实本机BTF确认common_data是8B头加256个s32 body，`+8` 是data_buf[0]，不是长度；[公共生产函数](https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/6957f6b646d1c919e175e6f9000eb50c8635273c/xiaomi/xiaomi_touch_core.c#L182)把u16+4的data_len作为s32数量，所以此入口要求3..256个实际body字。HAL `0x160f8` 整块pread1032B、consumer `0x16390` 原样转发，没有由raw29的pressure字段构造这份记录。未经证实不能把压力来源归为BLE。CPU前缀也不能交给完整 `parse_data_package`：该函数还读取真实主触摸/SC缓冲与噪声状态。当前没有真实29包、common压力/profile记录及最终事件同步数据，因此新路径只完成源码与ARM64编译；原有隔离配置/init再次通过，尚未执行帧prepare/坐标解算或生成uinput。没有第二FIFO reader、假触点或模拟输出。
