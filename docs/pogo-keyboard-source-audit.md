# Piano官方触点键盘：实现准备与硬件边界

目前Android证据确认这台设备的Nanosic I2C设备：bus4/address004c、name803、driver nanosic,803，DT为`/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000/nanosic@4c`。已捕获四个I2C虚拟HID：15d9:00a3 Keyboard、00a2 Mouse、00a1 Touch、00a4 Consumer；attach/power=1、键盘119/触控板d01来自本次Android运行日志。裁剪input记录在`private/analysis/pogo-keyboard-2026-10-05/input-devices.txt`，相关dmesg的早前断开与后续连接应按时间区分，不能全部当成当前状态。它们证明Android链路存在，不证明UEFI transport可用。

参考源固定为linux-piano `7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8`的[hid-nanosic-wn8030.c](https://github.com/blu-sharky/linux-piano/blob/7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8/drivers/hid/hid-nanosic-wn8030.c)，本机文件SHA `632324fe94f52e54de80d735840349223309451fa6e6398dde4386d01bb2c33c`。这是协议研究来源，未复制Linux实现或HID descriptor到UEFI代码；新代码独立按字段事实编写。该fork将mouse/consumer集合并入keyboard HID，而Android分成四个product，不能按Androidproduct数目猜runtime report布局。

## 同机DTB核对

基线是已捕获live.dtb，SHA `a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7`。

| 项 | 同机真实值 | 使用边界 |
| --- | --- | --- |
| I2C SE | `0xA98000/0x4000`，QUP1_SE6 | `qcom,i2c-geni`，status ok |
| wrapper | `0xAC0000/0x2000` | m/s-AHB时钟；有原生IOMMU/DMA描述 |
| SDA/SCL | GPIO56/57，qup1_se6_l0/l1，drive2，bias-disable | 要核对native mux编码/clock gate，不能直接用Android驱动编号作EFI handle |
| MCU IRQ | GPIO97，stock irq_pin flags0x2001 | fork用rising edge；GPIO数据ready与SE controller IRQ不同 |
| SE controller IRQ | SPI363、flags4 | FIFO IRQ或PIO轮询的controller completion来源 |
| reset/status/sleep | GPIO188/95/3，stock flags0 | Vendor reset_pin/sleep_pin的电气assert语义仍须核对；flags0不证明fork ACTIVE_LOW转换正确 |
| SE clock | GCC phandle0x35、clock index0x64（100），clk-freq-out1000000 | clock spec第二cell是clock ID，不是另一个phandle；source rate/divider未实机确认 |
| wrapper clocks | GCC ID0x7d/0x7e | 不以其他恰好同值的DT phandle误读provider |
| stock vdd | L14B固定3300000uV | stock名字vdd对应3.3V |
| stock dvdd | L11B固定1800000uV | stock名字dvdd对应1.8V |

mainline binding把1.8V称vddio、3.3V称dvdd，**不能按stock属性名直接迁移**。fork的power-on按1.8V I/O先开、3.3V后开，off反序；Piano variant reset2ms、boot500ms、wake50ms、bootloader每write后3ms。这些是源码策略，不能替代native GPIO/PMIC电气验收。本轮没有修改电源、mux或GPIO。

## Runtime与MCU边界

Piano runtime用slave0x4c，register0x4c读取68bytes；header byte0=0x57，byte2非零，report从offset3 packed。byte1/byte2的完整含义和incoming checksum尚未从本机raw traffic确认，当前只实现源码已使用的检查，不发明CRC。

| report ID | 包含ID的长度 | 字段 |
| --- | ---: | --- |
| 02 | 8 | 5buttons；signed16LE X/Y/wheel |
| 05 | 9 | modifier8bit、reserved8bit、6个HID key usages |
| 06 | 5 | consumer16LE；另外2bytes未解释 |
| 19 | 27 | button byte、3组8bytecontact、contact count；X0..3199/Y0..2135 |
| 22/23 | 16/32 | vendor route38/80、command；attach/hinge/auth等 |
| 24/26 | 当前frame剩余 | 可变vendor；边界受68byte frame限制 |

触控板pressure在参考descriptor中作为constant，不用于UEFI Z压力。当前absolute转换保持一个稳定primary contact ID，保留三contact原始状态；不伪造第四/第八contact，也不将absolute delta再加到mouse relative，避免两个report重复移动。Palm/confidence未满足tip/in-range/confidence组合时不当作active primary。

参考MCU bootrom路径使用register5c：查boot ID c8、state0、header06、data07（每包最多503B）、verify08/state3、start09。firmware文件是`nanosic/MCU_Upgrade.bin`，header位于7fc0，code8000。该路径是RAM下载，当前UEFI组件没有实现它，也没有flash/erase命令。未来loader必须固定同机firmware SHA、完整检查header/code-size/entry和每次传输边界；不能照抄仅检查最小A000文件长度后按header size计算checksum的路径。

sleep恢复的第一IRQ在参考驱动中只清suspended，第二IRQ才携带连接状态；未来native state machine要区分resume ACK、runtime report与bootrom，不能每个IRQ盲读68B或无限reset/reload。12秒keyboard-power keepalive、touchpad enable、LED/backlight等runtime commands目前也未发出。

认证包含vendor24/31/32和外部token提供者；Android已有devauth service连接状态证据。当前parser只通知AuthRequired/UidAvailable/ChallengeAvailable阶段，不复制或记录UID/challenge/token、不回复、不认定认证成功。是否需要认证才能持续输入、原生MCU与cover独立寿命/电源、token合法提供方式仍需真实链路确认；不对现有keys文件或服务作任何修改。

2026-10-05 再次只读核对当前 Android HID producer，并保存四份静态 report descriptor 到 `private/analysis/pogo-hid-2026-10-05/`。真实 keyboard input 为 ID05/9bytes，mouse 为 ID02/8bytes，touchpad 为 ID19/27bytes；三个触点的 X/Y logical maximum 都是3199/2135，与当前 parser 一致。Android consumer HID 是 ID06/3bytes；它是软件提交给 HID 的长度，不能用来把参考 I2C packed frame 的5bytes解释为3bytes，尾部两字节仍未知。descriptor 只能验证 Android 软件输入格式，不是 UEFI I2C 实际收包或认证成功的证据，也未读取实时按键或认证材料。

## Native EFI I2C/FIFO路径

本机Mu-Silicium commit `66e7bd1e7bcb757d4b28629bd6409d7209d3b242`包含PI标准I2cMaster/I2cIo headers。标准master GUID为cd72881f-45b5-4feb-98c8-313da8117462；IO GUID为b60a3e6b-18c4-46e5-a29a-c9a10665a28e。`EFI_I2C_REQUEST_PACKET`的operation之间是repeated START，末尾STOP。新`PianoPogoI2c`只构造write1(register4c)+read68的2-operation请求，没有LocateProtocol/StartRequest或任何硬件调用；异步请求要求buffer保持有效到completion，不能把stack packet交给异步服务后返回。

`Binaries/piano/Bringup/I2C/I2C.efi`为61440bytes、SHA `7edc3c4cc51825530f53e0bd616b046abad66c740b34eafad38e14a25fcf57b0`。binary strings表明有QUP FIFO路径、GPI不支持的分支，以及DTBExtn/HWIO/clock/devcfg依赖；它需要QUP/SE的core_base_addr/common_base/se_index/se_clock等native配置。文件存在不能证明当前pianoProbe已安装此protocol。后续[stage2 ABI审核](pogo-keyboard-native-i2c.md)已从安装指令确认OEM GUID/vtable/RVA与PI不同；完整callable参数语义、QUP1_SE6枚举、native配置和硬件读取仍unknown，不调用猜测接口。

若现有native接口不能证明可用，独立GENI FIFO/PIO方案可以参考固定kernel `i2c-qcom-geni.c`/`geni-se.h`，绕开GPI DMA/IOMMU数据通路。但仍须先确认I2C SE firmware protocol、FIFO enable/depth、source clock、GPIO mux、bus idle。SE offsets包括FW revision68、M_CMD600、IRQ610/clear618、TX700/RX780、FIFO status800/804；不能据这些offset就自运行MMIO。未来PIO所有wait必须有总deadline/iteration cap，NACK/abort/cancel失败必须返回并保留recovery，禁止更换SE microcode或无限bus-reset。本轮未执行这些动作。

## 已落地的host组件

`bootprofiles/uefi-app/PianoPogoReport.c/.h`提供事务性packed parser、64-key queue、16-control ring、relative累加/INT32饱和、absolute primary/release、detach清输入、软件toggle与unknown/auth阶段。坏frame/key queue overflow不提交半帧；控制telemetry满时丢最旧并计数，不使键盘输入永久停住。重复attach coalesces。未知incoming vendor不执行命令。

`PianoPogoInput.c/.h`提供真实SimpleTextIn、SimpleTextInputEx、SimplePointer、AbsolutePointer方法，8个bounded key-notify注册、caller提供的readiness signal/events；没有install handle/gBS或硬件Reset。ExtendedVerification返回UNSUPPORTED，普通Reset只重置软件状态。legacy WaitForKey只在存在非partial key时signal。mouse counts/mm resolution必须由调用者提供已核对值，代码不编造physical resolution。absolute范围采用参考layout，无压力轴。

当前按键采用US bootstrap，涵盖modifier、Caps/Num/Scroll、常用scan、keypad、consumer常用EFI scan。HII layouts/dead keys、typematic timer、触控板多指gesture/滚动、硬件LED与电源/认证状态机尚未实现。协议方法和synthetic报告host通过不等于设备完整适配。

运行`python3 tests/PianoPogoReportTest.py`会用真实Mu UEFI X64 headers、-Werror、ASan/UBSan编译实际三组PianoPogo C源码，覆盖packed/badframe事务回滚、键/lock/modifier/rollover、queue、负relative/wheel和饱和、touch/primary/release、detach、auth信息最小化、notify重入拒绝、真实protocol方法和PI请求布局。没有设备操作、prepare/staging/build修改或commit。memory-debug kernel保留Ready并仍可独立包装。
