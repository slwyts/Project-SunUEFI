# Piano 手写笔核心

这里保存 Piano 的开源笔解码核心、触控进程内的笔输入接线、离线工具，以及保留的原厂算法诊断 worker。笔和手指共用现有 THP 数据读取进程，不增加第二个触控流读取者。

**标准笔输入已能绘画，位置正确，线宽随压力变化。** 当前结果来自本机 120 Hz 冷启动环境，Linux 已收到 Report5 压力和屏幕 type29 矩阵。144 Hz 下的笔输入尚未确认；轻捏按键、重连后的反馈参数和画板笔光标已写入默认源码；新版本仍需实机确认。轻捏、双击笔身和笔身滑动见[笔身手势](#笔身手势)。

## 开源实现

`piano-pen-decoder.hpp/.cpp` 派生自 `ianchb/xiaomi-sheng-thp` 固定提交 `34046210932d654a4c0df0121ecc31c008f8148c`，保留 Apache-2.0 许可。`piano-pen-core.h/.cpp` 提供 C 接口，让现有的触控读取进程可以调用它。来源与修改范围见 [OPEN_CORE_ORIGIN.txt](OPEN_CORE_ORIGIN.txt)，完整许可见 [Apache-2.0.txt](Apache-2.0.txt)。

核心接收去掉 stream header 和 257 字节 transport 的原始 payload，复用 `piano-pen-frame.c` 的两层原始校验和、几何检查与四分包位图。只有四份主触摸数据齐全才标记完整；type17 不会改成 type29。矩阵经过轴分离、坐标计算、倾角校正与滤波后，返回带有效位的结果。未知压力、坐标或倾角不能补零上报。

校准从调用者提供的原厂 ini 读取，没有内嵌 Sheng 的位置表或尺寸。Piano 的 40/60 轴从两个 2400 点 mapping 抽取，pitch 按原厂整数公式计算。P81c 的 vendor3 选择默认 `stylus` 参数，不是 `stylus_3` 槽。输出声明为 portrait：X `0..213599`、Y `0..319999`、压力 `0..16383`、倾角 ±60；不额外交换坐标，也不除以 report factor10。

**Touch 校准必须按触控 LCDid／固件选择，不能按 Display 面板名称选择。** 当前设备触控 LCDid 为1，使用 BOE 触控固件及 BOE ini，即使显示链路标识为 CSOT。两份 ini 的位置表不同，必须分别保留。

`piano_pen_core_report5()` 接收15字节 BLE Report5。Report ID 后两个字节组成小端压力，不能用 raw29 metadata 构造替代报告。BLE 时间戳使用 `CLOCK_BOOTTIME`，与 NTP 驱动的 `ktime_get_boottime()` 同源；睡眠后不能混用 `CLOCK_MONOTONIC`。压力有效期由调用者明确给出，不是声称还原原厂的超时参数。

## 离线工具

`piano-pen-offline.cpp` 只读外部 ini 与捕获文件，输出诊断 JSON；没有 FIFO、蓝牙配置或 uinput 操作。

在仓库根目录编译，输出到 `build/pen-core/`；交叉编译可指定 `CC`、`CXX`、`AR` 和工具链参数：

```sh
make -C linux/userspace/pen
build/pen-core/piano-pen-offline --ini ACTUAL.ini --config
build/pen-core/piano-pen-offline --ini ACTUAL.ini --pressure-max-age-ns N --events ACTUAL.tsv
```

事件文件每行使用同一 BOOTTIME 时基：`时间戳 report5|report2|raw29 捕获文件`，或者 `时间戳 reset -`。`report2` 是两字节的笔身手势报告，输出其中的值和手势名称。未知字段输出 `null`。工具已经读取 BOE／CSOT 配置、原厂651个压力报告，以及 Linux 的 type29／Report5 序列。后者得到1177帧有效坐标与倾角，其中896帧同时具有有效压力；没有制作 raw29 测试帧。膜／悬浮专用分支、完整掌压干扰与边缘体验仍需要实机对照。

## 同一触控进程中的笔输入

`piano-pen-owner.c/.h` 将只读蓝牙 HID 压力、已由触控进程读取的 type29、同源 BOOTTIME 时间戳与标准 tablet-tool 输入连接起来。只有显式提供校准和压力有效期时才启用笔路径；没有笔参数时保留原有手指行为。正常 runtime 已编入该功能，不需要额外编译另一份触控程序。

```sh
piano-touch-view input --seconds 0 \
  --pen-ini /path/to/ACTUAL.ini --pen-pressure-max-age-ms 100 --pen-input
```

100ms是调用者选择的有效期，不能称为原厂参数。可加 `--pen-json` 输出诊断；未知压力不伪造为0，退出、流中断和 epoch改变会释放工具与按键状态。输入保持压力0..16383，横向坐标由 portrait输出转换为 `X=portraitY`、`Y=213599-portraitX`，倾角同步转换。已有实机确认的位置与线宽变化来自这条标准输入路径。

该路径不设置笔扫描或显示模式。启动时的校准选择与无线／磁吸状态仍由现有硬件启动逻辑协调，不能仅凭程序编译成功就忽略这些条件。

掌压识别在同一程序的手指解码中完成，见 [`piano-touch-view-palm-rejection.patch`](../../../tools/patches/piano-touch-view-palm-rejection.patch)。每个触点取峰值周围连通且不低于触摸阈值（默认 200）的格子，格子数达到 `PALM_AREA_CELLS`（70）的触点上报为 `MT_TOOL_PALM`，同一片手掌只占一个触点，抬起前不会变回手指，也不按下 `BTN_TOUCH`；libinput 不把它交给应用，手指变成手掌时取消已开始的触摸。70 取自原厂触控配置 `/odm/firmware/piano_nova_boe_thp_config.ini`（CSOT 版本取值相同）的 `palm_area_num_level2`。原厂按同样的 60×40 感应矩阵计数，每格约 16 mm²，70 格约 1140 mm²，相当于直径 38 mm 的圆。原厂同组还有 `palm_neg_area_num_level2=30`（负值格子数）和更大的 `palm_area_num_level1=110`；这里不统计负值格子，只用 70 格。按格子数判断，两指并拢时连成的细长区域仍是两根手指。触点大小按椭圆估算长短轴，以 `ABS_MT_TOUCH_MAJOR`／`ABS_MT_TOUCH_MINOR` 上报，单位是 13／mm（面板 239×163 mm）。type29 笔帧到来时仍按着的触点以手掌方式抬起，先落下的手不会在笔靠近时变成一次点击。BSP 的 `79-piano-pen-touch-group.rules` 把触屏和笔放进同一个 libinput 设备组，笔在感应范围内时，libinput 自带的触控仲裁忽略笔旁的新触摸。在平板上调整时，用 `evtest` 查看手指和手掌的 `ABS_MT_TOUCH_MAJOR`、`ABS_MT_TOUCH_MINOR` 与 `ABS_MT_TOOL_TYPE`，长短轴相乘再除以 3500 约等于格子数，据此调整补丁中的 `PALM_AREA_CELLS`。

## 原厂协议进展

原厂压力 producer 是 `com.android.bluetooth` 的 `bt_main_thread`，通过原生 `libbluetooth_jni.so` 接收 Report5，将每个原始字节零扩展为 int32，调用 `setModeLongValue(0,1088,15,values)`。触控服务随后构造 common7／`0x440` 并直接交给算法；这条路径绕过内核 common queue。Android 的专用分支会跳过普通 UHID 转发，解释了同轮 hidraw 报告数为0；不能据此认定 Linux BlueZ 的路由也相同。

Android 轻捏已经捕获到 `02 6e`／`02 00` Report2，与 `KEY_F19` 的 DOWN／UP 和 `MSC_SCAN=0007006e` 对应。Report6 是独立姿态数据，不是压力或轻捏。Linux 已接收到相同轻捏 Report2，以及长度21的 Report6 姿态报告，说明蓝牙 HID 通道确实有数据。Report2 同时承载轻捏、双击和笔身滑动，处理方式见[笔身手势](#笔身手势)。Report6 不转成任何输入。

## 原厂无线初始化

[`piano-pen-bluetooth.py`](piano-pen-bluetooth.py) 使用系统 BlueZ 完成原厂控制序列：读取笔信息、两轮分配 ID并设置参数／频率／电压、发送时间戳和磁吸状态，最后设置轻捏与屏幕状态。每一步等待回应，ID来自笔的回应，主机地址来自当前蓝牙适配器；不内嵌配对密钥或设备地址。

无线参数保存在 [`p81c-radio.json`](../../bsp/common/usr/share/piano/pen/p81c-radio.json)，依据本机 `OS3.0.309.0.WPYCNXM` 的原厂服务发送结果整理，与解算用的 BOE／CSOT ini不同。这个原厂版本的运行时 `TIME_STAMP` 是1745566508，选择普通参数表；不能把屏幕144Hz直接当作笔的频率参数。

跨系统使用后，笔可能已经忘记 Linux 保留的配对密钥。旧版 BlueZ
（SteamOS 的 5.79）可能只返回通用连接失败，程序同时监听标准内核蓝牙
管理接口的认证失败事件。只有失败地址与磁吸充电器识别的笔相符，才清除
该笔的旧配对并重新配对；不按普通超时清除配对，也不影响其他设备。
实机已完成认证失败后的自动重新配对与无线初始化，笔接受了三级轻捏震动设置。

完整初始化已在 Linux 收到全部回应。之前144Hz环境仍没有笔矩阵和压力；随后120Hz冷启动环境已经取得 type29与Report5，并完成标准笔输入绘画。显示／触控扫描协调仍需继续对照，不能据此断言144Hz硬件禁止笔输入。无线初始化工具不读取THP流、不改变显示模式、不生成输入设备，也不作为新的后台服务运行。

已安装磁吸草案和活动状态接口的设备，可显式运行：

```sh
sudo /usr/lib/piano/pen-bluetooth --address YOUR_PEN_ADDRESS \
  --profile /usr/share/piano/pen/p81c-radio.json \
  --stationary-device /proc/nvt_thp_pen_stationary
```

运行需要 Python GI与BlueZ，并且笔已经连接。磁吸和静止接口为可选：缺少磁吸开关时，不发送猜测的磁吸状态，但仍初始化笔的无线与轻捏反馈参数。临时对照期间使用草案模块；默认发布内核尚未包含这两个接口。

## HID 报告与桌面行为

P81C 的 137 字节 HID Report Map 将私有 Report5 数据放在键盘 Usage Page 的 `0x57`，Linux 通用 HID 会将它映射为 `KEY_KPPLUS`。压力包的四个 16 位字段和六个 8 位字段不是十个加号按键。默认内核的 [`0021` 补丁](../../../patches/linux/7.2.9/0021-hid-xiaomi-p81c-pressure-not-keyboard.patch) 扩展现有 `hid-xiaomi`，按 Bluetooth VID/PID、Report ID 和字段布局跳过这条错误键盘映射，保留原始 hidraw 数据给压力 owner。它不替换整份描述符，不全局屏蔽 `+`，也不影响键盘保护套。

`piano-touch-input` 在现有触控服务中启动 `pen-bluetooth --follow` 伴随进程。它只走 BlueZ，不读取 THP、也不生成键鼠输入；连接和 `ServicesResolved` 后完成初始化，断连后等待下一次连接重新发送反馈参数。磁吸状态只在开关存在时转发；没有磁吸／静止接口不阻塞已知的 BLE 功能。它支持当前笔返回的设备版本，不把 Modalias 硬锁到旧 `d0001`。`DA 01 01` 说明反馈设置被接收，物理震动仍需要轻捏确认。

断连时，伴随进程从 BlueZ 枚举已开启的适配器，尝试连接已配对、受信任的 P81C。SC96231 内核驱动从磁吸链路提供笔的准确地址后，程序优先使用该地址；BlueZ 报告认证失败时，只清除这支笔失效的配对并通过标准 Agent1／Device1 接口重新配对。配对代理不成为系统默认代理，不接受其他设备。原厂 Android 与 Linux 交替使用后，已完成自动删除旧配对、重新配对、恢复 GATT 控制服务和初始化。

扫描每次持续 30 秒，随后间隔 90 秒；磁吸状态变化提前重试，设备连接和服务出现由 D-Bus 事件立即处理。程序不会开启用户关闭的蓝牙，也不会终止其他程序的扫描。磁吸供电、地址和电量读取由内核 `sc96231` 与 MCA regulator 完成，不依赖用户空间 GPIO 或 I²C 命令。

压力 HID 设备通过 inotify 创建通知立即接入，保留一秒扫描作为通知不可用时的回退。初始化开始、无线参数完成、压力设备打开和第一份 Report5 均记录单调时钟，便于定位“悬停先出现、稍后才能点击”。命令回应时间仍取决于笔的连接参数与磁吸状态，不能据此承诺全部连接延迟已消除。

笔尖位置、压力和悬停仍通过标准 tablet-tool 接口输出。光标外观由桌面和应用决定，不能靠把笔伪装成触摸屏解决。`piano-pen-canvas` 在自己的画布内对笔事件使用隐藏光标，鼠标／触控板事件恢复普通光标，画布之外不改桌面全局设置。其他应用的笔光标遵循各自设置。

## 笔身手势

轻捏、双击笔身和笔身上下滑动由笔自己识别，都经 Report2 发出：两个字节 `02 value`，新值替换旧值，每个手势以 `02 00` 结束。笔固件只发送下表中的值，固件与原厂系统中的依据见[笔身手势协议](../../../docs/devel/hardware/pen-protocol.md#笔身手势)。

| Report2 | 手势 | Linux 输出 | 原厂行为 |
| --- | --- | --- | --- |
| `02 6e` … `02 00` | 轻捏，按住期间保持 | tablet-tool 节点：短按是笔尖位置的一次点击，按住 600 ms 以上是 `BTN_STYLUS` | 默认在笔的位置打开快捷面板 |
| `02 58`、`02 00` | 双击笔身 | tablet-tool 节点的 `BTN_STYLUS2` 按下、抬起 | 交给原厂创作、笔记等支持笔身手势的应用 |
| `02 61`、`02 00` | 上滑 | 笔键盘节点的 `KEY_PAGEUP` | 阅读与办公应用收到 Page Up，短视频应用切到上一条 |
| `02 5b`、`02 00` | 下滑 | 笔键盘节点的 `KEY_PAGEDOWN` | 阅读与办公应用收到 Page Down，短视频应用切到下一条 |

滑动由内核 `hid-xiaomi` 的 [`0025` 补丁](../../../patches/linux/7.3/0025-hid-xiaomi-p81c-body-gestures.patch)直接映射为 Page Up／Page Down，发给当前窗口，与键盘上的翻页键相同；它不依赖这里的程序，其他发行版使用同一内核也能得到。通用 HID 原本会把双击和滑动变成小键盘回车、`9` 和 `3`，补丁去掉了这些映射，也不再发送轻捏的 F19。

轻捏和双击由同一触控进程读取 hidraw，作为笔按钮发给 tablet-tool 节点，只在笔处于感应范围内时上报；抬出范围、断连、流中断时释放按钮。设置 `PIANO_PEN_SQUEEZE_ACTION=stylus` 时轻捏始终作为 `BTN_STYLUS`。不支持笔的应用通常把 `BTN_STYLUS` 当作中键、`BTN_STYLUS2` 当作右键；绘图应用可以把两个按钮绑定到橡皮擦、取色等工具。双击或滑动打断仍按住的轻捏时，轻捏结束但不算一次点击。

笔尖受压时笔不发送手势，停笔片刻后才会识别。双击和滑动的开关保存在笔里，`pen-bluetooth` 按 `p81c-radio.json` 的 `double_tap` 与 `slide` 在每次连接时打开它们；改为 `false` 后重启 `piano-touch.service` 即可在笔端关闭。原厂在双击和滑动时会让平板另发一次振动命令，Linux 不发送，因此这两个手势没有笔端振动。

双击与滑动的报告格式来自笔固件和原厂系统，尚无平板上的实测记录。在平板上检查时，`sudo libinput debug-events --show-keycodes` 应在笔的键盘设备上显示 `KEY_PAGEUP`／`KEY_PAGEDOWN`，在 `Piano Focus Pen Pro` 上显示 `BTN_STYLUS2`；`sudo evtest` 可分别查看两个设备。只看原始 Report2 的命令见协议页。

## 构建与校准打包入口

`tools/build_release_helpers.py` 与 `tools/build_piano_runtime_helpers.py` 已共用 `build_pen_core()`，从源码副本调用这里的 Makefile。默认 native runtime bundle 包含 `/usr/bin/piano-pen-offline`、`/usr/bin/piano-pen-canvas`、`/usr/lib/piano/pen-bluetooth`，以及 Apache-2.0 许可和来源说明。编译清单记录 C／C++ 源码、头文件、Makefile、工具链和目标静态 C++ 库；离线工具是 ARM64 静态程序，构建不需要原厂 ini。

`libpiano-pen-core.a` 包含 C 编译的 owner／frame 与 C++ 编译的 core／decoder，已经静态链接到正常构建的 `piano-touch-view`，库文件本身不安装进系统。触控源码和入口包装保留 C ABI，最终链接使用同一目标 sysroot 的 C++ 链接器。rootfs 通过现有 `runtime.stage()` 安装程序并保留补丁、owner 和核心源码的来源记录。

原生 ELF 不能放进现有 `Architecture: all` 配置包。`tools/package_bsp.py` 当前只接受配置文件；原生程序放在 native runtime 层。ini 本身是架构无关数据，但需要作为明确的原厂输入单独登记，安装到 `/usr/share/piano/pen/calibration/`。构建应显式接收含 BOE／CSOT 两份 ini 的输入目录，记录 ROM 版本、来源及文件摘要，不能默认读取某台开发电脑的 `内部记录目录 路径。当前公开 `piano-firmware` 输入有触控固件 bin，但没有这两份 ini；本地和 CI 必须使用同一份明确的校准输入，不能各自从隐藏目录补文件。校准数据的自动提取与打包尚未接入；缺失校准时不会自动使用另一面板的配置。

## 保留的原厂算法 worker

`prepare_alg_gnu.py` 与 `piano-pen-worker.c` 仍用于对照调用者提供的固定原厂 ALG，代码与原库指令不混在一起。其配置／初始化已经隔离执行成功。`--decode29` 重建 HAL 帧前缀；`--prepare29` 另需1032字节 common7／`0x440`，调用压力与矩阵准备接口。它不输出坐标，也不能把缺失 SC 状态的前缀直接交给完整 `parse_data_package` 或 slot7。

主实现路线是上述独立开源核心，不需要分发原厂 ALG/HAL。完整字段、来源与剩余硬件问题见 [手写笔协议开发记录](../../../docs/devel/hardware/pen-protocol.md)。
