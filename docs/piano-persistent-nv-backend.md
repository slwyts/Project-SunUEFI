# Piano 标准变量存储后端

本轮实现了磁盘双槽 journal、标准 FVB ABI、runtime 发布保护、VariableFlashInfoLib 和标准 VariableRuntimeDxe 的 runtime NV 写入保护。它们经过离线验证。当前产品仍使用 RAM emulation，未完成早期驱动接入；本轮没有设备写入、GPT 修改或 provisioning，不能据此宣称平板变量已经持久化。

## 共享介质与提交协议

14MiB container 仍使用既有固定 LUN4 [375040,378624)，但只有真实 GPT 第95项完整保留范围且两份 immutable volume header 校验成功后，ProductVolumeNvIo 才返回私有 IO。原厂空 gap/no reservation 返回 NOT_FOUND。journal 没有 permission/enable boolean，也没有 format/provision 回调。

container 块0/1是双 header，2..2047是 FAT 子卷，2048..2815 / 2816..3583 是各3MiB journal 槽。每槽 block0 是 header，blocks1..144 是完整589824-byte snapshot，block767 是最后提交的 footer。snapshot 包含256KiB variable FV区域、64KiB FTW working、256KiB FTW spare。

每次写入不活动槽：先使其 footer 无效并同步/读回，再写 header 和完整 snapshot，同步并逐块独立读回比较，最后写 footer、同步、读回。只有全部精确成功后才更新 RAM mirror、序列号和当前槽。warning/error、未知提交或读回不符使 dirty/quarantine 粘滞，阻止重试和虚报成功。旧活动槽全程保留。恢复验证 header/footer/payload CRC、volume UUID/layout/slot/序列及 payload 长度，选最大完整序列；同序列不同内容拒绝，损坏双槽不自动格式化。

wire 为固定小端：两种16-byte magic PIANO-NVJRNL-v1 / PIANO-NVCMIT-v1；version@16=1，metadata bytes@20=128，whole4KiB CRC@24（计算时零），layout@28=1，slot@32，kind@36=1/2，sequence@40 u64，payload bytes@48=589824，payload CRC@52，header CRC@56（只footer），reserved@60=0，EFI bytes_le UUID@64..79，其余80..4095全零。CRC是完整性检测，不是认证或权限。

## 保留标准变量与 FTW

PianoNvFvb 提供真实 Get/SetAttributes、GetPhysicalAddress、GetBlockSize、Read、Write、EraseBlocks 签名，使用4096-byte块和144-block memory-mapped FV。写遵循1→0约束，erase先验证整个参数列表再修改候选；每次变动通过 journal 同步持久化。没有另写 GetVariable/SetVariable 存储引擎。

标准 FV 使用实际 EFI_FIRMWARE_VOLUME_HEADER、系统 NV GUID 和 authenticated VARIABLE_STORE_HEADER。empty working/spare 为FF，交给标准 FaultTolerantWriteDxe 初始化/恢复，不生成猜测的 FTW record。VariableRuntimeDxe 按标准机制处理变量记录、认证、quota、reclaim、Get/Next/Set/Query；FTW 的 working/spare 和 variable FV 一起纳入原子 snapshot。

标准 SetVariable 锁提升到 TPL_NOTIFY 后调用 FVB.Write。共享 UFS private NV IO 因此需要支持真实 NOTIFY lease，不能降到 CALLBACK，不能 Allocate/WaitForEvent/运行 APP USB worker；storage provider 已针对这一 ABI 实现单一已有 DMA/MMIO poll 路径。公开 FAT 同样会在其 CALLBACK 锁内调用 BlockIO。

## runtime 生命周期

PianoNvFvbPublish 拒绝 UEFI_APPLICATION 代码：它核对实际 LoadedImage 的 RuntimeCode/RuntimeData 类别以及 publish 函数所属 image。通过实际 GetMemoryMap，要求 binding、journal、mirror 完整覆盖 RuntimeData 与 EFI_MEMORY_RUNTIME。已有 VariableArch 时拒绝发布，防止用恢复镜像覆盖已初始化的 RAM cache。

