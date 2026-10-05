# Piano Pogo stage2：native I2C只读transport准备

当前交付是固定binary/同机DT的离线审核和默认关闭的runtime-read scaffold。没有实际OEM backend、协议安装观测、UEFI I2C读取、电源/mux修改或键盘硬件验收。阶段1的report parser与四种UEFI输入协议host组件仍Ready；本轮不更改它们，也不改nativeStage、prepare、真实build/staging、Linux pins、shared DMA或设备。

`tools/audit_pogo_i2c.py`可重新生成`artifacts/pogo/native-i2c-review.json`。它固定三个输入SHA，解析真正PE section/RVA、GUID/vtable和AARCH64安装指令，检查live DT phandle/provider/cell数量及角色；审核输出始终`hardware_verified=false`、`transport_enabled=false`、`transport_backend_implemented=false`，不能作为自动enable配置。

```sh
python3 tools/audit_pogo_i2c.py --output artifacts/pogo/native-i2c-review.json
python3 -m unittest discover -s tests -p test_pogo_i2c.py -v
python3 tests/PianoPogoReportTest.py
```

## 固定binary与ABI证据

本机`upstream/Mu-Silicium/Binaries/piano/Bringup/I2C/I2C.efi`为61440bytes，SHA `7edc3c4cc51825530f53e0bd616b046abad66c740b34eafad38e14a25fcf57b0`。PE32+ ARM64、subsystem11 boot-service driver、ImageBase0、SizeOfImage0xf000、entry RVA1000。`.text` RVA1000/sizeb000，`.data` c000/2000，`.reloc` e000/1000。文件header占一页，不能对其他PE简单假设RVA等于file offset。

RVA1460的真实代码将GUID地址`image+C058`放x1、接口`image+C0E8`放x2、NULL放x3，并调用BootServices slot148，即`InstallMultipleProtocolInterfaces`。GUID为`b27ae8b1-3e10-4d07-ab5c-eb9a6dc6fa8f`。48byte接口初始数据为revision64bit `0x10000`和五个64bit地址。`PianoPogoInspectNativeInterface`只接受这个已relocate的地址布局，不将它们转成函数指针，也不调用它们。

| 接口偏移 | 方法RVA | 本版本dataflow研究 |
| --- | --- | --- |
| 00 | revision10000 | 64bit revision |
| 08 | 2720 | open：w0 instance、x1 output handle；内部5374将instance映射QUP/SE，再25c0 power-on |
| 10 | 27d8 | read：x0 handle、x1 config、w2 register、w3 register length、x4 output data、w5 read length、x6 output transferred count |
| 18 | 2c60 | write：寄存器前缀与data合成单descriptor；本轮不调用 |
| 20 | 28d4 | transfer：x0 handle、x1 config、x2 descriptors、w3 descriptor count、x4/x5 completion相关指针、w6额外参数、x7 output transferred count |
| 28 | 2764 | close：先power-off，再internal close；本轮不调用 |

方法角色/参数含义来自本版本机器码dataflow推断。没有同版本OEM头文件，callback、timeout额外参数和slave-config完整语义未确认，**不能称完整callable ABI已验证**。这份接口不是PI标准I2cMaster/Io；open没有PI方法的`This`参数。将OEM接口cast成PI接口会错放参数。

read在27f8分配48bytes，构造两个24byte descriptor。descriptor可确认：buffer pointer偏移0、length32偏移8、flags32偏移c、stride18。read首descriptor flags5、第二flagsb；register width1/2经过反转处理，word宽度与flags不能从PI布局直接复制。return通过w0，是32bitvendor status；不能直接用UEFI的高bit `EFI_ERROR`解释正数vendor error。read调用transfer后从总count减去register长度，backend还须验证完整read count为68，而不是只检status。

transfer29e8校验config偏移0为100/400/1000，符合kHz；偏移8<=1、偏移12byte的额外clock/edge配置仅允许若干值。还有地址、clock stretch、timeout字段须核对。native instance5374依赖运行时`/soc/TOP_QUP_<index>`、`/soc/I2C_HUB_<index>`的num_se及其他devcfg属性；不能把Androidbus4、LinuxSE6或`open(7)`自动视为同一个native实例。两个固定DT都没有num_se/core_base_addr/qup_id属性，因此固定resource DT存在并不证明DTBExtn运行时能提供该配置。

open不是纯身份查询：25c0→6340/6044→clock、GPIO、FIFO初始化，68f4→7394会在已载protocol不匹配时调用7030加载SE microcode，并设置FIFO水位/IRQ。它可能复用现有MCU runtime，也可能改变controller，不能先调用open再声称所有副作用仍未知但安全。

FIFO支持由25c0的mode检查、33bc transfer实现及strings互相印证；GPI mode有明确unsupported分支。33bc在同步情形轮询FIFO ISR/状态，以10us stall和计数上限结束；本版本失败路径总等待量约400ms，外层10ms deadline无法中断这一阻塞调用。所需clock/MMIO/GPIO也仍未知，故暂不实现此原生调用。FIFO/PIO软件设计不等于硬件SE已经处于可用FIFO状态。

