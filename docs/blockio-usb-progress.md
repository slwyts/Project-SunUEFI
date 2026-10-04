# UEFI BlockIO / USB 当前状态

## UFS BlockIO

第 57 次已实机发布 6 个只读 LUN 父设备；标准 DiskIoDxe / PartitionDxe 连接后形成 133 个 GPT 分区子设备，共 139 个 BlockIO 句柄。6 个父设备的标准 ReadBlocks 多块测试成功，GPT signature 正确，所有 WriteBlocks 测试返回 EFI_WRITE_PROTECTED。设备端实际发生 900 次 4 KiB 只读块传输。恢复后全部 26 个启动分区哈希一致，6 份 GPT 与 Android 只读对照一致。

`PianoReadOnlyBlock.c/.h` 是独立只读适配器，未使用早期带写入功能的 `PianoUfsBlockIo.c`。IoAlign=1，通过统一 DMA bounce buffer 支持调用者未对齐的 buffer。检查 MediaId、长度倍数、64 位 LBA 范围与错误传播；当前硬件 transport 为 READ(10)，超出其 32 位 LBA 范围会拒绝，当前 6 个 LUN 均在范围内。

控制器、时钟和 owned SMMU 保持到消费者停止。正常卸载先 DisconnectController / 卸载协议，再停止队列、解除映射和关闭时钟。自动恢复路径通过 shutdown protocol 先 halt UFS，不在 timer 回调里释放消费者或其 page tables；之后 cold reset。同步读操作串行，重入返回 NotReady。

大量分区探测会覆盖 2 MiB ramoops 环；因此 halt 时重新输出紧凑的最终报告。第 56 次初始记录被覆盖，不据此声明完整验收；第 57 次获得了完整最终报告，原始捕获均保留。

## USB

第 58 次在保留原 PHY / Type-C 状态下开启 USB GDSC 和 7 项已核对时钟。真实读回 DWC3 ID=33313130、GCTL=00102005、GUSB2PHYCFG=00102400；controller halt 成功。USB SID40 对应 native HAL 的 USB0，owned stage-1 CB0 创建及 PA→IOVA 查询通过。native attach 使用参数 03000000；detach 参数 0 返回成功却未解除流，读回检查因此保留了表内存。使用匹配参数解除后，59–64 次实机读回确认 USB 流已消失、其他流未改变，表内存才释放。

`PianoDwc3Device.c` 实现隔离的 USB2 EP0 实验：事件环、TRB、setup / IN payload 全部使用共享 DMA 分配、映射与缓存生命周期；active event ring 通过 PianoDmaSyncForCpu 只读同步，不假装 DMA 已停止。endpoint START / END、controller halt 失败时不释放硬件仍可能访问的内存。

`PianoUsbControl.c` 支持 Device / Configuration / String / Qualifier 描述符、GetStatus、GetConfiguration、SetAddress 和 SetConfiguration。地址和配置在 status completion 后生效；未知或非法请求 stall。配置完成后只读 vendor IN 5A 返回 SUNUEFI1 诊断信息，不提供写入命令。

第 59–64 次 EP0 实机观察均未收到连接 / SETUP 事件，电脑未枚举；62 次已确认在 90 秒运行窗口内拔插。HS / SS、核心软复位、事件缓冲区中断屏蔽清除和 Run / START 顺序均做过隔离测试；不能声明 USB 已枚举或 PC 调试往返成功。电脑端 `watch_usb_ep0.py` 只观察 1209:8750，读取标准描述符 / 配置和只读诊断请求，保存原始描述符及结果。

USB 枚举实验仍独立于 UFS；没有启用 native UsbConfigDxe / UsbfnDwc3Dxe 的缺依赖路径，也没有把 UFS 的 IOVA 或 context bank 直接共享给 USB。成功后再考虑 bulk fastboot。

第 63 / 64 次只读 SPMI：SID7 FD08=80、FD46=80（ready / enable），FD51=0A、FD54=03、FD55=03、FD57=03 与同机 XBL tuning 一致。M31 PHY CTRL0=01、COMMON0=6B、HS_CTRL2=07，SS COM=01。设备没有收到新的连接事件，当前优先排查 WCD9390 / Type-C mux / VBUS-session-valid 的传递，不能仅凭这些寄存器宣称模拟链路完全正常。没有修改 PMIC 电压或 repeater 配置。

源代码现在在未配置时返回 EFI_NOT_READY，避免把 endpoint command 被接受误当 USB 可用。这个结果状态修正只做主机编译检查；59–64 原始日志中的 Success 表示当时轮询/清理代码未报错，configured=0 才是枚举结果。

BlockIO 的 ExitBootServices halt-only hook 已 ARM64 编译，当前 57 次已验证镜像未含该后续改动；未验证版本单独命名 piano-blockio-ebs-UNTESTED.img，不能把其 EBS 回调描述成已完成 OS 交接实测。
