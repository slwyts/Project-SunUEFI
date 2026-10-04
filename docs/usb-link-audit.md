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
| 1 | 0 / 24 | 从固定 ramoops console 0xA3500000 捕获至多 64 KiB 尾部，返回 SUNLOG01 元信息 |
| 2 | page number / 24+该页实际 payload | SUNPAGE1，24-byte 页头和至多 512-byte 快照 payload |

所有整数为 little-endian。Status: offset 8/9/10/11 为 address/configuration/SS/SETUP 处理时的 phase；offset 12/16/20/24/28 为 DCFG/DSTS/GCTL/QSCRATCH HS/SS；32/36/40 为 device-event、SETUP、已接受的 5B reply 计数；44 为 flags（bit0 曾配置成功，bit1 快照有效）。

Snapshot info: offset 8/12/16 为 generation/total bytes/完整 CRC32，20/22 为 16-bit page size/flags。Flags bit0 表示只保留了 64 KiB 尾部，bit1 表示在保留尾部中找到最新 SUNUEFI_RAMLOG_BEGIN，并从该 marker 开始导出。未找到 marker 时返回已明确标记的 console tail，不宣称其包含完整启动日志。

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