## 精确同机板级事实与unknown

captured live.dtb SHA `a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7`。resource `upstream/Mu-Silicium/Resources/DTBs/piano.dtb` SHA `82b2404bc872c1f60eb4c54c62eb9f2094b97586bdc2aeccae9765f5f258aff1`。它们是不同snapshot，不互相替代。

- SE6 `A98000/4000`、wrapper `AC0000/2000`；controller IRQ SPI363/flags4，和MCU readyGPIO97不同。
- 当前pinctrl-0指向`/soc/pinctrl@f000000/mi_qupv3_se6_i2c_pins/mi_qupv3_se6_i2c_sda_active`及scl节点；GPIO56/57、qup1_se6_l0/l1、drive2、bias-disable。同DT通用qupv3_se6节点的pull-up不代表当前选择。
- reset188/status95/sleep3；stock flags0没有验证电气assert极性。MCU ready97 stockflags2001需结合native GPIO采样/IRQ语义核对。
- GCC provider phandle35/#clock-cells1，SE clock ID100，wrapper125/126；qcom,clk-freq-out=1MHz。native divider/source rate仍unknown。
- stock vdd=>L14/3.3V约束，dvdd=>L11/1.8V约束。stock qcom,init-voltage分别3.2V/1.064V，不能以constraint值冒充实际供电测量；后者也不能自动作为稳定runtime电压。mainline binding名字与stock不同，不能按名称反接rails。

Androidbus4/address4c、name803和四个HID、attach/power=1的证据见[阶段1审核](pogo-keyboard-source-audit.md)。这些记录属于Android phase；本轮没有UEFI电源/session证明。native GPIO mux编码、Bus枚举、DTBExtn来源、I2C SE protocol firmware、bus idle、FIFO深度与runtime IRQ状态都未验收。

## 受限transport接口与host合同

新增`PianoPogoTransport.c/.h`默认宏`PIANO_POGO_RUNTIME_READ_EXPERIMENT=0`。即使人为置Enabled，默认编译的Poll仍返回UNSUPPORTED。启用需要编译macro1、ExplicitEnable和固定SHA/SE/GPIO/rail值同时匹配；八项caller验证字段全部TRUE：ABI、native bus→SE6、native config、GPIO electrical、clock/mux/supply、existing runtime session、backend hard deadline、FIFO/PIO only。这些字段是未来集成者提交的证据合同，helper不自行测量、不替caller发证。当前审核不提供任何TRUE的硬件验证字段。

backend接口只接受deadline与68byte frame输出，操作固定为slave4c、write1 register4c、repeated START/read68/STOP；没有可传任意write/bootrom/auth命令的API。caller传入的DataReady也必须来自已核对的GPIO/中断状态；第一resume IRQ是否只是wake ACK要在未来状态机处理，不在这里无条件读。没有backend时应停在NOT_READY，不自行open/reset/alternate bus。

每次Poll最多调用backend一次、间隔至少1ms，整个诊断session最多64次，10ms总deadline且时间必须单调/不overflow。检查read count、真实parser结果；失败锁定后续读取，不重试/reset/reload。overdue返回后拒绝解析frame，避免把超时结果当成功。硬deadline必须由backend本身实现，外层检查只能拒绝迟到结果。frame是stack暂存并volatile清除，transport不留vendor/auth原始内容。Stop只停止软件读取，不调用native close/power-off。struct初始化只用于未活动的caller对象；活动session不可重初始化或在调用期间释放callback上下文。

未来可基于独立查证的GENI FIFO/PIO实现bounded backend；若需DMA，应复用项目现有shared broker/mapper并重新审查此FifoPioOnly合同，不能自行创建GPI SMMU mapping。现组件不调用DMA、MMIO、AllocatePool、native protocols或BootServices。

本轮7项Python test对真实binary/DT和实际C实现做有意义检查：GUID/vtable/安装slot、PE rawbounds/nonexec/overlap、RVA非fileoffset、所有三个SHA漂移、GPIO/clock/provider/mux/供电角色误映射；真实Mu UEFI headers +ASan/UBSan编译默认off与显式on两份C，验证无callback触发、八项gate、relocation/overflow、一次read68、rate/budget、重入拒绝、short/vendor error/timeout/clock倒退、失败不重试和软件Stop。此结果不证明native backend或者硬件键盘可用。

仅从runtime报告观察attach/hinge/auth阶段；不发bootrom04/05查询，因为需要先验证当前bootrom/runtime状态及command选择语义。源码中的这些身份查询实际也要write register5c+command，不能伪装成无write。后续确认bootrom后可单独review04/05受限诊断，06/07/08/09 MCU RAM加载与认证发送仍另有边界；当前无持久flash写或firmware下载。
