# P81C 0.0.32 固件的二进制结构与调试入口

分析对象是 Android 原厂升级流程缓存的 `XW_P81C_Pen_FW_User_V0.0.32.img`，710,312 字节，SHA-256 `7d05be949d58e937164782844216ad34cb669bc8cd1619ff29c49eea62902606`。原 ZIP 内的单一 IMG 与缓存解压文件逐字节一致。本分析没有连接笔、发送 OTA 命令或修改固件。

## 容器不是 Android boot.img

它是 Dialog / Renesas SUOTA 的 `Qq` 单镜像。头部字段与实际校验结果如下。多字节数值按小端解释；`AA 22` / `AA 44` 是字节序列标记，不是把它们按小端整数名称重命名。

| IMG 偏移 | 长度 | 内容 |
| --- | --- | --- |
| `0x00` | 2 | `51 71`，ASCII `Qq` |
| `0x02` | 4 | payload 长度 `0xAD2A8` = 709,288 字节 |
| `0x06` | 4 | payload CRC32 `0xDCD50A1C`；对 `0x400..EOF` 计算 zlib CRC32 完全相同 |
| `0x0A` | 16 | ASCII `P81C_0.0.32` + NUL 填充 |
| `0x1A` | 4 | 时间戳 `0x69412DA0`，2025-12-16 10:00:00 UTC |
| `0x1E` | 4 | 向量表/应用 payload 偏移 `0x400` |
| `0x22` | 4 | 安全段标记 `AA 22`，后续长度 0 |
| `0x26` | 4 | 管理段标记 `AA 44`，后续长度 0 |
| `0x2A..0x3FF` | 982 | `FF` 填充 |

头中时间是固件打包日期，不是本次采集或升级时间。CRC32 只能证明保存的数据完整，不能充当签名验证。当前文件没有非空安全/管理段，payload 可直接读取与反汇编；这不能推导笔芯片的 OTP 安全配置，也不能推导升级流程允许任意改包。

## 主控与代码地址

可确认它包含 Arm Thumb / Cortex-M 代码。`Dialog BLE`、`DA14`、`charger_da14697` 字符串、`0x200xxxxx` RAM、`0x500xxxxx` 寄存器访问以及启动表结构，共同指向 Dialog / Renesas **DA1469x、Cortex-M33** SDK。具体 DA14691/95/97/99 料号尚不能仅由这个二进制确定：镜像同时带有多个硬件候选驱动。

