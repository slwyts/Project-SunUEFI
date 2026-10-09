# P81C 手写笔固件与原厂升级协议

P81C 是 Xiaomi Focus Pen Pro 的笔端固件。它运行在笔内的微控制器上，与平板的 Novatek 触控固件、Linux 内核和 Android 蓝牙服务分别属于不同层次。保存这份固件可以帮助分析笔的压力、轻捏、振动、蓝牙和充电行为；把它放进 Linux 的 `/lib/firmware` 不会自动改善这些功能。

本文记录 `P81C_0.0.32` 和同机原厂 Android 的实现。没有向笔发送升级、擦除或重启命令，也没有修改原厂升级过程。原始 ZIP、IMG 和校验信息保存在 [`vendor/piano-pen/p81c/0.0.32/`](../../vendor/piano-pen/p81c/0.0.32/)。笔的坐标与压力处理见 [Piano 手写笔协议](piano-pen-protocol.md)。

## 样本与分析来源

| 来源 | 用途 |
| --- | --- |
| Android 缓存中的 `20Kf_XW_P81C_Pen_FW_User_V0.0.32.zip` | 原厂下载包，MD5 为 `25fc9fc49b0b333ef5c28852d237bb97` |
| ZIP 内的 `XW_P81C_Pen_FW_User_V0.0.32.img` | 笔端升级镜像，710,312 字节 |
| 同机 `BluetoothExtension.apk` | 原厂下载、型号选择、GATT 分片与日常控制实现；SHA-256 为 `3b1cae2a9f6226701584982d33a5b46065d2a5cbbf3d63b44e3d0fade337f3bf` |
| `StylusOtaService` 实际日志 | 远端版本为 `0.0.32`，ZIP 校验通过并完成解包 |
| Renesas DA1469x 官方 SDK 文档 | 对照 SUOTA 服务、镜像头和启动流程，不能代替同机证据 |

APK 反编译输出位于本地 `private/analysis/piano-pen-producer-20261009/bluetooth-extension-decompiled/sources/`。下面的行号均对应这份固定 APK 的输出。混淆类名只作为定位标记，不应当成稳定 API；重新反编译其他 ROM 时应按方法、UUID 和调用关系查找。

## 型号选择与下载

P81C 的 PnP ID 为 VID `0x0022`、PID `0x5081`。`oobhelper/a.java:116` 从 Device Information 的 PnP ID 特征 `2A50` 读取 VID、PID，`oobhelper/h.java:126` 的升级分支明确将 PID 20609（`0x5081`）交给 `MiuiMippOtaManager`，即混淆类 `p085y0.h`。该类读取 `.img`，使用 SUOTA 服务。

同一 APK 还有 `MiuiMippOtaManagerV2`（`p085y0.l`），使用 `FED0/FED1/FED2` 和 `A5` 包头，其文件选择器读取 `.bin`。这是其他 PID 的路径。**不能因为它名字带 V2，就把这套协议用于 P81C。** APK 内另一个 `8EC90001` 控制特征的 DFU 分支也不能混用。

下载由 `com/android/bluetooth/ble/app/mipencil/ota/StylusOtaService.java` 完成：

1. 当前版本来自 Device Information 的 Firmware Revision `2A28`，解析点为 `oobhelper/a.java:114`。
2. `p076v0/c.java:348` 将产品写为 `p81c`、设备写为 `p81c.pen`；`:421` 使用模块 `p81c.pen.full` 请求更新元数据。
3. `StylusOtaService.java:41` 的缓存目录是 `/data/user_de/0/com.xiaomi.bluetooth/files/`。回调 `mipencil/ota/a.java:30` 计算 ZIP 的 MD5，只将十六进制结果前十位与服务器的 `md5` 比较，然后解包。
4. 解包目录通过 IPC 返回给升级管理器。该下载校验与笔端接收完成后的检查是不同阶段，不能把 ZIP 下载成功当成升级成功。

