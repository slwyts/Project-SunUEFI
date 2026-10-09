# Piano 手写笔核心

这里保存 Piano 的独立开源笔解码核心、离线工具及保留的原厂算法诊断 worker。它们不会启动第二个触控服务，也不会自行读取 THP FIFO 或向桌面注入输入。

**Linux 目前仍不能用这支笔绘画。** Android 的真实压力来源与轻捏报告已经确认，开源核心也已完成编译和原厂校准读取；Linux 已收到 Report2 轻捏按键和 Report6 姿态报告，但仍未收到 Report5 压力或屏幕笔 type29；触控采集仍只有手指 type3。解码代码准备好，不等于已经得到真实笔输入。

## 开源实现

`piano-pen-decoder.hpp/.cpp` 派生自 `ianchb/xiaomi-sheng-thp` 固定提交 `34046210932d654a4c0df0121ecc31c008f8148c`，保留 Apache-2.0 许可。`piano-pen-core.h/.cpp` 提供 C 接口，让现有唯一的触控读取进程可以调用它。来源与修改范围见 [OPEN_CORE_ORIGIN.txt](OPEN_CORE_ORIGIN.txt)，完整许可见 [Apache-2.0.txt](Apache-2.0.txt)。

核心接收去掉 stream header 和 257 字节 transport 的原始 payload，复用 `piano-pen-frame.c` 的两层原始校验和、几何检查与四分包位图。只有四份主触摸数据真实齐全才标记完整；type17 不会改成 type29。矩阵经过轴分离、坐标计算、倾角校正与滤波后，返回带有效位的结果。未知压力、坐标或倾角不能补零上报。

校准从调用者提供的真实原厂 ini 读取，没有内嵌 Sheng 的位置表或尺寸。Piano 的 40/60 轴从两个 2400 点 mapping 抽取，pitch 按原厂整数公式计算。P81c 的 vendor3 选择默认 `stylus` 参数，不是 `stylus_3` 槽。输出声明为 portrait：X `0..213599`、Y `0..319999`、压力 `0..16383`、倾角 ±60；不额外交换坐标，也不除以 report factor10。

**Touch 校准必须按触控 LCDid／固件选择，不能按 Display 面板名称选择。** 当前设备触控 LCDid 为1，使用 BOE 触控固件及 BOE ini，即使显示链路标识为 CSOT。两份 ini 的位置表不同，必须分别保留。

`piano_pen_core_report5()` 接收真实15字节 BLE Report5。Report ID 后两个字节组成小端压力，不能用 raw29 metadata 构造替代报告。BLE 时间戳使用 `CLOCK_BOOTTIME`，与 NTP 驱动的 `ktime_get_boottime()` 同源；睡眠后不能混用 `CLOCK_MONOTONIC`。压力有效期由调用者明确给出，不是声称还原原厂的超时参数。

## 离线工具

`piano-pen-offline.cpp` 只读外部 ini 与实际捕获文件，输出诊断 JSON；没有 FIFO、蓝牙配置或 uinput 操作。

在仓库根目录编译，输出到 `build/pen-core/`；交叉编译可指定 `CC`、`CXX`、`AR` 和工具链参数：

```sh
make -C linux/userspace/pen
build/pen-core/piano-pen-offline --ini ACTUAL.ini --config
build/pen-core/piano-pen-offline --ini ACTUAL.ini --pressure-max-age-ns N --events ACTUAL.tsv
```

事件文件每行使用同一 BOOTTIME 时基：`时间戳 report5|raw29 捕获文件`，或者 `时间戳 reset -`。未知字段输出 `null`。工具已经读取 BOE／CSOT 实际配置，并处理原厂采集的651个真实压力报告；没有制作 raw29 测试帧，也尚未处理真实笔矩阵。膜／悬浮专用分支、完整掌压干扰和最终桌面事件仍需要实机对照。

## 原厂协议进展

原厂压力 producer 是 `com.android.bluetooth` 的 `bt_main_thread`，通过原生 `libbluetooth_jni.so` 接收 Report5，将每个原始字节零扩展为 int32，调用 `setModeLongValue(0,1088,15,values)`。触控服务随后构造 common7／`0x440` 并直接交给算法；这条路径绕过内核 common queue。Android 的专用分支会跳过普通 UHID 转发，解释了同轮 hidraw 报告数为0；不能据此认定 Linux BlueZ 的路由也相同。