[Renesas DA1469x 资料](https://www.renesas.com/en/document/dst/da1469x-datasheet?r=1606281)说明该系列的应用 CPU 是 Cortex-M33。[SDK 启动与内存布局](https://lpccs-docs.renesas.com/um-b-092-da1469x_software_platform_reference/User_guides/User_guides.html#booting)说明应用通过 QSPI remap 执行，启动代码按 copy/zero 表初始化 RAM。这解释了同一向量表中同时出现低地址 Flash 函数和 `0x200xxxxx` RAM 函数。

文件里没有完整 Product Header、QSPI 活动/升级槽地址或芯片 Boot ROM。导入反汇编器时应去掉 `0x400` 字节头，把 body 映射到 **应用 remapped VA 0**；不能把文件偏移直接当运行地址，也不能认为 VA 0 是芯片物理 QSPI 地址。

| 含义 | 文件偏移 | 运行地址/值 |
| --- | --- | --- |
| 向量表 | `0x400` | VA `0x00000000` |
| 初始栈 | `0x400` 的首个 word | `0x20045888` |
| Reset 向量 | `0x404` 的 word | `0x00000201`（最低位表示 Thumb） |
| Reset_Handler 实际指令 | `0x600` | VA `0x00000200` |
| NMI 向量 | `0x408` 的 word | `0x200007D9`，代码源文件偏移 `0xA8940` |
| HardFault 向量 | `0x40C` 的 word | `0x200007F1`，代码源文件偏移 `0xA8958` |

已对 Reset_Handler 的真实字节进行 Thumb/M-class 反汇编。VA `0x20A/0x20C` 从 literal pool 读取 copy 表边界 `0xA83B4..0xA83D8`，VA `0x212..0x244` 按三元组复制。VA `0x246/0x248` 读取 zero 表 `0xA83D8..0xA83E8` 并清零，随后调用 VA `0xA7C` 和 `0x420`。VA `0x290..0x2A7` 是 literal pool，不能继续当指令解释。

| ROM 源文件偏移 | 应用源 VA | RAM 目标 VA | 长度 |
| --- | --- | --- | --- |
| `0xA87E8` | `0xA83E8` | `0x20000680` | `0x3834` |
| `0xAC01C` | `0xABC1C` | `0x20045888` | `0x314` |
| `0xAC330` | `0xABF30` | `0x20068800` | `0x1378` |

zero 表清零 `0x20069B78` 长 `0x7B10`，以及 `0x20003EB8` 长 `0x417C8`。这些地址由镜像启动指令和表数据取得，工具没有写死 0.0.32 的 copy 表位置。

## 可用于 Linux 调试的功能线索

以下“存在”指镜像里有对应代码和日志，不代表已经证明当前笔安装了所有这些芯片。字符串引用列是保存该字符串 VA 的 literal/data word 的 IMG 偏移；只有注明的反汇编入口有局部控制流确认。

| 功能 | IMG 字符串偏移 | 字符串应用 VA | 指针引用 IMG 偏移 | 用途 |
| --- | --- | --- | --- | --- |
| AW86224 振动驱动 | `0x9A498` | `0x9A098` | `0x34980` | 与 CDV2624 区分实际振动芯片和增益路由 |
| CDV2624 振动驱动 | `0x99B50` | `0x99750` | `0x32898` | 同镜像内另一硬件候选 |
| 轻捏/触摸双击日志 | `0xA5058` | `0xA4C58` | `0x3E8CC` | 继续追 Touch 状态机、双击门限和允许条件 |
| MIPP 振动增益 | `0xA5CA4` | `0xA58A4` | `0x41C44` | 与 Android `5A 01 level` 命令对应 |
| 默认 MIPP 60 Hz 配置 | `0xA4458` | `0xA4058` | `0x3AD1C` | 与原厂平板给笔的扫描/刷新配置比较，不能把日志中的 60 Hz 当笔最高采样率 |
| 压力库初始化及版本 | `0xA4350` | `0xA3F50` | `0x3AD2C` | 查压力标定、零点和笔尖传感器路径 |
| BL7488D 扫描启用 | `0x8EFE8` | `0x8EBE8` | `0x286F4` | 捏笔触控候选控制器 |
| BL6486 扫描启用 | `0x98784` | `0x98384` | `0x2A9A8` | 另一触控候选 |

还可直接定位：`icm42670p` / `bmi325` 惯性传感器、`app_air_mouse` 空中鼠标状态机、`sidespin` 侧转行为、`f_psensor_cal` 压力标定、`app_bms` 充电状态和 `cps4520` 电源驱动。图像中的 `TouchPencil`（IMG `0x8EA19`）和 `DarwinPen`（IMG `0x981C9`）位于相关控制器数据区；其内容和架构尚未完成解析，不能当 M33 主控函数名。混合 Flash 范围含常量、字串与其他芯片数据，并不是整段连续可执行代码。

已找到一张 29 项命令分发表，IMG `0xA5BBC..0xA5CA3`。其条目形状是 `[id:u8, payload_len:u8, reserved:u16=0, handler:u32]`，命令 ID 单调递增，handler 均为 Thumb 指针。IMG **`0xA5C4C`** 的条目为 `5A 01 00 00 0D 18 04 00`，直接把 `0x5A`、1 字节 payload 与增益 handler `0x4180D` 对应起来；不再仅靠日志猜测。其他条目暂作为候选，不因表中存在就直接向笔发送。

振动增益有比字符串更进一步的局部代码证据：

- handler VA **`0x4180C`**，IMG **`0x41C0C`**，从第三参数指向的 payload 中读取第一个 byte。
- VA `0x4182C` 把 byte 放入 `r1`，`0x4182E` 设置 `r0=6`，`0x41830` 调用 VA **`0x4253C`**。
- VA `0x4253C` helper 构造内部消息，首字节为 5、第二字节为这个 payload 值，再调用 VA `0x23224`。这里的 5 是内部消息类型，不是增益上限。
- VA `0x41834..0x4183A` 打印对应 haptic gain 日志。这证明该路径确实处理增益值，但未完整追踪最终芯片写寄存器、量化曲线或钳位规则。Android 的 0..5 范围来自原厂客户端，不应由这段局部代码自行猜测。

Linux 下一步宜先通过原厂 GATT/输入日志确认笔的实际硬件候选与 MIPP 消息，再对相应函数做细化控制流分析。当前 Linux 压力/位置功能不要求把 OTA 镜像写入平板文件系统，也不应将这个固件当作运行时自动升级依赖。

## 复现分析

```sh
python3 tools/host/inspect_p81c_firmware.py \
  vendor/piano-pen/p81c/0.0.32/XW_P81C_Pen_FW_User_V0.0.32.img \
  --output build/p81c-inspection/manifest.json \
  --export-dir build/p81c-inspection/disassembler-input
```

工具同样接受单 IMG 的原 ZIP，只用 Python 标准库。它验证容器范围和 payload CRC32，读取向量表、识别已观察到的 SDK startup 指令模式、导出 RAM 初始化映射，并列出选定功能日志的偏移和指针引用。`body-remapped-00000000.bin` 可供 Ghidra/IDA/radare2 以 Cortex-M33 little-endian Thumb 导入；各 `ram_initialization_*.bin` 应分别映射到 manifest 中的 `runtime_va`，否则 NMI/HardFault 等 RAM 函数无法正确跳转。

本次输出包括 `binary-analysis.json`、`zip-analysis.json` 和 `selected-thumb-disassembly.txt`。导出只用于静态分析，工具没有设备连接、刷写、签名绕过或 OTA 升级能力。解出的函数用途以局部汇编和字符串为依据，还没有原始源码或完整符号恢复。
