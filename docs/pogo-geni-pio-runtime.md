# Piano SE6 GENI FIFO/PIO runtime diagnostic

当前交付是默认关闭、尚未接线的固定寄存器状态机与真实 C host 测试。没有调用设备、native Open、firmware loader、clock/rail/GPIO接口或prepare，没有修改现有Pogo parser、native transport scaffold、shared DMA和当前UFS strict诊断。它不能作为键盘硬件已经可用的证明。

## 本机证据与仍缺的状态

本机I2C.efi固定SHA为`7edc3c4cc51825530f53e0bd616b046abad66c740b34eafad38e14a25fcf57b0`。实际PE section/RVA解析确认RVA7450从所选SE的`+68`读取FW revision，RVA7478/747c读取并取protocol字段，RVA749c右移8位；RVA7394检查已载protocol，RVA73c4进入7030 firmware加载函数。native Open可间接走到这个流程。它还会配置clock/GPIO/FIFO，因此不能先调用Open再把它当成无副作用探测。

本轮继续沿用固定captured live.dtb：SE6地址`A98000/4000`，wrapper`AC0000/2000`，slave`4c`，SDA/SCL为GPIO56/57，ready为GPIO97，DT输出速率1MHz。DT只描述资源；它不证明UEFI阶段的clock/mux/rail、MCU runtime、FIFO和firmware状态。Android bus4也不自动等于native实例编号。

本树`Silicon/Qualcomm/QcomPkg/Include`没有与该binary匹配的OEM I2C slave-config或callable master头文件。Mu的MdePkg提供标准PI I2cMaster/I2cIo，但其ABI与该OEM接口不同。本实现不cast或调用这些未知地址。