EBS通知仅设置 CPU fence；之后 FVB Write/Erase 和 journal IO 均返回 UNSUPPORTED。VA通知通过真实 ConvertPointer 转换镜像、journal 和7个 FVB 方法，清除 retired boot-only IO 与 scratch，不调用旧 UFS/Boot Services。GetPhysicalAddress 保留原物理地址，Read 使用已转换 mirror。转换失败不能继续服务，通知绑定会停止。

标准 Variable.c 增加 product-only PIANO_NV_BOOT_ONLY=1 前置保护：在 MOR/auth/cache 修改之前拒绝 runtime NV 创建/更新，并通过只读 FindVariable 拒绝 attribute-zero NV 删除，后续查找处再防御一次。默认宏0保留旧诊断行为。GetVariable/GetNext 和 QueryVariableInfo 沿用标准 runtime caches；Query 的容量是已恢复 store 的标准容量信息，不表示 runtime NV 写入可用。volatile RT 变量仍由标准引擎按其属性和 quota 处理。

UFS在OS/重启前由统一 owner manager关闭。必须先完成 NvJournalFlush、确认 clean，再禁止新 NV mutation，最后关闭 volume/UFS；不得跨EBS重新使用旧 private IO。完整 runtime NV SetVariable 仍需要另外设计设备/OS所有权和实时持久介质，不在本轮能力中。

## 当前初始化阻碍

真实启动顺序必须是：早期存储可用→验证 provisioning→恢复 journal 到 RuntimeData mirror→发布 FVB/FlashInfo→标准 FTW/VariableRuntimeDxe 初始化。bootprofiles/nv/standard-components.dsc.inc 与标准 VariableFlashInfoLib 是这一集成的 canonical组件，不是独立测试功能集；当前不能盲目把产品 EmuNv PCD 改成FALSE。

HALIOMMU 原 DEPEX 同时需要 VariableArch 和 VariableWriteArch，而早期真实 NV 恢复又需要其 DMA/SMMU，因此有实际循环。其余12个 native foundation不要求变量，但不能伪造 VariableWriteArch 或未经证据绕过 HAL DEPEX。NativeProbe源码未直接调用变量，不代表 opaque HAL内部完全无依赖。

Mu EmuNv 初始化不注册真实 FTW notify。RealNv 初始化在 entry 先复制 NV到 mNvVariableCache 并建立 offset/quota/auth 状态；之后 FVB notify并不重新从 late-restored介质构建全部cache。因此晚安装FVB不能证明已恢复保存变量。下一步需要消除该 native HAL依赖，或者经过完整验证的标准变量分阶段初始化；本轮没有实施bootstrap hack或重复安装变量引擎。

2026-10-05进一步审查固定HAL二进制（SHA `98598d11ff42f89534bb6e25fa20a0b99964a152b59df2f379021fb903ea479c`）：12项DEPEX恰为BaseTools GenDepex的整组ArchProtocols，GpiDxe也使用同一组。HAL constructor把RT保存到B210，已审指令/literal引用只发现store；GetApi/Create/Configure/Attach/Sync/Detach/Destroy最小路径没有直接GetVariable/SetVariable调用。外部资源/Kernel callbacks未全审，所以不能声明整个HAL无间接NV依赖，也不因此删除原DEPEX。

实际native Sync经2154→15DC→23F8执行CB+618/TLBIALL、CB+7F0/TLBSYNC并无界轮询CB+7F4。Detach只清路由，不清SCTLR/TTBR；Destroy把CBAR改为1FF00却未证明CB disabled。新的变量无关backend必须有界sync，核全路由引用及SCTLR.M=0，先配置和验证root/attrs再发布stream；不能照搬native弱认领条件或全局reset。当前ACTLR/TBU/secure和早期owner资格仍未知，独立源码尚不作为已验证硬件后端启用。

## 离线证据及 host seed

实际 journal/FVB C通过ASAN/UBSAN的594个 atomic/torn-write 回调断电边界，验证旧完整/已提交新镜像恢复、flash/erase/range、runtime不访问IO和VA转换。publication测试验证RuntimeCode/RuntimeData及旧VariableArch拒绝；标准入口提取测试证明NV创建和零属性删除在MOR前拒绝，诊断宏0行为保留。ARM64严格语法检查覆盖后端和实际标准 Variable.c 两种宏。

