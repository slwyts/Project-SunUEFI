# piano USB 零事件诊断与最小 session 修复

2026-10-05。此记录依据测试 58–66 的 RAM 日志、本机原生驱动反汇编、实际 Android DT，以及 MiCode 公布的 piano 源码。session/address 修复已让 66 实际枚举为 SuperSpeed、configuration=1；新增的只读诊断分页通道已通过主机测试，完整电脑往返待下一次实机验证。

## 当前结论

已确认独立 EP0 实验遗漏了 Qualcomm USB glue 的 session-valid 设置。`PianoDwc3Device.c` 原先配置 DWC3、事件环和 EP0 后直接设置 `DCTL.RUN_STOP`；它没有运行原生 attach 回调，也没有写 QSCRATCH。保留 PHY 和 repeater 初始化状态不能替代这一步。

本机 `build/UsbfnDwc3Dxe.disasm` 提供了直接地址证据：

- RVA `e988` 中，`e990/e994` 构造 **0x0A6F8810**；`e998–e9ac` 依次读取、OR `0x00100000`、OR `0x10000000` 并写回。
- `e9b0–e9b8` 访问上述地址 `+0x20`，即 **0x0A6F8830**，OR `0x01000000`。
- RVA `e9c0` 对称清除这些位。
- RVA `6174` 的启用分支在 `61b8` 调用 `e988`，随后才在 `61e8` 设置 `DCTL.RUN_STOP`；停用分支在 `6204` 调用 `e9c0`。

