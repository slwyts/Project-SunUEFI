# Piano 手写笔核心

这里保存 Piano 的开源笔解码核心、触控进程内的笔输入接线、离线工具，以及保留的原厂算法诊断 worker。笔和手指共用现有 THP 数据读取进程，不增加第二个触控流读取者。

**标准笔输入已能绘画，位置正确，线宽随压力变化。** 当前结果来自本机 120 Hz 冷启动环境，Linux 已收到真实 Report5 压力和屏幕 type29 矩阵。144 Hz 下的笔输入尚未确认；轻捏按键、重连后的反馈参数和画板笔光标已写入默认源码；新版本仍需实机确认，应用的具体动作不在硬件解码层硬编码。

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

事件文件每行使用同一 BOOTTIME 时基：`时间戳 report5|raw29 捕获文件`，或者 `时间戳 reset -`。未知字段输出 `null`。工具已经读取 BOE／CSOT 实际配置、原厂651个真实压力报告，以及 Linux 的实际 type29／Report5 序列。后者得到1177帧有效坐标与倾角，其中896帧同时具有有效压力；没有制作 raw29 测试帧。膜／悬浮专用分支、完整掌压干扰与边缘体验仍需要实机对照。

## 同一触控进程中的笔输入

`piano-pen-owner.c/.h` 将只读蓝牙 HID 压力、已由触控进程读取的 type29、同源 BOOTTIME 时间戳与标准 tablet-tool 输入连接起来。只有显式提供实际校准和压力有效期时才启用笔路径；没有笔参数时保留原有手指行为。正常 runtime 已编入该功能，不需要额外编译另一份触控程序。

```sh
piano-touch-view input --seconds 0 \
  --pen-ini /path/to/ACTUAL.ini --pen-pressure-max-age-ms 100 --pen-input
```

100ms是调用者选择的有效期，不能称为原厂参数。可加 `--pen-json` 输出诊断；未知压力不伪造为0，退出、流中断和 epoch改变会释放工具与按键状态。输入保持真实压力0..16383，横向坐标由 portrait输出转换为 `X=portraitY`、`Y=213599-portraitX`，倾角同步转换。已有实机确认的位置与线宽变化来自这条标准输入路径。

该路径不设置笔扫描或显示模式。启动时的实际校准选择与无线／磁吸状态仍由现有硬件启动逻辑协调，不能仅凭程序编译成功就忽略这些条件。

## 原厂协议进展

原厂压力 producer 是 `com.android.bluetooth` 的 `bt_main_thread`，通过原生 `libbluetooth_jni.so` 接收 Report5，将每个原始字节零扩展为 int32，调用 `setModeLongValue(0,1088,15,values)`。触控服务随后构造 common7／`0x440` 并直接交给算法；这条路径绕过内核 common queue。Android 的专用分支会跳过普通 UHID 转发，解释了同轮 hidraw 报告数为0；不能据此认定 Linux BlueZ 的路由也相同。

Android 轻捏已经捕获到真实 `02 6e`／`02 00` Report2，与 `KEY_F19` 的 DOWN／UP 和 `MSC_SCAN=0007006e` 对应。Report6 是独立姿态数据，不是压力或轻捏。Linux 已接收到相同轻捏 Report2，以及长度21的 Report6 姿态报告，说明蓝牙 HID 通道确实有数据。默认输入 owner 只将真实 `02 6e`／`02 00` 转成 `BTN_STYLUS`，与真实工具接近状态一起上报；Report6 和未知笔身滑动字段不会伪造成滚轮。对应 HID 补丁去掉同一个 F19 的键盘重复输出。桌面动作由应用的标准笔按钮接口处理，具体动作与笔端物理反馈仍需实机确认。

## 原厂无线初始化

[`piano-pen-bluetooth.py`](piano-pen-bluetooth.py) 使用系统 BlueZ 完成原厂控制序列：读取笔信息、两轮分配 ID并设置参数／频率／电压、发送时间戳和真实磁吸状态，最后设置轻捏与屏幕状态。每一步等待实际回应，ID来自笔的回应，主机地址来自当前蓝牙适配器；不内嵌配对密钥或设备地址。

无线参数保存在 [`p81c-radio.json`](../../bsp/common/usr/share/piano/pen/p81c-radio.json)，依据本机 `OS3.0.309.0.WPYCNXM` 的原厂服务实际发送结果整理，与解算用的 BOE／CSOT ini不同。这个原厂版本的运行时 `TIME_STAMP` 是1745566508，实际选择普通参数表；不能把屏幕144Hz直接当作笔的频率参数。

完整初始化已在 Linux 收到全部回应。此前144Hz环境仍没有笔矩阵和压力；随后120Hz冷启动环境已经取得真实 type29与Report5，并完成标准笔输入绘画。显示／触控扫描协调仍需继续对照，不能据此断言144Hz硬件禁止笔输入。无线初始化工具不读取THP流、不改变显示模式、不生成输入设备，也不作为新的后台服务运行。

已安装磁吸候选和活动状态接口的设备，可显式运行：