命令 `python3 tools/emit_nv_seed.py --volume-uuid UUID --output EMPTY_DIRECTORY` 调用实际 C formatter/journal，产生 nv-ftw.bin 与A/B各3MiB槽。A sequence1有效commit，B全zero无commit。工具没有设备接口，输出manifest明确 not provisioned；UUID只是host计划输入。真正首次 provisioning 仍必须由Root完成完整区域备份、GPT/volume/CRC校验及可恢复操作，不能用seed文件或用户授权boolean冒认现有介质已保留。

参考标准实现：[TianoCore EmuVariableFvbRuntimeDxe](https://github.com/tianocore/edk2/tree/master/OvmfPkg/EmuVariableFvbRuntimeDxe)；本实现增加同步双槽持久提交并明确限制 runtime NV 写，不把RAM/file emulation计为设备已持久化。

## 标准初始化阶段审查结论（2026-10-05）

审查固定Mu_Basecore `bb557081f80f4883ed832e34ab36bdca6ede1e10`，以下行号对应当前已应用runtime写保护的工作区。

- `RuntimeDxe/VariableDxe.c:580` 的 VariableServiceInitialize 先调用 VariableCommonInitialize，随后才设置gRT四个变量函数与发布 VariableArch。真实read arch不是在NV尚未初始化时提供的独立volatile-only入口。
- `RuntimeDxe/Variable.c:3880` 的 VariableCommonInitialize 先初始化NV store，再由其AuthFormat决定volatile/HOB格式，分配volatile store和scratch。`VariableNonVolatile.c:134/178` 的Real初始化直接复制当前NvBase镜像；`294/321/353`随后建立mNvVariableCache、AuthFormat、大小累计和NonVolatileLastVariableOffset。
- `VariableDxe.c:624` 仅在非Emu模式注册FTW notify；Emu直接调用WriteServiceInitializeDxe。`FtwNotificationEvent:448`绑定FVB，并在491将真实NonVolatileVariableBase改为flash/mirror base，在537初始化write service。它没有重载完整NV cache、重扫offset/space并重建所有认证上下文的late-restore合同。
- `Variable.c:3592/3640` 的WriteServiceInitialize执行reclaim、HOB flush、AuthVariableLibInitialize及属性注册；quota由`InitializeVariableQuota:566`和EndOfDxe/ReadyToBoot路径处理。换掉NV镜像或Pcd值无法同步这些状态。
- `VariableDxe.c:407/429`最终发布VariableWriteArch。该固定实现遇WriteServiceInitialize错误时只记录日志，随后仍执行安装marker；未来接入真实NV时还需确认错误不会产生写服务ready宣告。本轮没有改这个阶段或用其行为绕过依赖。

产品native HALIOMMU的原DEPEX是12个architecture GUID的AND，其中包括真实VariableArch `1e5668e2-8481-11d4-bcf1-0080c73c8881`与VariableWriteArch `6441f818-6362-4e44-b570-7dba31dd2453`；证据是private inventory中的原始depex bytes/解析。NativeProbe.c:14/48执行原DEPEX，并不会跳过它。当前PianoOwnedSmmu.c:229/234/238仍调用nativeCreate/Configure/Attach，175/202使用nativeSync。

因此“先volatile/read arch、稍后真正NV write arch”不能独自消除当前循环：HAL仍等WriteArch，UFS journal恢复仍等HAL。没有发现可直接使用的标准stage hook。提前发布RAM成功的WriteArch、晚改cache/Pcd、重复安装两份变量runtime函数或绕过DEPEX均没有被实施。

下一条确定路线是变量无关的早期SMMU/IO backend：先恢复实际NV journal到RuntimeData镜像，然后标准Variable/FTW引擎只初始化一次、发布真正写服务。Root和SMMU/storage实现负责该早期backend；此处保持现有journal/FVB接口，不扩展独立profile或bootstrap scaffold。早期NV与物理runtimeNV能力仍未完成。
