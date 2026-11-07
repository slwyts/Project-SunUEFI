# Y700 四代 UEFI 触摸

Y700 四代（TB322FC，CSOT 3K 面板，联想拯救者 Y700 四代平板）在 UEFI 阶段用 GPIO 模拟 SPI 读取 Novatek NT36536 触摸控制器，发布 Absolute Pointer 协议供内置菜单使用。这套代码只覆盖 UEFI 菜单的单指输入；Windows 和 Linux 的触摸由各自系统的驱动负责，不复用这里的固件下载和报文读取。

## 构建依赖

本页列出的文件不能单独编译。`Y700Touch.c` 包含构建期生成的 `Y700TouchConfig.h`，后者由 `tools/y700/prepare_touch.py` 写入，内容来自 `vendor/y700/dtb/y700-live.dtb` 和 `vendor/y700/touch/novatek_ts_csot_3k_fw.bin`。同一个脚本还负责把 `Y700Touch.c` 加入 `ProductCore.inf` 的 `[Sources]`。Y700 平台包（DSC/FDF、`SUNUEFI_Y700` 开关、`PianoProductCore.c` 里 `Y700TouchStart`/`Y700TouchStop`/`Y700TouchReplay` 的调用）和这两个 vendor 文件尚未合并到本仓库；在它们就位前，检出本页文件不会得到可构建的固件。

`prepare_touch.py` 生成配置前会核对设备树里的 SPI 控制器 `0xA90000`、CS0、Mode 0、最高 10 MHz、GPIO 48/49/50/51 的引脚复用和 GPIO 162 的中断描述，并核对完整固件的 SHA256 `31382287ce3c5842f6df299589233459056043878deb1d27f7ef6b7f104ce271`（253952 字节）。任一处与本机采集不一致就停止构建，不生成配置。

## 硬件与协议来源

原厂 Android 的最终设备树指定 SPI 控制器 `0xA90000`。MISO、MOSI、CLK、CS 分别是 GPIO 48、49、50、51，中断是 GPIO 162。当前实现不申请主机 DMA（直接内存访问），不调用其他设备的 GENI/GPI 二进制接口，只驱动这四个 SPI 引脚和一个中断引脚。

驱动协议依据联想公开的 Novatek `NT36XXX_SPI` 源码；本机实际寄存器地址和版本差异取自同机原厂 `nvt_36xxx.ko` 的 ELF 符号和反汇编。公开源码里的旧 NT36523、NT36532 内存表与本机不一致，不能直接替换：本机事件缓冲区是 `0x130900`，不是旧表的 `0x125800`。

## 初始化

`Y700Touch.c` 在 `Y700TouchStart` 中按以下顺序初始化，任一步失败都会调用 `Y700TouchStop` 还原已经占用的资源：

1. 读取串行引擎状态，确认对应引擎空闲；保存四个 SPI 引脚的复用、输出和控制权状态，切换为 GPIO mode 0。
2. 配置 GPIO 162 的上升沿硬件锁存：保留 raw status 的捕获位，关闭 TLMM（Top Level Mode Multiplexer，高通 GPIO 控制块）summary 输出和 PDC 唤醒输出，不使能 CPU 中断。sun 的 `intr_ack_high` 默认为 false，因此向 status 写 0 清除，而不是写 1 清除。
3. 核对完整的芯片 trim（`15 0e 00 23 65 03`）与级联配置，读取固件信息头和 PID `0x6097`；有效的其他型号 PID 一律拒绝。
4. 按原厂流程把四段固件写入触摸芯片 SRAM，保留原厂每次片选 0x3c00 字节的分段边界，等待内部级联复制完成，读回两段信息头，再核对 GCM CRC 状态的 byte1 bit3、ILM/DLM 结果 CRC 和最终 PID。固定固件每次启动都重新加载，避免继承 Android 挂起后芯片的深度睡眠状态。这条路径不写触摸 flash。
5. 单独发送普通上报命令 `76 00` 并记录 ACK，ACK 不为 0 不阻止后续输入。启动后校准阶段可能尚未应答，把它当作硬条件会误停输入。

成功后发布 Absolute Pointer 协议、ConsoleIn 设备路径和一个 20 ms 周期定时器。定时器只检查上升沿锁存位，有新边沿才读 108 字节事件缓冲区；空闲时不读 SRAM，也不以瞬时 GPIO 高电平作为读包条件。

## 报文与菜单输入

触摸报文长 108 字节：前 65 字节的累加和必须为 0，之后是十个 6 字节槽。槽里 id 是首字节高 5 位（1..10），状态是低 3 位（1 或 2 为按下），X/Y 是 16 位大端坐标，最大 `19040 × 30400`。校验失败、恢复标记（`FD`/`FE`）或坐标越界都整帧判坏，不用坏帧合成抬手。

`Y700TouchDecode.h` 只跟踪一个主触点并保持到它抬起；主触点抬起时先入队一次释放，下一帧才把另一个触点作为新的按下，避免第二根手指被解释成跨屏拖动。按下和抬手都单独入队，抬手保留最后坐标，不因坐标没变化而丢失。`Push` 按当前 GOP（Graphics Output Protocol）分辨率判断横竖屏，把原生坐标做一次旋转映射；这是 UEFI 菜单自己的显示变换，与 Windows 侧的显示旋转是两条独立路径。

坏帧连续三次会强制入队一次抬手，防止菜单停在按下状态。连续看到看门狗标记时，`GetState` 只在应用级 TPL（Task Priority Level，UEFI 任务优先级）触发有界次数的固件重载，不在定时器回调里下载固件。

## 退出与恢复

正常移交系统或重启前，`Y700TouchStop` 先停定时器、断开并卸载协议、恢复 GPIO 162 的控制与中断配置、按读回核对四个 SPI 引脚的原值，最后释放时钟。任一步失败就设置 `Y700TouchRetained` 并拒绝继续移交。ExitBootServices 的通知只设置生命周期标记，不做清理。

## 验证

`tests/native/test_y700_touch_decode.c` 用手写报文核对 65 字节校验和、坐标边界（0 与 `19040 × 30400`）、同一触点移动、主触点切换时的抬手、空帧、坏帧和长度错误；`tests/native/test_usb_debug.sh` 会编译并运行它。这些是主机上的解码检查，不涉及设备，也不能证明队列和定时器路径正常。

在设备上确认菜单输入要读 RAM 日志：`SUNUEFI_Y700_TOUCH_RESULT` 记录初始化阶段、固件版本、报文与错误帧计数，`SUNUEFI_Y700_TOUCH_EVENT` 记录按下与抬手，`SUNUEFI_Y700_TOUCH_IRQ` 记录边沿与空闲计数。协议发布成功和收到真实触摸事件是两件事。静置后仍能使用，应表现为空闲期间 `edges` 与 `transfers` 不变、`idle` 持续增长，恢复操作后 `edges` 和 `events` 增长且 `bad`、看门狗、`recoveries` 都为 0。在 TB322FC 上已确认静置后菜单可继续点击和滑动；设备采集日志不在本仓库。GPIO SPI 的吞吐和初始化耗时需实机测量，不能按原厂硬件 SPI 的 10 MHz 推算。

## 审核里留下的限制

- UEFI 侧只有一个主触点，多点输入未实现；Windows 的多点由独立驱动负责。
- 横屏坐标映射只按 `HorizontalResolution > VerticalResolution` 判断，需要在设备上按实际渲染核对方向，这里不写成已验证。
- `PianoTouchInput.h` 在 UEFI 头里用 `double` 做坐标比例换算，ARM64 DXE 依赖软浮点；这是既有代码，不在本次改动里调整。