```sh
sudo /usr/lib/piano/pen-bluetooth --address YOUR_PEN_ADDRESS \
  --profile /usr/share/piano/pen/p81c-radio.json \
  --stationary-device /proc/nvt_thp_pen_stationary
```

运行需要 Python GI与BlueZ，并且笔已经连接。磁吸和静止接口为可选：缺少真实磁吸开关时，不发送猜测的磁吸状态，但仍初始化笔的无线与轻捏反馈参数。临时对照期间使用候选模块；默认发布内核尚未包含这两个接口。

## HID 报告与桌面行为

P81C 的真实 137 字节 HID Report Map 将私有 Report5 数据放在键盘 Usage Page 的 `0x57`，Linux 通用 HID 会将它映射为 `KEY_KPPLUS`。压力包的四个 16 位字段和六个 8 位字段不是十个加号按键。默认内核的 [`0021` 补丁](../../../patches/linux/7.2.9/0021-hid-xiaomi-p81c-pressure-not-keyboard.patch) 扩展现有 `hid-xiaomi`，按真实 Bluetooth VID/PID、Report ID 和字段布局跳过这条错误键盘映射，保留原始 hidraw 数据给压力 owner。它不替换整份描述符，不全局屏蔽 `+`，也不影响键盘保护套。

真实轻捏从 Report2 接入同一个 tablet-tool 节点的 `BTN_STYLUS`，笔在有效范围内时才发给应用；抬出范围、断连、流中断时释放按钮。内核不再同时发送相同 F19 全局按键。普通应用可把它用作侧键或上下文菜单；绘图应用可绑定工具栏或工具切换。笔身上下滑尚无已确认的原始字段，暂不生成滚轮或笔刷大小快捷键。

`piano-touch-input` 在现有触控服务中启动 `pen-bluetooth --follow` 伴随进程。它只走 BlueZ，不读取 THP、也不生成键鼠输入；真实连接和 `ServicesResolved` 后完成初始化，断连后等待下一次连接重新发送反馈参数。磁吸状态只在真实开关存在时转发；没有磁吸／静止接口不阻塞已知的 BLE 功能。它支持当前笔返回的设备版本，不把 Modalias 硬锁到旧 `d0001`。`DA 01 01` 说明反馈设置被接收，物理震动仍需要实际轻捏确认。

笔尖位置、压力和悬停仍通过标准 tablet-tool 接口输出。光标外观由桌面和应用决定，不能靠把笔伪装成触摸屏解决。`piano-pen-canvas` 在自己的画布内对真实笔事件使用隐藏光标，鼠标／触控板事件恢复普通光标，画布之外不改桌面全局设置。其他应用的笔光标遵循各自设置。

## 构建与校准打包入口

`tools/build_release_helpers.py` 与 `tools/build_piano_runtime_helpers.py` 已共用 `build_pen_core()`，从源码副本调用这里的 Makefile。默认 native runtime bundle 包含 `/usr/bin/piano-pen-offline`、`/usr/bin/piano-pen-canvas`、`/usr/lib/piano/pen-bluetooth`，以及 Apache-2.0 许可和来源说明。编译清单记录 C／C++ 源码、头文件、Makefile、实际工具链和目标静态 C++ 库；离线工具是 ARM64 静态程序，构建不需要原厂 ini。

`libpiano-pen-core.a` 包含 C 编译的 owner／frame 与 C++ 编译的 core／decoder，已经静态链接到正常构建的 `piano-touch-view`，库文件本身不安装进系统。触控源码和入口包装保留 C ABI，最终链接使用同一目标 sysroot 的 C++ 链接器。rootfs 通过现有 `runtime.stage()` 安装程序并保留补丁、owner 和核心源码的来源记录。

原生 ELF 不能放进现有 `Architecture: all` 配置包。`tools/package_bsp.py` 当前只接受配置文件；原生程序放在 native runtime 层。ini 本身是架构无关数据，但需要作为明确的原厂输入单独登记，安装到 `/usr/share/piano/pen/calibration/`。构建应显式接收含 BOE／CSOT 两份实际 ini 的输入目录，记录 ROM 版本、来源及文件摘要，不能默认读取某台开发电脑的 `private/` 路径。当前公开 `piano-firmware` 输入有触控固件 bin，但没有这两份 ini；本地和 CI 必须使用同一份明确的校准输入，不能各自从隐藏目录补文件。校准数据的自动提取与打包尚未接入；缺失校准时不会自动使用另一面板的配置。

## 保留的原厂算法 worker

`prepare_alg_gnu.py` 与 `piano-pen-worker.c` 仍用于对照调用者提供的固定原厂 ALG，代码与原库指令不混在一起。其配置／初始化已经隔离执行成功。`--decode29` 重建 HAL 帧前缀；`--prepare29` 另需真实1032字节 common7／`0x440`，调用压力与矩阵准备接口。它不输出坐标，也不能把缺失 SC 状态的前缀直接交给完整 `parse_data_package` 或 slot7。

主实现路线是上述独立开源核心，不需要分发原厂 ALG/HAL。完整字段、来源与剩余硬件问题见 [手写笔协议开发记录](../../../docs/devel/piano-pen-protocol.md)。
