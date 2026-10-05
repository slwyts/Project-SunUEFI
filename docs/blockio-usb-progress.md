# UEFI BlockIO / USB 当前状态

## UFS BlockIO

第 57 次已实机发布 6 个只读 LUN 父设备；标准 DiskIoDxe / PartitionDxe 连接后形成 133 个 GPT 分区子设备，共 139 个 BlockIO 句柄。6 个父设备的标准 ReadBlocks 多块测试成功，GPT signature 正确，所有 WriteBlocks 测试返回 EFI_WRITE_PROTECTED。设备端实际发生 900 次 4 KiB 只读块传输。恢复后全部 26 个启动分区哈希一致，6 份 GPT 与 Android 只读对照一致。

第 67 次加入 EnhancedFatDxe 和真实 SFS 检查后，仍有 139 个 BlockIO 句柄，识别到 7 个 UFS 文件系统卷（1 个属于 LUN0、6 个属于 LUN4）。全部 OpenVolume/GetInfo/目录读取成功且 ReadOnly=1；LUN0 卷中一个普通文件通过 EFI_FILE_PROTOCOL 读取 4096 bytes，CRC32=7C5D9A17。其余卷的 file=Not Started 不代表普通文件读取成功。本次 1358 次 4 KiB 只读传输，所有 6 份 GPT CRC 验证成功，返回 Android 后 26 个启动分区 SHA-256 一致。Shell 已编译打包，但本轮完整 Shell 输出被日志环覆盖，尚不据此声明命令执行验收。

`PianoReadOnlyBlock.c/.h` 是独立只读适配器，未使用早期带写入功能的 `PianoUfsBlockIo.c`。IoAlign=1，通过统一 DMA bounce buffer 支持调用者未对齐的 buffer。检查 MediaId、长度倍数、64 位 LBA 范围与错误传播；当前硬件 transport 为 READ(10)，超出其 32 位 LBA 范围会拒绝，当前 6 个 LUN 均在范围内。

控制器、时钟和 owned SMMU 保持到消费者停止。正常卸载先 DisconnectController / 卸载协议，再停止队列、解除映射和关闭时钟。自动恢复路径通过 shutdown protocol 先 halt UFS，不在 timer 回调里释放消费者或其 page tables；之后 cold reset。同步读操作串行，重入返回 NotReady。

大量分区探测会覆盖 2 MiB ramoops 环；因此 halt 时重新输出紧凑的最终报告。第 56 次初始记录被覆盖，不据此声明完整验收；第 57 次获得了完整最终报告，原始捕获均保留。

## USB

第 58 次在保留原 PHY / Type-C 状态下开启 USB GDSC 和 7 项已核对时钟。真实读回 DWC3 ID=33313130、GCTL=00102005、GUSB2PHYCFG=00102400；controller halt 成功。USB SID40 对应 native HAL 的 USB0，owned stage-1 CB0 创建及 PA→IOVA 查询通过。native attach 使用参数 03000000；detach 参数 0 返回成功却未解除流，读回检查因此保留了表内存。使用匹配参数解除后，59–64 次实机读回确认 USB 流已消失、其他流未改变，表内存才释放。

`PianoDwc3Device.c` 实现隔离的 USB EP0 实验：事件环、TRB、setup / IN payload 全部使用共享 DMA 分配、映射与缓存生命周期；active event ring 通过 PianoDmaSyncForCpu 只读同步，不假装 DMA 已停止。endpoint START / END、controller halt 失败时不释放硬件仍可能访问的内存。

`PianoUsbControl.c` 支持 Device / Configuration / String / Qualifier 描述符、GetStatus、GetConfiguration、SetAddress 和 SetConfiguration。DWC3 的 DCFG.DevAddr 在 SET_ADDRESS 的 SETUP 阶段、启动 status TRB 之前写入；软件地址/配置状态仍在 status completion 后提交。未知或非法请求 stall。配置完成后只读 vendor IN 5A 返回 SUNUEFI1 诊断信息，不提供写入命令。

第 59–64 次 EP0 实机观察均未收到连接 / SETUP 事件，电脑未枚举；62 次已确认在 90 秒运行窗口内拔插。HS / SS、核心软复位、事件缓冲区中断屏蔽清除和 Run / START 顺序均做过隔离测试；不能声明 USB 已枚举或 PC 调试往返成功。电脑端 `watch_usb_ep0.py` 只观察 1209:8750，读取标准描述符 / 配置和只读诊断请求，保存原始描述符及结果。

第 65 次按同机原生代码启用 QSCRATCH HS session/VBUS 和 SS power-present 后，首次收到 RESET、CONNECTDONE、SETUP。第 66 次同时修正 DWC3 SET_ADDRESS 时序，电脑实际枚举为 **1209:8750、5000 Mb/s、configuration=1、serial=SunUEFI-piano**；固件处理了 Device/BOS/Configuration/String 描述符、SET_ADDRESS 和 SET_CONFIGURATION。正常 halt 后 QSCRATCH 原值恢复、owned SMMU 清理读回通过，返回 Android 后 26 个启动分区哈希一致。PC vendor IN 往返尚未通过：此轮 libusb 无权限打开设备，不能把成功枚举等同于 debug_verified。