因此本机 USB0 QSCRATCH 基址 **0x0A6F8800** 和以下掩码已由原生代码确认，不是从其他 SoC 推测出的地址。MiCode 的 [piano dwc3-qcom.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/dwc3/dwc3-qcom.c#L25) 给出了对应字段名，并在 VBUS notifier 中执行相同操作。

| 地址 | 寄存器/字段 | attach 设置 |
| --- | --- | --- |
| `0x0A6F8810` | `QSCRATCH_HS_PHY_CTRL` / `UTMI_OTG_VBUS_VALID` bit 20、`SW_SESSVLD_SEL` bit 28 | 原值 OR `0x10100000` |
| `0x0A6F8830` | `QSCRATCH_SS_PHY_CTRL` / `LANE0_PWR_PRESENT` bit 24 | 原值 OR `0x01000000` |

测试 65 已补强因果证据：`SESSION_SAVED hs=0 ss=0`，设置 `10100000/01000000` 并读回后，RESET、CONNECT_DONE、SETUP 首次实际出现。测试 58–64 未采集 QSCRATCH，不能补写这些旧测试的实测值；但独立 session 步骤的修复已由 65 证实有效。

## 现有日志的含义

测试 59–64 的 `DSTS.USBLNKST = (DSTS >> 18) & 0xf` 均为 **4 / SS_DIS**。测试 60–64 的 `DSTS.COREIDLE` bit 23 为 1、`DEVCTRLHLT` bit 22 为 0，`DCTL = 90F00000`：控制器已经运行，但链路仍处于 disabled 状态。`DSTS.CONNECTSPD = 4` 不能单独证明已有 SuperSpeed 主机连接。[MiCode DWC3 字段与链路状态定义](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/dwc3/core.h)

测试 58–64 没有任何 `SUNUEFI_USB_DEVICE_EVENT`、`SUNUEFI_USB_EP_EVENT` 或 `SUNUEFI_USB_SETUP`；测试 60–64 的末尾 `GEVNTCOUNT = 0`。测试 62 的 90 秒内拔插仍未产生事件，与独立实验没有转发 Type-C/软件 session 通知相符。EP 命令接受和软件 IOVA 翻译成功仍不是 USB 事件 DMA 已完成的证据。

## DWC3 编程核对

当前源中的 MMIO 偏移与原生驱动、Linux 定义相符，未找到可以解释零连接事件的偏移或 EP0 参数倒置：

| 项目 | 当前源 | 核对结果 |
| --- | --- | --- |
| Device role | `GCTL +0xC110`，`PRTCAPDIR = 2` | 正确 |
| Event ring 0 | `+0xC400/+0xC404` 地址，`+0xC408 = 4096`，`+0xC40C` 字节计数/写减 | 正确；运行时 size bit 31 清零 |
| Device event enable | `DEVTEN +0xC708` bits 0/1/2/3/9 | 包括 disconnect/reset/connect-done |
| EP0 enable | `DALEPENA +0xC720 = 3` | 两个物理控制端点启用 |
| Endpoint command | `+0xC800 + ep*0x10`；PAR2、PAR1、PAR0、CMD 分别 +0/+4/+8/+12 | 正确 |
| EP config | control type 0、MPS 512/64、physical endpoint `ep<<25`、完成 bit 8/not-ready bit 10、event ring 0 | 正确 |
| SETUP TRB | 8 bytes，TRBCTL 2，HWO/LST/ISP/IOC | 正确 |
| STARTTRANSFER | PAR0 = TRB address high、PAR1 = low | 正确 |

依据：`bootprofiles/uefi-app/PianoDwc3Device.c`、`upstream/reference-kernel/drivers/usb/dwc3/{core.h,gadget.h,core.c,ep0.c}`。本机原生 `UsbfnDwc3Dxe` 在 RVA `8c80–8ce4` 使用相同 event-ring 偏移；其 `e988` attach 操作是现有实验缺失的额外步骤。

## PHY、EUD 与 Type-C / I2C

日志中 M31 `UTMI_CTRL0 = 01` 为正常 OPMODE 且 SLEEPM 为 1；`COMMON0 = 6B` 的 PHY_ENABLE/RETENABLEN 有效、SIDDQ 为 0、FSEL 为 38.4 MHz。`HS_CTRL2 = 07` 包含 VBUSVLDEXT0，但 `VBUS_DET_EXT_SEL` bit 4 为 0，不能据此推断 DWC3 的软件 session 已有效。[MiCode M31 PHY 定义](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/phy/phy-msm-m31-eusb2.c#L24)

PM8550B SID 7 的 `FD08 = 80`、`FD46 = 80` 证明 repeater ready/enable，不能证明 cable attach。尚未读取 host workaround 的 force 寄存器 `FDE8/FDED`，但它们不应先于 QSCRATCH 的最小验证被修改。

实际 `private/analysis/live.dts` 显示：

- `usb0.extcon = <0x344>` 指向 **EUD** `/soc/qcom,msm-eud@88e0000`，其 `status = ok` 且有 `qcom,secure-eud-en`。物理 Type-C 的 role-switch 路径则连接 PMIC GLINK 下的 UCSI connector endpoint 与 usb0 endpoint。
- M31 DT 给出 EUD enable 地址 **0x088E2000**、detect 地址 **0x0C278000**。两者 bit 0 可作为后续只读观察点。M31 驱动在 device/EUD active 模式保留 EUD；不能凭 DT 中 enabled 就认定它是故障或直接关闭。
- `wcd939x_i2c@e` 在 QUP SE3 `i2c@a8c000` 上，但 `status = disabled`；同一总线的 **fsa4480@42** 为 `status = ok`。USB 节点仍保留 `qcom,wcd_usbss` phandle，这不是 WCD939x 已运行或物理芯片存在的证明。

若 QSCRATCH 设置有效仍无事件，下一项 I2C 只读诊断应优先识别 SE3 地址 **0x42** 的 device ID `0x00`，读取 switch enable `0x04`、control `0x05`、status `0x07`，再决定是否涉及 USB 数据路径。piano FSA 驱动在无音频附件时使用 control **0x18**、enable **0x98**，并识别 ID **0xF6** 的 DIO4485 变种。不要在尚未识别芯片之前照搬 WCD939x 或 FSA 写序列。[MiCode piano FSA4480 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/soc/qcom/fsa4480-i2c.c#L138)

## 已实施的最小补丁

`PianoDwc3Device.c` 在调用方已 halt 控制器、owned SMMU 已建立的入口保存两项 QSCRATCH 原值并记录 DSTS。事件环与 EP0 配置完成之后、RUN_STOP 之前，保留其他位设置 session/power-present，并读取完整值验证。任何读回不一致都会退出，不继续启动控制器。

cleanup 必须先得到 `DSTS.DEVCTRLHLT = 1`，之后才恢复两个 QSCRATCH 原值、验证读回并归还 DMA 缓冲。Halt 超时路径保留 session 和所有缓冲，走既有冷重启；该分支没有提前 restore。

新增日志为 `SUNUEFI_USB_SESSION_SAVED`、`SUNUEFI_USB_SESSION_SET`、`SUNUEFI_USB_SESSION_RESTORE`，分别包含原值、目标/读回、恢复/读回以及 DSTS。补丁没有初始化 PHY、写 PMIC、运行 Type-C/I2C 驱动或改动其他硬件路径。

`tools/test_usb_session.c` 直接包含实际固件 C 源，以主机 MMIO/DMA 模型覆盖正常零事件退出、session 读回拒绝、STARTTRANSFER 失败、分配失败和 Halt 失败。验证包括其他位保留、RUN_STOP 前 session 有效、成功 Halt 后完整恢复，以及 Halt 失败时不恢复/不释放 DMA。以下命令已通过；没有执行 prepare 脚本、完整固件构建或任何设备操作：

```sh
cc -std=gnu11 -fshort-wchar -ffunction-sections -fdata-sections \
  -Wall -Wextra -Werror -Wno-unused-parameter \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include \
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include \
  tools/test_usb_session.c -Wl,--gc-sections -o /tmp/sunuefi-test-usb-session
/tmp/sunuefi-test-usb-session
```

## 65–66 地址阶段修复与枚举结果

65 的 SET_ADDRESS(1) 在 status 完成后才写 DCFG，随后重复 RESET。`uefi.txt:503–521` 的顺序为 SETUP、EP1 STATUS NRDY、type=3/bytes=0 STATUS START、STATUS COMPLETE、此时才写地址。相同请求出现四轮，未推进到正常描述符获取。

修复把 DCFG.DevAddr 写入移到已验证 SETUP 分支，新增 ADDRESS_ARM 的写前/写后读回，软件 Address/Configuration 仍在 STATUS 完成后提交。[MiCode piano ep0_set_address](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/piano-w-oss/drivers/usb/dwc3/ep0.c#L562) 同样在 SETUP 处理时写地址；[TI DWC3 编程手册](https://www.ti.com/lit/pdf/spruhj7) p47 Table 2-50 明确要求 STATUS START 在 DCFG 新地址编程之后。

66 的日志已经顺次出现 ADDRESS_ARM、GET_DESCRIPTOR(Device/BOS/Config/String)、SET_CONFIGURATION(1) 和 `EP0_RESULT configured=1`。电脑 `usb-ep0-host-test-66/manifest.json` 记录 speed=5000、configuration=1、serial=SunUEFI-piano。该 watcher 的 `debug_verified=false` 是 libusb 打开失败，随后 queried=True 没有重试；因此枚举成功还不等于厂商诊断请求已往返成功。

## 只读 EP0 诊断协议

保留已有 `bmRequestType=0xC0, bRequest=0x5A, wValue=0, wIndex=0, wLength=12` 的 SUNUEFI1 状态。新增同样 vendor-device IN 的 **0x5B**；只在 Configuration=1 时接受。没有 OUT payload、任意地址/寄存器读取参数或写入命令。

| wValue | wIndex / wLength | 内容 |
| --- | --- | --- |
| 0 | 0 / 48 | SUNDBG01，固定 48-byte 当前状态 |
| 1 | 0 / 24 | 从固定 ramoops console 0xA3500000 捕获至多 256 KiB 尾部，返回 SUNLOG01 元信息 |
| 2 | page number / 24+该页实际 payload | SUNPAGE1，24-byte 页头和至多 512-byte 快照 payload |

所有整数为 little-endian。Status: offset 8/9/10/11 为 address/configuration/SS/SETUP 处理时的 phase；offset 12/16/20/24/28 为 DCFG/DSTS/GCTL/QSCRATCH HS/SS；32/36/40 为 device-event、SETUP、已接受的 5B reply 计数；44 为 flags（bit0 曾配置成功，bit1 快照有效）。

Snapshot info: offset 8/12/16 为 generation/total bytes/完整 CRC32，20/22 为 16-bit page size/flags。Flags bit0 表示日志超过快照上限时只保留了 256 KiB 尾部，bit1 表示在保留尾部中找到最新 SUNUEFI_RAMLOG_BEGIN，并从该 marker 开始导出。未找到 marker 时返回已明确标记的 console tail，不宣称其包含完整启动日志。

Page header: offset 8/12 为 generation/offset，16/18 为 16-bit payload bytes/header bytes=24，20 为该页 payload CRC32。**最后一页的 wLength 必须等于 24+剩余字节**，避免短包长度恰好整除 EP0 MPS 时需要额外 ZLP。Host 从 snapshot info 计算精确长度，校验 generation、offset、大小、页 CRC、完整 CRC 和状态计数增长。

快照是仅 CPU 访问的 bounded pool，捕获前后比较 console header，发生变动最多重试三次；所有回复复制到既有 `mTx`，沿已有 TRB、共享 DMA/SMMU 和缓存 ownership 发送。后续 USB/日志写入不会改变已冻结的页。总线 RESET 使快照失效；cleanup 在 controller Halt 后释放 CPU 副本，原 Halt 失败保留 DMA 的分支保持不变。

## 电脑端运行与验收

在下一次 RAM boot **之前**启动独立 reader，test-id 使用实际新测试编号（下面 68 仅示例）：

```sh
sudo python tools/read_usb_diagnostic.py --test-id 68 --seconds 170
```

reader 只匹配 1209:8750 与 SunUEFI-piano serial，等待可打开设备；保留 libusb 的 ACCESS/PIPE/TIMEOUT/NO_DEVICE 等具体错误。ACCESS 时明确要求使用 sudo 或已有 USB 访问权限；没有更改系统权限规则。读请求使用 [libusb control-transfer API](https://libusb.sourceforge.io/api-1.0/group__libusb__syncio.html)，不 claim/detach interface、不设置配置、不发 OUT 请求。

输出为 `private/analysis/usb-diagnostic-host-test-68/manifest.json`、`ramlog.bin`、`ramlog.txt`。真实验收需要 manifest 中 **debug_verified=true 和 log_verified=true**，并且 status_after.accepted_replies 相比 status_before 增长、页/全量 CRC 全部匹配。固件日志还应有 request=5B、DIAG_SNAPSHOT 和正常 IN/STATUS OUT 完成。仅看到设备节点、旧 watcher 的枚举标志或主机 mock 成功不算通道实测。

`tools/test_usb_diagnostic.c` 直接包含实际 C 源和既有 session/address 测试，额外检查 console wrap、最新 marker、稳定快照、页 CRC、越界/错误方向/错误长度拒绝、RESET 失效，以及回复沿 mTx shared-DMA 的 IN→STATUS OUT 流程。`tools/test_usb_diagnostic_host.py` 离线覆盖二进制往返、最后页精确长度、空快照、timeout 同页重试、损坏 CRC、过期 generation、reset/过期状态及大小上限，测试不初始化 libusb。

```sh
cc -std=gnu11 -fshort-wchar -ffunction-sections -fdata-sections \
  -Wall -Wextra -Werror -Wno-unused-parameter \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include \
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include \
  tools/test_usb_diagnostic.c -Wl,--gc-sections -o /tmp/sunuefi-test-usb-diagnostic
/tmp/sunuefi-test-usb-diagnostic
python tools/test_usb_diagnostic_host.py
```

## session 步骤的验收依据

只改变上述 session 步骤，保持已验证的时钟、SMMU、PHY 和 EP0 参数。先比较 SAVED 与 SET 的完整读回，再检查 DSTS 是否离开 SS_DIS、事件环是否产生 RESET/CONNECT_DONE，以及 SETUP/地址/配置是否推进。最后确认 RESTORE 的读回等于 SAVED。电脑侧实际枚举结果与上述固件证据都应留存。

65 已满足 session 设置后产生连接事件的验收，66 已满足枚举验收。后续仍以具体请求/响应和 CRC 作为电脑诊断通道证据，不用命令接受、旧 sticky SMMU fault 或 repeater ready 替代实测结果。

## 标准 fastboot 调试通道（test82 已验证 RAM 与日志往返）

`PIANO_USB_FASTBOOT` 默认 **0**。0 保留此前测试过的 18-byte、无 bulk endpoint 的 EP0-only configuration；1 必须同时应用于 `PianoDwc3Device.c` 和 `PianoUsbControl.c`，同一 `PianoDwc3Ep0Experiment` 入口启用 FF/42/03 interface、logical 01 OUT / 81 IN（DWC physical EP2/3）。VID/PID 仍为 1209:8750，serial 为 **SunUEFI-piano**。EP0 已在 test72 实机枚举成功；新增 bulk / shared-DMA quiet 已在 test82 满足下述具体 RAM 与日志往返验收，后续新 lifecycle 尚未实机验证。

本机 `/usr/bin/fastboot` 为 37.0.0-android-tools，`--help` 有 `stage`、`get_staged`、`fetch`。AOSP [fastboot 协议](https://android.googlesource.com/platform/system/core/+/refs/heads/main/fastboot/README.md) 明确两条 bulk endpoint，FS/HS/SS MPS 分别 64/512/1024，以及 `upload` 的 DATA-size → 原始数据 → OKAY 顺序。[官方 CLI 源码](https://android.googlesource.com/platform/system/core/+/refs/heads/main/fastboot/fastboot.cpp) 的 `get_staged` 调用 Upload，实际命令为 `upload`；`stage` 调用 Download。当前采用较保守的 64-byte command / response 上限，超限输入明确 FAIL，不截断；新版协议的 4096/256 上限尚未全部实现，验收命令均小于 64 字节。没有伪造 `fetch` 的 partition 内容，也没有自定义 USB 宿主工具参与新验收。

| 标准 CLI 用法 | 固件实际行为 |
| --- | --- |
| `getvar product/version/all/max-download-size` | 标识、协议 0.4、RAM 上限 64 MiB、持久存储写禁止策略 |
| `oem status` | INFO 返回策略、shared-DMA transport、DCFG/DSTS |
| `getvar SunUEFI:usb-state` | 当前配置的 S/H/F speed 标识 |
| `stage FILE` | 仅下载到上限 64 MiB 的 RAM pool，完成后成为可 upload 的数据 |
| `oem sha256` | 对完整 RAM download 输出两条各 32 hex 的 INFO，再 OKAY |
| `get_staged FILE` | 标准 `upload`，回传当前 staged RAM 数据，随后 OKAY |
| `oem ramlog` | 固定 console 捕获至多 256 KiB、最新 session marker 开始的尾部，复制到独立 frozen staged pool |
| `getvar SunUEFI:log-size/log-crc32/log-generation` | 实际 CLI 分别查询三个完整变量名；固定 8 hex metadata 描述 frozen log |
| `oem discard` | 清零并释放 RAM download / frozen staged 数据 |

`flash`、`erase`、`set_active`、unlock、EDL、任意 OEM、未知命令均 FAIL；`boot` 明确未实现。下载与 log upload 不调用 UFS、BlockIO、变量写入或 partition API。`reboot` / `continue` 保留现有 RAM command-layer 行为，必须待全部 IN response 被完成事件确认后才退出 USB 循环；验收脚本不发送它们。

Bulk MPS 来自 CONNECTDONE 时 DSTS.ConnectSpd，区分 FS1/3、HS0、SS4/5。配置描述符同时包含 SuperSpeed endpoint companion。Physical EP3 使用 FIFO1、EP2 FIFO0；初始 DEPSTARTCFG(0) 后分别配置四个 transfer resource，遵循本机 Linux `dwc3_gadget_start_config` 的持久资源方式。配置 1 在 status ACK 之前完成 endpoint programming，ACK 完成后才发布第一条 OUT TRB；配置 0 禁用 DALEPENA bulk bits、清理 CPU 队列，并等待活跃 bulk END 完成后再完成 control status。

只有 4 KiB 独立 RX/TX bounce 和 TRB / event ring 进入统一 DMA/SMMU。CPU response frame 队列最多 16 项、64 MiB+1024 bytes；Send callback 在返回前复制数据，不把 upload / console pool 直接交给 DMA。一条 IN 未完成时不发布下一条 command OUT，不允许 host 的下一条命令覆盖 response 队列。DATA、原始 upload 与 OKAY 按队列顺序发送。RAM 数据 phase 接受 short / zero packets，以声明的总长度结束。

Reset / disconnect 清 configured、software address/config、download、frozen upload、log metadata 与 CPU response 副本；当前硬件 IN bounce 不会被这些 zero/free 操作覆盖。DWC USB3.1 ENDTRANSFER 用 ForceRM+CMDIOC，**保留 active mapping 直到 EPCMDCMPLT**；event parameters 的 command 位为整个 DWORD 的 [27:24]。Linux [gadget.c](https://github.com/torvalds/linux/blob/master/drivers/usb/dwc3/gadget.c) 的 `__dwc3_stop_active_transfer` / `dwc3_gadget_endpoint_command_complete` 及当前 [core.h](https://github.com/torvalds/linux/blob/master/drivers/usb/dwc3/core.h) 提供这一依据。失败 START/END、错误完成或未知 ownership 进入全局 Halt cleanup；Halt 失败保留 DMA/session，冷重启若返回进入 CpuDeadLoop，绝不 fallthrough 让 caller 关闭时钟/SMMU。

Console 检查现已包括 signature、Start < capacity、Size <= capacity、**Start <= Size**。快照前后 header 必须一致。Metadata 从独立 frozen staged log 获取，后续现场输出或旧 5B snapshot 请求不会改写正在 upload 的内容。Reset/disconnect/discard 后 staged pool 清零释放；成功 Halt 后统一回收 DMA，保留既有 quiet event-ring cache sync。

Host-only 全测试入口：

```sh
bash tools/test_usb_fastboot.sh
```

它把实际 C 源编译为 `/tmp` host binary，覆盖默认 EP0/session/5B、command allowlist、SHA256、download overflow、frozen upload zero/free，以及 gate=1 的 65,553-byte binary roundtrip、FS/HS/SS 描述符/FIFO、状态 ACK 时序、config0 异步 END、reset/disconnect、Start>Size 拒绝。新 bulk 测试含 ASan+UBSan 与 leak detection；它没有调用设备，也没有运行 prepare 或固件构建。

Root 的 staging 需要复制 `PianoFastboot.c/.h`、新增 INF Sources 的 `PianoFastboot.c` 并为上述两个源打开 gate；BaseCryptLib（SHA256）、BaseLib、BaseMemoryLib、MemoryAllocationLib 在当前 RamApp 已存在。USB controller/owned SMMU 初始化、关机及既有 PHY/session 步骤继续由原路径负责，没有安装 native Usbfn transport。

新 RAM boot 前在电脑启动 root 提供的 CLI-only 验收脚本（82 为当前预定编号）：

```sh
python tools/check_fastboot_debug.py --test-id 82 --wait-seconds 180
```

脚本只选择 SunUEFI-piano，使用标准 fastboot：查询身份和状态、stage 65,553-byte 固定非零 pattern、核对 SHA256、get_staged 逐字节比较、oem ramlog 后读取 frozen metadata 并 get_staged 核对长度/CRC32/generation，最后 discard。电脑结果保存于 `private/analysis/fastboot-debug-host-test-82/`；需要 manifest 的 `VERIFIED_FASTBOOT_RAM_AND_LOG_ROUNDTRIP` / `debug_verified=true`，固件 `SUNUEFI_FASTBOOT_BULK_READY`、command/bytes 计数与正常 Halt/session restore 才能称 bulk 调试实机验证完成。节点枚举本身不是 roundtrip 验收。

## test82 实机验收与下一版 reboot lifecycle

test82 已成功，源码基线为 commit `9956710`。[封存验收 JSON](../artifacts/usb-debug/fastboot-validation-test-82.json) 与 [已验证镜像](../artifacts/usb-debug/piano-fastboot-bulk-verified-test-82.img) 保存这次结果；镜像 SHA256 为 `1df2ea003098556f45d179045be83cc22fa896f0c33a18785708377c56e91be7`。Host uid **1000** 使用 stock CLI，manifest 中全部 **14 条 CLI exit=0**。

RAM pattern 为 **65,553 bytes**，SHA256 `adac32e38739c26a99b60a27239bbfb419c69f2ec5aa4a546b5fca6b813ac9a8`，下载摘要和 upload 逐字节比较都匹配。Frozen log 为 **65,536 bytes**、CRC32 **90217DD8**、generation **1**，SHA256 `490f60aa608585dc418f1de714d39aa1cd0c0f4a065cc28116d8d0be46f33df9`。`current_session_marker=false`：这次导出的确为已校验的 console tail，不能称为完整 bootlog。

固件 speed=4、SuperSpeed MPS=1024，结果 Success，OUT **65,761** / IN **131,565** bytes，queue=0；QSCRATCH session 回读恢复 0/0。USB owned stream 解除验证通过，其他 stream 未改变；Android 启动完成，全部 **26** 个 boot partition SHA 仍一致。64 MiB RAM 上限、长时间压力、拔插压力和 UFS+USB 同时运行仍没有由此测试证明。

test82 没有测试 `reboot`；其后新增 lifecycle 属于下一未验证版本，计划由 test83 单独验证。Device 层现在只在收到完整 IN OKAY 完成事件、Halt 成功、全部 DMA buffer 回收成功后，发布一次性的 `PianoDwc3ConsumeRebootRequest()`；普通重启不在 Device cleanup 中直接执行。Controller 消费请求后关闭 owned SMMU，再检查八条 clock 与 GDSC 的 release 返回值，全部成功才执行 `ResetSystem(EfiResetCold)`；reset 若返回则 CpuDeadLoop。`continue` 仍按原行为退出 USB 后返回。

Halt 失败保留活跃 DMA 并按原 fail-stop 分支复位/DeadLoop；DMA free、OwnedClose、clock 或 GDSC 失败不会假装 clean 或执行普通 reboot。日志分别报告 `SUNUEFI_USB_DEVICE_CLEANUP` 的 retained buffer 数、`SUNUEFI_USB_SMMU_CLOSE` 的 attached/table 状态及 `SUNUEFI_USB_CONTROLLER_END` 的 release 失败/未确认状态，并拒绝覆盖仍保留的 Controller context。Close 失败而 DWC3 已 Halt 时可以释放 USB clocks，但 SMMU table 保留和 reboot 取消会明确记录。

`bash tools/test_usb_fastboot.sh` 已覆盖完整实际 Device event-loop 的 SETUP → configuration → reboot → IN ACK → cleanup → 单次消费，以及未 ACK / DMA free 失败不发布请求、continue 返回。独立 `test_usb_reboot_lifecycle.c` 编译实际 Controller，验证 OwnedClose → clocks → GDSC → reset 顺序、ResetSystem 返回后 DeadLoop、Close/clock/GDSC/Halt 失败保留状态与 retry refusal。Bulk/event-loop 测试通过 ASan+UBSan/leak，四个固件源通过 AArch64 freestanding syntax check；这些仍是 host 证据，实机 reboot 以 test83 的新日志为准。没有加入未核实的 `reboot-bootloader` / recovery 重启 reason。

## 标准只读 partition fetch 接口（未连接实机 backend）

`PianoFastboot.h/.c` 已提供可注入 `PIANO_FB_STORAGE`，回调为 `Ready(Context)`、`Info(Context, ExactName, Info)`、`ReadBlocks(Context, Info, Lba, Bytes, Buffer)`。`PIANO_FB_PARTITION_INFO` 包含完整 ASCII `Name[37]`、64-bit partition byte size、block size、ReadOnly、opaque Token。Info 成功必须返回与请求完全相同的名称、非 NULL token、只读标识和合法整块容量；没有 case folding、slot 推导、prefix alias、按名字接管已有分区或 RAM 快照充当 live partition。Backend 独立验证真实句柄、MediaId、GPT/device path 和 IoAlign，协议层不会绕过该检查。

只有三个回调已注册且 Ready 精确返回 EFI_SUCCESS 时，`getvar:max-fetch-size` 才返回 **0x00010000**；否则明确 FAIL backend not ready。`getvar:partition-size:<exact-name>` 返回 `OKAY0x` 后跟 16 hex 的真实 backend 容量。标准 wire 是 `fetch:<exact-name>:0x<offset>:0x<size>`，与 [AOSP FetchToFd](https://android.googlesource.com/platform/system/core/+/refs/heads/main/fastboot/fastboot_driver.cpp) 的三字段请求一致；当前只接受有明确 offset / size 的 bounded 请求。

每次 fetch 非零且最多 **64 KiB**。Offset / size 最多 16 hex digits，用 `Offset <= capacity`、`Size <= capacity - Offset` 验证，避免相加溢出。Unaligned byte head/tail 通过至多一个 block scratch 读取完整块再复制；中段整块读取，backend ReadBlocks 不接收半块请求。整个 bounded chunk 读成功后才发送 **DATA%08x → exact raw bytes → OKAY**，底层读失败直接 FAIL，不能出现 partial DATA 被当成成功。CPU 临时 payload/scratch 在全部 send / read / alloc 失败路径清零释放；send callback 仍遵守立即复制契约。

`test_fastboot_fetch.c` 直接编译实际 command layer，覆盖 Ready 关闭时不公布 max-fetch-size、精确名称/缺失 partition/无 token/非只读 metadata、64 KiB/unaligned 4K/>4 GiB/near-UINT64 ranges、读/分配/各 send 阶段失败、zero/free、以及 `flash` 继续拒绝；它通过 ASan+UBSan/leak。`PianoFastbootBlockRead.c/.h` 的真实 EFI BlockIO adapter 由 root 接入；当前 USB Device 没有注册该 backend，test83 的 USB-only reboot 测试不会宣称或提供 live partition fetch。

后续真实 backend 就绪后的 CLI 用法为 `fastboot -s SunUEFI-piano fetch xbl_config_a FILE`，优先与现有 PC 备份 SHA 比较。UFS + USB 共存仍需新测试；资源清理必须逆序 Open UFS → Open USB → Close USB → Close UFS。Combined reboot 必须在 UFS Stop/Halt/Close 后最终 reset，不能直接复用当前 USB-only Controller 最终 reset 路径，也不能根据 test82 的 USB detach 证明 UFS 共存。没有实现 partition 写入、erase、slot 切换或 flash。

## UEFI 画面标准 upload 路由（默认关闭，未实机验证）

Device 文件的 `PIANO_USB_SCREENSHOT` 默认 **0**，不会引用 `PianoFastbootScreen` helper；此时 `oem screenshot` 明确 FAIL diagnostic command unavailable，原 USB-only profile 仍可工作。打开宏后，`oem screenshot` 调用 root 提供的真实 GOP capture helper，必须实际 `Blt(VideoToBltBuffer)` 成功并 StageCopy 完整 **24-bit BMP** 后才 OKAY。失败没有伪帧或假成功。

成功 capture 的 metadata 为五个 8-hex getvar：`SunUEFI:screen-width`、`screen-height`、`screen-size`、`screen-crc32`、`screen-generation`（每项均使用完整 `SunUEFI:` 前缀）。随后标准 `fastboot -s SunUEFI-piano get_staged uefi.bmp` 使用相同 upload / shared-DMA 路径导出 frozen BMP。Successive screenshot generation 增长；reset / disconnect / discard / 新 download 清零 metadata。新的 ramlog capture 清除 screen metadata，新的 screen capture 清除 log metadata；各自描述当前 staged upload。清理 staged CPU 副本不会改写正在 IN DMA 的独立 bounce buffer。

`test_usb_screenshot_route.c` 直接编译实际 USB router，用 capture-contract mock 验证五 metadata、独立 upload、失败拒绝、generation、reset/discard/download 清除与活跃 IN bounce 保留；ASan+UBSan/leak 通过。真实 GOP → BMP helper 的像素/几何/失败路径另由 root 的实际 helper 测试验证。没有把 capture-contract mock 当成设备画面证据；实机截图、较大 BMP transfer 和新 profile 仍需后续验收。当前调试入口仍是前台 90 秒实验，后台产品调试和大分区导出时长没有由上述协议接口完成。
