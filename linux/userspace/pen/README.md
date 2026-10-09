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

## 构建与校准打包入口

现有 `tools/build_release_helpers.py` 与 `tools/build_piano_runtime_helpers.py` 编译单文件 C runtime；这个多文件 C++ 核心尚未接入默认构建。独立 `Makefile` 已能生成库和离线工具；下一步应由两者共用一处 pen 编译函数，调用这一构建入口：C 编译现有 frame 校验器，C++20 编译 core/decoder/offline，再生成供同一 touch owner 链接的库和离线工具。不要再加 systemd unit。`runtime.stage()` 的文件列表与 manifest 一起更新，rootfs 继续从现有 runtime bundle 安装。

原生 ELF 不能放进现有 `Architecture: all` 配置包。`tools/package_bsp.py` 当前只接受配置文件；本地编译程序继续放在 native runtime 层。ini 本身是架构无关数据，但需要作为明确的原厂输入单独登记，安装到 `/usr/share/piano/pen/calibration/`。构建应显式接收含 BOE／CSOT 两份实际 ini 的输入目录，记录 ROM 版本、来源及文件摘要，不能默认读取某台开发电脑的 `private/` 路径。当前公开 `piano-firmware` 输入有触控固件 bin，但没有这两份 ini；本地和 CI 必须使用同一份明确的校准输入，不能各自从隐藏目录补文件。这些打包接线尚未实现。

## 保留的原厂算法 worker

`prepare_alg_gnu.py` 与 `piano-pen-worker.c` 仍用于对照调用者提供的固定原厂 ALG，代码与原库指令不混在一起。其配置／初始化已经隔离执行成功。`--decode29` 重建 HAL 帧前缀；`--prepare29` 另需真实1032字节 common7／`0x440`，调用压力与矩阵准备接口。它不输出坐标，也不能把缺失 SC 状态的前缀直接交给完整 `parse_data_package` 或 slot7。

主实现路线是上述独立开源核心，不需要分发原厂 ALG/HAL。完整字段、来源与剩余硬件问题见 [手写笔协议开发记录](../../../docs/devel/piano-pen-protocol.md)。