第 72 次 PC vendor IN 已通过实机验收：兼容 5A 与扩展 5B 状态/快照/分页，debug_verified=true、log_verified=true，128 页共 65536 bytes，每页及总 CRC32=C15D667A 校验通过，132 次请求，accepted_replies 从1增长至131。软件/硬件地址均为1，configuration=1，SuperSpeed连接成立。该快照仅为末尾64 KiB，current_session_marker=false，不能充当完整启动日志；ring轮询日志降噪另行处理。正常 halt 后 QSCRATCH 恢复原0/0、USB SID40解除且其他stream不变，返回 Android 后26启动分区哈希一致。第72次本身没有bulk OUT或文件上传；第82次后续标准fastboot已完成这些RAM功能，仍没有fastboot flash写入。

第 70 次最终 Shell 摘要已实机确认 map-r、dh-simplefilesystem、drivers 三条命令均真实 Load/Start 并返回 Success；正常应用返回后的显式 UnloadImage 为 Invalid Parameter，与核心自动卸载相符。7个真实SFS卷再次识别，全部只读。MODE SENSE(10)读取所有LUN的缓存页成功，均DPOFUA=1；LUN4为WCE=1、mode WP=0、unit bLUWriteProtect=1。LU配置值需结合真实 fPowerOnWPEn/fPermanentWPEn 标志解释，尚不能据此直接宣称可写。

第76/77次进一步只读查询确认 fPermanentWPEn=0、fPowerOnWPEn=0。LUN4 的 bLUWriteProtect=1 是配置支持，上电保护当前未启用。第80次随后完成固定块FUA写入/sync/独立读回/恢复，并经Android完整gap/GPT/邻块再读取确认，详见[受控写入](ufs-controlled-write-transport.md)。原分区BlockIO仍只读；第86次限定测试窗口可写BlockIO/FAT文件读写与全gap恢复已实测，见[限定文件系统](ufs-bounded-fs-session.md)。第77次队列最终TR/TMR doorbell和两个run、IRQ读回全0；标准Setup服务/FV LoadImage成功且StartImage被调用，等待输入期间由原120秒timer自动恢复，所有26分区SHA一致。

USB-only 已测基线与联合 UFS/USB 实验使用独立profile；没有启用 native UsbConfigDxe / UsbfnDwc3Dxe 的缺依赖路径，也没有把 UFS 的 IOVA 或 context bank 直接共享给 USB。第82次标准fastboot bulk通过实机验收：普通uid1000运行全部14条标准CLI，RAM65553bytes SHA256和逐字节回传、日志65536bytes CRC90217DD8校验成功。固件OUT65761/IN131565、queue0、Halt和session0/0恢复、USB SID40解除且其他streams不变；Android26SHA一致。封存artifacts/usb-debug/fastboot-validation-test-82.json和piano-fastboot-bulk-verified-test-82.img。日志仅64KiB尾部，未包含当前session起始marker，不能作为完整bootlog。第83次标准reboot完整清理返回Android，第84次标准CLI导出3200×2136 GOP BMP并校验通过。当前前台实验尚未集成菜单常驻服务；目标重启和flash待做。

第 63 / 64 次只读 SPMI：SID7 FD08=80、FD46=80（ready / enable），FD51=0A、FD54=03、FD55=03、FD57=03 与同机 XBL tuning 一致。M31 PHY CTRL0=01、COMMON0=6B、HS_CTRL2=07，SS COM=01。设备没有收到新的连接事件，当前优先排查 WCD9390 / Type-C mux / VBUS-session-valid 的传递，不能仅凭这些寄存器宣称模拟链路完全正常。没有修改 PMIC 电压或 repeater 配置。

源代码现在在未配置时返回 EFI_NOT_READY，避免把 endpoint command 被接受误当 USB 可用。这个结果状态修正只做主机编译检查；59–64 原始日志中的 Success 表示当时轮询/清理代码未报错，configured=0 才是枚举结果。

BlockIO ExitBootServices halt-only、Reserved DMA/table和失败retain代码已经编译，且第77/80次正常halt读回通过；完整带UFS的OS EBS交接仍未验证，见[生命周期](ufs-ebs-lifecycle.md)。

## 第87/89次联合只读fetch

标准fastboot已通过真实UFS桥读取exact `xbl_config_a`：524288 bytes、64KiB分段，与已有原始备份逐字节和SHA256一致（e95c1e673bee3a400a7f9992fe23f43ecbc5cc5ff6f0312003ec948b08e2b8fc）。SID60/40分别使用不同CB0/1和不同root；第87次60项读取前后检查通过，无UFS写入。USB关闭和UFS队列halt成功，但UFS native detach后的严格其他槽位比较拒绝释放，因此联合自动重启未通过。第87次手动恢复后26启动分区SHA一致。

第87次未加CRC的文本中出现slot113差异，但真实USB stream是slot1；不能把slot113称为USB peer，也不能据此宣称硬件该槽发生变化。第89次加入CRC双份baseline/final/rejection和两次直接MMIO读回，数据fetch再次匹配，现停在保护检查等待Android恢复后回收。该关闭问题解决之前，不宣称联合生命周期稳定，不放宽其他未知槽位检查。证据见 `artifacts/usb-debug/fetch-validation-test-87.json`。