Android 轻捏已经捕获到真实 `02 6e`／`02 00` Report2，与 `KEY_F19` 的 DOWN／UP 和 `MSC_SCAN=0007006e` 对应。Report6 是独立姿态数据，不是压力或轻捏。Linux 已接收到相同轻捏 Report2，以及长度21的 Report6 姿态报告，说明蓝牙 HID 通道确实有数据。轻捏的桌面动作、姿态用途和笔端振动仍未集成，不能把报告已收到写成这些功能已可用。

## 原厂无线初始化

[`piano-pen-bluetooth.py`](piano-pen-bluetooth.py) 使用系统 BlueZ 完成原厂控制序列：读取笔信息、两轮分配 ID并设置参数／频率／电压、发送时间戳和真实磁吸状态，最后设置轻捏与屏幕状态。每一步等待实际回应，ID来自笔的回应，主机地址来自当前蓝牙适配器；不内嵌配对密钥或设备地址。

无线参数保存在 [`p81c-radio.json`](../../bsp/common/usr/share/piano/pen/p81c-radio.json)，依据本机 `OS3.0.309.0.WPYCNXM` 的原厂服务实际发送结果整理，与解算用的 BOE／CSOT ini不同。这个原厂版本的运行时 `TIME_STAMP` 是1745566508，实际选择普通参数表；不能把屏幕144Hz直接当作笔的频率参数。

完整初始化已在 Linux 收到全部回应。随后真实划线采集中，Report5仍为0，1728条屏幕记录仍全部为type3；因此初始化已经实现，但还不能绘画。下一步需要核对原厂用笔时的显示／触控扫描协调。工具不读THP流、不改变显示模式、不生成输入设备，也未作为新后台服务启用。

已安装磁吸候选和活动状态接口的设备，可显式运行：

```sh
sudo /usr/lib/piano/pen-bluetooth --address YOUR_PEN_ADDRESS \
  --profile /usr/share/piano/pen/p81c-radio.json \
  --stationary-device /proc/nvt_thp_pen_stationary
```

运行需要 Python GI与BlueZ，并且笔已经连接。缺少真实磁吸开关时会报错，不假定笔已取下。临时对照期间使用候选模块；默认发布内核尚未包含这两个接口。

## 构建与校准打包入口

`tools/build_release_helpers.py` 与 `tools/build_piano_runtime_helpers.py` 已共用 `build_pen_core()`，从源码副本调用这里的 Makefile。默认 native runtime bundle 包含 `/usr/bin/piano-pen-offline`、`/usr/lib/piano/pen-bluetooth`，以及 Apache-2.0 许可和来源说明。编译清单记录 C／C++ 源码、头文件、Makefile、实际工具链和目标静态 C++ 库；离线工具是 ARM64 静态程序，构建不需要原厂 ini。

`libpiano-pen-core.a` 留在构建目录，供以后链接到现有触控进程，不安装进系统。当前触控服务的默认行为没有改变，没有新增 systemd unit，也没有自动开启绘画输入。rootfs 通过现有 `runtime.stage()` 安装工具并保留来源记录。

原生 ELF 不能放进现有 `Architecture: all` 配置包。`tools/package_bsp.py` 当前只接受配置文件；原生程序放在 native runtime 层。ini 本身是架构无关数据，但需要作为明确的原厂输入单独登记，安装到 `/usr/share/piano/pen/calibration/`。构建应显式接收含 BOE／CSOT 两份实际 ini 的输入目录，记录 ROM 版本、来源及文件摘要，不能默认读取某台开发电脑的 `private/` 路径。当前公开 `piano-firmware` 输入有触控固件 bin，但没有这两份 ini；本地和 CI 必须使用同一份明确的校准输入，不能各自从隐藏目录补文件。校准数据的自动提取与打包尚未接入；缺失校准时不会自动使用另一面板的配置。

## 保留的原厂算法 worker

`prepare_alg_gnu.py` 与 `piano-pen-worker.c` 仍用于对照调用者提供的固定原厂 ALG，代码与原库指令不混在一起。其配置／初始化已经隔离执行成功。`--decode29` 重建 HAL 帧前缀；`--prepare29` 另需真实1032字节 common7／`0x440`，调用压力与矩阵准备接口。它不输出坐标，也不能把缺失 SC 状态的前缀直接交给完整 `parse_data_package` 或 slot7。

主实现路线是上述独立开源核心，不需要分发原厂 ALG/HAL。完整字段、来源与剩余硬件问题见 [手写笔协议开发记录](../../../docs/devel/piano-pen-protocol.md)。