寄存器/命令事实参考固定linux-piano `7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8`中的`i2c-qcom-geni.c`、`geni-se.h`和`qcom-geni-se.c`；测试固定其当前SHA。上游参考见[GENI I2C driver](https://github.com/torvalds/linux/blob/v6.12/drivers/i2c/busses/i2c-qcom-geni.c)与[GENI register definitions](https://github.com/torvalds/linux/blob/v6.12/include/linux/soc/qcom/geni-se.h)。代码独立实现有限状态机，没有复制Linux driver实现。

## 固定操作与接口

新增`bootprofiles/uefi-app/PianoGeniI2cPio.c/.h`，宏`PIANO_GENI_I2C_PIO_EXPERIMENT`默认0，尚未加入任何INF/DSC/FDF或prepare flag。默认编译下Initialize/Read返回UNSUPPORTED，不调用transaction callbacks；纯packet builder和显式只读Capture仍可用于后续审核。

`PianoGeniI2cPioBuildRuntimePacket()`没有slave/register/length参数，只输出两条固定命令：

| 阶段 | 长度寄存器 | M_CMD0 | FIFO数据 |
| --- | --- | --- | --- |
| WRITE1 + STOP_STRETCH | TX `26c=1` | `08009804`，opcode1、slave4c、STOP_STRETCH | `700=4c`，仅runtime register选择 |
| repeated START + READ68 + STOP | RX `270=68` | `10009800`，opcode2、slave4c | 只从`780`读RX FIFO |

没有任意write、其他slave、bootrom/auth、MCU RAM更新、SE microcode、bus-clear/STOP opcode、force-default、secondary sequencer reset或GPI/DMA接口。只有两个固定M_CMD0值能从公开Read路径发出。

IO接口接受固定SE6窗口、同步有界Read32/Write32、真实单调微秒NowUs三个callbacks。本组件不内置MMIO指针解引用、不调用BootServices或Stall、不创建DMA/SMMU映射。Root后续必须提供已经验证的设备内存映射及callback实现。

`PianoGeniI2cPioCapture()`读取30个状态/配置寄存器，包括FW revision、interface-disable、M/S active/control/IRQ/command、DMA/GSI/IRQ routing、clock/SCL counters、FIFO packing/width/depth、水位、长度和FIFO计数。它不读取RX FIFO数据，因为读数据会pop FIFO；也不写寄存器。只读Capture本身不证明MMIO已正确映射或clock已打开。

Initialize与每次Read入口要求八项caller证据：显式启用、device-MMIO映射、独占SE owner、已运行MCU runtime、clock rate/mux、rails/GPIO、电气与FW/packing、callbacks真实有界。准确FW revision和clock words必须来自下一次物理快照，不能把host fixture值当硬件值。当前没有提供这些硬件证明。

实际寄存器还须满足：FW protocol`[15:8]=3`且full revision精确匹配；FIFO接口可用，DMA/GSI/外部IRQ route为0；FIFO width32、depth非零、byte granularity0，4x8 packing为`7F8FE/FFEFE`；M/S无active/control/IRQ，FIFO空，IOS两输入都high；clock/SCL配置与证据精确匹配。不修改FW、packing、clock或GPIO来制造可用状态。不同合法packing版本也先拒绝，待真实证据单独扩展。

## Deadline、清理与保留

Read接口接受绝对截止时间，入口距deadline最多10000us且大于1000us。transfer只使用deadline前9000us部分，最后1000us留给清理；较短caller deadline也沿同一分配收敛。所有session Read32/Write32的前后都检查同一个deadline，fresh/中途/final Capture也经过这个保护。截止后不继续发起controller操作，无法完成清理则quarantine，不输出frame。

真实硬deadline仍取决于NowUs及callbacks已经有界：callback本身若卡在MMIO/固件内，这个C函数无法抢占它。host的late-callback测试证明迟到结果不会成功/继续清理，不能代替实机callback时延证明。独立poll上限4096、cleanup上限512防止计时器冻结时无限循环；iteration cap不是毫秒计时。

每次poll核对FW、DMA/GSI/IRQ route、当前M_CMD0和未拥有的secondary command；前缀结束后再次Capture配置/owner，才允许READ68。owner、GPI/DMA、命令或未知IRQ发生变化时，记录OwnershipLost并停止后续命令/取消/恢复操作，进入quarantine。不能以清理自己的名义改动未知owner状态。

已确认仍为本次命令的错误/timeout才进行有界M cancel，必要时M abort；不做bus-clear或secondary reset。Quiet读失败立即返回并quarantine，不再发控制写或恢复写。确认M/S inactive、IOS高、FIFO可安全排空后，只恢复本次改动的M_IRQ_EN、TX/RX watermark和TX/RX length，每项都读回。最终完整Capture再通过原FW/配置/idle gate才能Clean。旧M_CMD0不会被“恢复”，因为写旧命令会重新执行它；Before/After保存实际命令值。

RX计数严格限制在68bytes，FIFO word count不得超过已观察depth。非空FIFO的LAST valid必须1..4，按本binary RVA6FD0的`wc*4+last_valid-4`事实处理；不猜valid0代表4。short/extra字节、NACK、未知状态、controller或恢复失败均禁止frame交付。Frame和BytesRead只有完整68字节且Clean、deadline合格时输出；失败输出清零，局部buffer volatile清除。

Context保留attempt/command/TX-word计数、partial Received、poll/cleanup计数、Before/After、last/cleanup状态及Failed/Quarantined/OwnershipLost/BusMayBeHeld。失败不重试，已初始化context不能重新初始化抹ledger。quarantine意味着上层必须保留SE owner所需的clock/IRQ/session引用并fail-stop，不能直接宣布可boot/reset；本组件没有自动reset或释放上层资源的能力。

## Host验证与接线边界

执行：

```sh
python3 tests/test_geni_i2c_pio.py -v
```

两项Python tests均通过。实际C分别编译default0和opt-in1，使用真实Mu UEFI types、ASan/UBSan及Wall/Wextra/Werror；生产C另经AArch64 opt-in语法检查。opt-in有48个控制/故障场景：8个独立gate、1个成功事务、20个运行失败、reentry/预算2项、错误base/window2项、14个初始寄存器漂移、1个Capture错误；另有1个default-off场景及共用的packet/null/read-only Capture检查。每个运行失败还检查后续read/reinitialize不再触发callback。

成功fixture验证只发两个固定命令、一个4c FIFO word、读取17个RX words、完整68字节，并恢复五项原配置。故障覆盖64/66-byte short read、72-byte overread、NACK、未知IRQ、TX/RX途中GPI变化、foreign command、cancel成功/需abort/两者失败、计时器冻结/倒退、寄存器恢复读回错误、WRITE callback失败、FIFO read失败、late callback、fresh FW/secondary状态变化。第二项Python test核对真实native FW-read/loader指令、固定DT和三份reference源SHA。

尚未提供MMIO/monotonic adapter、native owner停车、GPIO97实际ready采样、电气/rail/clock证明、物理FW/packing/idle快照、UEFI Pogo读报告或输入协议验收。现有PianoPogoTransport的native ABI gate也未被本实现冒充通过；后续接线需要明确PIO backend的独立合同，不应伪造NativeAbiVerified。首次硬件阶段应先完成只读SE6快照，再决定是否具备固定runtime read68的条件。