公开下载源为 [小米 OTA CDN 上的原厂 ZIP](https://cdn.cnbj1.fds.api.mi-img.com/ota-packages/20Kf_XW_P81C_Pen_FW_User_V0.0.32.zip)。研究记录不保存账户标识、设备地址、令牌或蓝牙配对密钥。

## P81C 的 SUOTA 端点

以下 UUID 全部来自 `p085y0/h.java:106` 的常量表，并由它的实际读写方法和 `oobhelper/a.java` 回调确认用途。

| 用途 | UUID | 数据 |
| --- | --- | --- |
| SUOTA 服务 | `0000fef5-0000-1000-8000-00805f9b34fb` | GATT 服务 |
| `MEM_DEV` | `8082caa8-41a6-4021-91c6-56f9b954cc34` | 4 字节，小端整数；启动传输、结束、重启 |
| `GPIO_MAP` | `724249f0-5ec3-4b5f-8804-42345af08651` | 4 字节，小端整数 |
| `PATCH_LEN` | `9d84b9a3-000c-49d8-9183-855b673fda31` | 2 字节，小端块长度 |
| `PATCH_DATA` | `457871e8-d516-4ca1-9116-57d0b17b9cb2` | 升级文件数据，Write Without Response |
| `STATUS` | `5f78df94-798c-46f5-990a-b3eb6a065c88` | 单字节通知，控制传输进度 |
| `PATCH_DATA_CHAR_SIZE` | `42c3dfdd-77be-4d9c-8454-8f875267fb3b` | 2 字节，小端特征容量 |
| `STATUS` 的 CCCD | `00002902-0000-1000-8000-00805f9b34fb` | 写 `01 00` 开启通知 |

原厂客户端采用 GATT 分块传输。没有证据表明这次 P81C 升级使用 L2CAP CoC，不能直接照搬其他 Renesas SDK 的可选传输路径。

## 分片、流控与结束

`p085y0/h.java:438` 将完整 IMG 读入数组，再在末尾追加一个字节：原始 IMG 全部字节的异或值。函数 `:141` 虽然把日志标签叫作 `crc`，实现实际上是 **XOR-8**，不是 CRC32。该样本的异或值为 `0x7C`，空中数据总长度是 **710,313 字节**。它与 IMG 头中的正文 CRC32 是两层独立校验。

`PATCH_DATA_CHAR_SIZE` 读取完成后，客户端请求 `capacity + 3` 的 ATT MTU；收到 MTU 回调后，实际分片长度为 `min(capacity, MTU - 3)`，见 `p085y0/h.java:333`。起始逻辑把八个分片组成一个块，最后一块按实际余量缩短。数据特征写完成的回调只驱动块内下一片，笔的 `STATUS` 通知才驱动下一块；两种回调都需要记录。

| 阶段 | 原厂行为 | 源码位置 |
| --- | --- | --- |
| 准备 | 设置高连接优先级、启用 `STATUS` 通知 | `p085y0/h.java:272`、`:245` |
| 开始 | 向 `MEM_DEV` 写 `0x13000000`，线上的字节为 `00 00 00 13` | `p085y0/h.java:525` |
| 初始握手 | 等待 `MEM_DEV` 写回调和 `STATUS=0x10` 两个条件，再写 `GPIO_MAP=0x05060500` | `p085y0/h.java:290`、`:518`；`oobhelper/a.java:50`、`:209` |
| 块长度 | 向 `PATCH_LEN` 写当前块长；末块缩短 | `p085y0/h.java:507` |
| 数据 | 分片写入 `PATCH_DATA`；块内由写回调推进，块间等待 `STATUS=0x02` | `p085y0/h.java:361`；`oobhelper/a.java:64`、`:227` |
| 结束 | 向 `MEM_DEV` 写 `0xFE000000`，即 `00 00 00 FE`，等待后续通知 | `p085y0/h.java:418` |
| 完成 | 恢复均衡连接优先级、关闭输入流、向 Android 界面报完成 | `p085y0/h.java:261`；`MiuiBleOobHelperService.java:3411` |
| 重启入口 | 可向 `MEM_DEV` 写 `0xFD000000`，即 `00 00 00 FD` | `p085y0/h.java:426` |

`oobhelper/a.java` 明确只把 `0x10` 转为握手步骤 3，把 `0x02` 转为传输步骤 5。其他状态未在该回调里详细解码。后续 Linux 工具应保留未知状态原值，不能将任何通知都当作成功。

原厂 `H3()` 完成路径还会根据已发现的服务决定是否排队重启笔。存在 `sendRebootSignal()` 并不代表当前这次升级已经实际发出过重启；最后仍应重新读取 `2A28`，确认笔的运行版本。

这些端点及 `0x13`／`0xFE`／`0xFD` 的用途也与 [Renesas DA1469x 的 SUOTA 服务说明](https://lpccs-docs.renesas.com/um-b-092-da1469x_software_platform_reference/User_guides/User_guides.html#suota-service-description) 对应。该文档说明，块长用于流控，整图发送结束后还要检查镜像，下一次重启的引导程序负责最终验证与启用。P81C 是否启用了该 SDK 的所有安全选项，必须另看笔的实际配置。

## 镜像头与笔内实现

本样本采用 `Qq` 镜像头，IMG 前 `0x400` 字节为头部，正文长 `709,288` 字节。正文 CRC32 为 `0xDCD50A1C`，实际对 `IMG[0x400:]` 计算得到相同结果。头中版本是 `P81C_0.0.32`，时间字段对应 `2025-12-16 10:00 UTC`，向量表偏移为 `0x400`。

头部、Cortex-M 向量表、Dialog/Renesas BLE 字符串和寄存器访问共同指向 **DA1469x 家族**。目前不把日志中的 `DA14697` 名称当成实际芯片料号的最终证明，也不把样本中的所有驱动都当成这支笔的物料清单。镜像头形式和启动流程可以对照 [Renesas DA1469x 镜像与引导说明](https://lpccs-docs.renesas.com/um-b-092-da1469x_software_platform_reference/User_guides/User_guides.html#booting)。

已找到的字符串与 Thumb 代码引用提供了以下分析入口。这里的“文件偏移”是 IMG 中的位置，“代码地址”按去掉 `0x400` 头部后的映射计算；没有符号表时仍需逐函数确认参数与调用者。

| 线索 | IMG 文件偏移 | 引用代码地址 | 对 Linux 调试的意义 |
| --- | --- | --- | --- |
| `AW86224` 振动驱动日志 | `0x9A498` | `0x34980` | 追踪电机初始化、增益和实际播放，与 FE11 的振动请求对应 |
| `CDV2624` 驱动日志 | `0x99B50` | `0x32898` | 同镜像存在另一种振动驱动；需确认运行时选择，不能断言两者同时工作 |
| Touch double-tap | `0xA5058` | `0x3E8CC` | 区分笔身触摸检测、双击配置和发送给主机的按键 |
| MIPP haptic gain | `0xA5CA4` | `0x41C44` | 轻捏反馈强度与笔内波形／增益的后续入口 |
| 默认 MIPP 60 Hz | `0xA4458` | `0x3AD1C` | 表示笔内默认参数，不能直接解释平板 144 Hz 下为何收不到坐标 |
| 压力处理库 | `0xA4350` | `0x3AD2C` | 与蓝牙 Report 5 的原始压力和校准路径对照 |

## 日常控制与升级协议要分开

日常控制使用 **FE10 服务、FE11 写入、FE12 通知**，后缀都是 `aa6c-462a-964a-7f2ed5b3e512`。命令通常是 `opcode, payload_length, payload`。下表来自同机 `MiuiBleOobHelperService` 的构包和回复解析，而非按 Renesas SDK 猜测。

| 请求 | 含义 | 回复 | 固定 APK 的定位点 |
| --- | --- | --- | --- |
| `01 00` | 查询笔功能位 | `81 02 feature0 feature1` | `MiuiBleOobHelperService.java:1074` |
| `02 00` | 查询各芯片工作状态 | `82 02 state0 state1` | 同文件 `:1191` |
| `51 01 state` | 告知磁吸状态；附着 0、分离 1 | `D1 00` | 同文件 `:3273`、`:1383` |
| `52 00` | 查询活动／静止状态 | `D2 01 state` | 同文件 `:1420` |
| `5A 01 level` | 设置轻捏反馈强度，级别 0–5 | `DA 01 status`，1 成功、0 失败 | 同文件 `:2613`、`:1580` |
| `5C 04 down_hi down_lo up_hi up_lo` | 设置轻捏按下、松开的两个阈值；大端 u16。原厂构包限制 down 为 1–900、up 为 0–900，单位尚未确认 | `DC 01 status` | 同文件 `:2661`、`:1482` |
| `5E 02 type value` | 振动控制类型与控制值，不是轻捏事件 | `DE 00` | 同文件 `:2894`、`:1522` |
| `5F 01 enabled` | 开关双击 | `DF 01 status` | 同文件 `:637`、`:1502` |
| `59 02 pen_type level` | 设置书写反馈，类型与强度分别编码 | `D9 01 status` | 同文件 `:2923`、`:1562` |
| `5B 01 enabled` | 开关加速度计／陀螺仪通道 | `DB 01 status` | 同文件 `:644`、`:1547` |
| `61 01 enabled` | 通知屏幕状态 | `E1 00` | 同文件 `:654`、`:1603` |
| `62 01 state` | 通知报点／抬起移动状态 | `E2 00` | 同文件 `:656`、`:1608` |

`feature0.bit0` 表示无线充电能力；`feature1.bit0` 表示 Haptic，`feature1.bit1` 表示高频压力能力。`state0.bit0` 是充电 IC、`.bit2` 是无线充 IC、`.bit5` 是压力 IC、`.bit6` 是触摸 IC 的工作状态。这些是笔回复的数据，不能用配置文件伪造为支持。

**`DA 01 01` 只说明强度设置被接受，不是一次真实轻捏，也不是电机已经振动的证明。** 原厂的 `oobhelper/h.java:40` 接收 IPC 消息 3006 后，会把 Android 用户设置中的双击、反馈强度和两个轻捏阈值排进命令队列。笔固件升级或重新连接之后，Linux 也应重新初始化这些日常参数，再分别观察实际轻捏输入和电机输出。

现有 [`piano-pen-bluetooth.py`](../../linux/userspace/pen/piano-pen-bluetooth.py) 已做无线参数、双击、轻捏反馈和阈值初始化。没有必要为设置反馈强度刷写笔固件。若用户报告轻捏不振动，首先检查它是否完成初始化、收到哪一条 FE12 回复以及实际运行版本，再对照原厂同样的操作。`62` 不能用来冒充“开启压力”；其开关影响按键／报点行为时，必须与 HID 输入路径一起确认。

固件升级还可能改变 HID 描述符。原厂 `oobhelper/i.java:837` 读取广播里的 HID 描述符版本，与本地常量 3 比较；版本较新时清除 `persist.bluetooth.spechid`，不再使用本地旧描述符。Linux 同样应以笔当前返回的 Report Map 为准。如果升级后笔、触摸或键盘出现多余按键，应该先确定事件来自哪个真实输入设备，再核对 Report ID、长度、字段和描述符，不能用全局屏蔽 `+` 键掩盖协议解析问题。

## 各层分别负责什么

| 功能或问题 | 笔端固件 | 平板触控／Linux 驱动 | 主机用户空间 |
| --- | --- | --- | --- |
| 笔尖坐标和倾角 | 发射笔信号 | 接收 Novatek type 29 原始矩阵，解算并输出标准输入事件 | libinput／绘图程序消费输入 |
| 压力 | 笔尖压力 IC，蓝牙 Report 5 | 与坐标时序融合，释放过期压力 | 应用处理压力曲线 |
| 轻捏、双击 | 笔身检测及 HID／控制报告 | 按 HID 描述符解释实际报告 | 绑定桌面动作；通过 FE11 配置反馈 |
| 震动 | 电机驱动和波形／增益执行 | 不应虚拟成平板电机 | 使用真实笔控制协议，确认物理效果 |
| 蓝牙唤醒、重连 | 广播、连接与休眠策略 | 蓝牙控制器及标准 BlueZ/HID 支持 | 跟随真实磁吸／屏幕状态，不用固定地址或删除配对来强连 |
| 磁吸无线充电 | 笔端接收、温度和电量状态 | 平板端充电发射器、霍尔检测及其电源驱动 | 通过标准电源／电池接口展示 |
| 144 Hz 下无笔坐标 | 有可研究的发射时序 | 面板刷新与触控扫描／笔同步是主要对照点 | 正确转发原厂配置，不把桌面刷新率直接当笔扫描率 |

笔内存在“默认 60 Hz”日志，不足以证明它只支持 60 Hz，也不足以证明修改这一个常数能解决 144 Hz 问题。同机 Android 的工作链需要一起对照：实际面板模式、笔协议时序、触控扫描模式、FE11 配置和 Report 5／type 29 是否到达。只改笔固件可能既不改变平板扫描，又引入升级风险。

下一步有价值的调试是：保持原厂固件不变，记录新版本的 `2A28`、FE11 请求／FE12 回复及真实轻捏、压力输入；再把必要初始化和输入解析写入现有 BSP。确实定位到笔端算法或协议缺陷后，才研究修改笔固件，而不是先生成一个 Linux OTA 写入器。
