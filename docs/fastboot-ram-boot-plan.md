# Fastboot RAM boot / 1024 MiB preparation

最初阶段增加纯内容parser、离线host工具；目前还增加独立EFI启动协调器与现有CPU下载池的所有权适配器，默认未接线，尚无实机LoadImage/StartImage验收。没有改Device/Controller、现有Fastboot命令状态、prepare/构建脚本、BlockRead/Screen或设备。`PIANO_FASTBOOT_MAX_DOWNLOAD` 仍为 **64 MiB**，`boot` 仍返回未实现，不能把1024MiB作为已可用能力公布。

## 本机37与官方boot封装

[AOSP fastboot.cpp LoadBootableImage](https://android.googlesource.com/platform/system/core/+/refs/heads/main/fastboot/fastboot.cpp) 只有遇到 `ANDROID!` 输入时原样下载；否则调用 [bootimg_utils.cpp](https://android.googlesource.com/platform/system/core/+/refs/heads/main/fastboot/bootimg_utils.cpp) 把文件作为kernel生成Android容器。`fastboot boot APP.efi` 不是raw PE直传，extension不改变行为；源码也先要求输入至少sizeof(v3)=1580bytes。本机 `/usr/bin/fastboot` 为37.0.0-android-tools，help没有raw EFI boot开关。

`tools/test_fastboot_boot_cli.py` 使用本机binary、仅loopback TCP fake server，不枚举/连接USB。人工8KiB MZ fixture的`boot`实际wire为download10240bytes→boot：ANDROIDv0、page2048、kernel逐字节等于原fixture；同一Android容器作为`.img`输入逐字节原样下载。server固定FAIL，绝不执行。相同MZ文件`stage`原样传8192bytes且不发送boot；512-byte输入boot在download之前too short。封存结果为 [CLI审计JSON](fastboot-ram-boot-cli-audit.json)，不是设备boot验收。

本机也实测了显式header versions1–4。v4 raw-input wrapper存在已确认producer quirk：version=4但header_size=1580、signature_size=0；规范v4为1584。AOSP当前mkbootimg_v3_and_above同样写sizeof(v3)。新parser只把这一精确zero-signature形态单独标为`KnownV4CliHeaderQuirk`，仍必须在边界内读取完整1584bytes。它没有将该形态假称canonical v4。

未来如实现CLI `.efi`兼容，应明确两条不同路径：标准boot时有意验证/提取Android容器中的有效ARM64 PE；真正raw PE需要标准stage后显式的EFI执行命令或另一个已定义的host路径。不能因为宿主把MZ包入kernel就宣称raw EFI已经直传/执行。wrapped PE需要限制ramdisk/second/dtbo/dtb等不适用组合，并忽略不可信legacy physical load addresses；本轮未实现该策略或OEM执行命令。

## 纯parser实际能力

新 `PianoFastbootBoot.c/.h` 接收 `{Context, exact Read(offset,bytes), source Bytes}`，不要求fullbuffer pointer，不分配内存，不读取物理地址或改变状态。输出区分 **Android容器** 与 **raw ARM64 PE**，并单独说明Android kernel是否含有效PE。所有component都是offset/length，不能当作load地址权限。

Android现代header v0–v4按 [官方bootimg.h](https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/main/include/bootimg/bootimg.h) 检查header/page/version/reserved、kernel/ramdisk/second/recoveryDTBO/DTB/boot-signature与trailing；包含bounded cmdline/extra-cmdline ranges。所有rounding与component边界使用64-bit减法检查。v1/v2 recoveryDTBO offset必须匹配布局；v3/v4 page固定4096。init_boot式kernel0/ramdisk存在被识别为容器，但不假称可独立启动。vendor_boot和未知magic/version不混为boot image。

ARM64 PE32+只识别EFI application subsystem10，检查MZ/PE、AA64 machine、optional header、最多96section、image/header/file/section alignment、file/raw/RVA边界、不重叠section、file-backed executable entry、data directories与certificate file范围。certificate和Androidsignature只检查范围，未做认证；结构识别不等于LoadImage会接受、签名可信或OS能启动。所有地址字段是不可信元信息。

host负例覆盖截断、错误machine/subsystem/page/header、overflow、错误恢复offset、非法PE section/directory/certificate、reader失败/warning和失败时zero输出。1GiB logical source仅通过小metadata读测试，未分配/复制1GiB；最大单次Read1660bytes，最多约百次metadata callback。ASan/UBSan/leak与AArch64 freestanding -Werror通过。

离线真实文件对照也通过：当前39721472bytes debug Image被识别为AA64 EFI app、2sections、SizeOfImage0x26a0000；captured boot_a为96MiB canonical Androidv4，kernel0x2328a00内含AA64 PE、无ramdisk；init_boot_a为8MiB v4 ramdisk-only，不能独立boot。Trailing partition padding/AVB等保留为range，不在这里认证或执行。96MiB原始boot_a本身已经大于当前64MiB下载上限。

## 高DRAM候选与尚未取得的证明

`tools/audit_fastboot_ram_pool.py` 读取captured live.dtb、native-memory-map.json和test85完整49descriptor EFI trace，只做离线interval运算；结果保存 [内存审计JSON](fastboot-ram-boot-memory-audit.json)，带输入SHA。排除所有固定DT reserve/reserve-map、全部native regions（含FD/heap/Kernel等）、当前EFI allocations和ABL chosen initrd，不写数据或映射。

raw DT非零memory合计 **16470685696bytes=15.339521408GiB**。本次EFI Linux-is_usable_memory定义的WB types1/2/3/4/7/9/14合计 **402345984bytes=383.70703125MiB**，不是可全部用于preboot池的空闲页。Conventional仅 **282222592bytes**，最大连续 **281149440bytes**；原native DXE heap为 **443351040bytes**。类型和统计定义必须明确，不能用约384MiB宣称可分配1GiB。

fixed/native/当前所有者排除后，高段 `[0x8C0000000,0xC00000000)` 连续 **13GiB**，有13个2MiB-aligned的1GiB初步窗口；例如 `[0xA00000000,0xA40000000)` 位于高DDR bank内部。这只是fixed-clear candidate。原DT dynamic debug_kinfo_region(4KiB)和dump_mem_region(90MiB)的alloc-ranges覆盖这些候选；allowed placement window是未知base的不确定性边界，不能称为实际已占字节。没有该阶段实际分配证据时，严格保守窗口排除没有可证safe区间。

工具支持 `--runtime-reserves FILE.json` 的明确phase记录：`{"phase":"android-runtime-test-86","regions":[{"node":"/reserved-memory/debug_kinfo_region","start":"0xbfffff000","bytes":4096}, ...]}`。节点/size必须匹配DT，actual范围须在allowed范围内。当前已读入并绑定SHA的 `private/analysis/dram-runtime-android-after-test-86.json` 记录kinfo为BFFFFF000..C00000000、dump为880000000..885A00000（half-open），明确uefi_ownership_verified=false；它们只约束该Android阶段，不推定ABL/UEFI同所有权。工具分别报告observed-phase conflicts/free与unresolved UEFI placement：13个fixed-clear窗口中只有最后BC0000000..C00000000和该phase kinfo相交，A00000000..A40000000无这两项actual reserve交集。`safe_to_use`仍全部false，不能由此自动注册或公布容量。

当前TCR snapshot允许44-bit VA/PA，地址宽度容得下候选；TTBR0=D7FFF000，但没有captured page-table walk/AT probe。缺高RAM EFI descriptor不等于物理RAM或CPU映射不存在，也不能据地址宽度断言可CPU读写。候选启用仍需有效DDR bank/firmware保留/阶段所有者证明、CPU translation和实际cache/权限验证。当前普通EFI allocator无法分配这些未注册高段。

## full RAM/CMA合约与late opt-in池

test85已在8CPU、setup/mm/sched/console后，于trust_ui reusable CMA的PFNf3800触缺vmemmap L2 fault。离线工具明确区分永久no-map/secure固定区域与**fixed reusable CMA**。trust_ui F3800000..F8800000 80MiB、qdss 82800000..84800000 32MiB在当前EFI Linux-usable之外；它们对download arena是有所有者排除项，但若DT仍宣称可reusable，Linux需要它们的有效System RAM/sparse/vmemmap基础，不能统统永久删出full RAM又保留CMA registry。

后续可评估：preboot把验证过的reusable CMA作为occupied WB BootServicesData、不供allocator；EBS后成为Linux RAM，再由DT reserve/CMA接管。这仍需native安全与实际映射证明，不能仅改type/删CMA节点。高DRAM arena应late opt-in、独占reserve且不改变原低DXE heap，让有native32-bit假设的smallAlloc继续在原低地址。不要一次把13GiB都交给generic allocator。

池的后续验证应先取得正确full-memory contract和阶段reservation，再独立记录arena PA/VA/size、mapping/cache attrs、没有reserve/FD/heap/ABL overlap；分阶段用小probe验证，最后才1GiB完整write/read/hash。CPU translation检查也不能替代实际权限/所有者证明。arena在运行、reset/abort、boot交接、EBS时都需要显式owner与释放/保留规则。

## transport/boot lifecycle必须同时改

只改MAX常量不可行：当前AllocateZeroPool下载到低heap，upload Send会把完整payload复制入response queue，1GiB会产生fullbuffer doublecopy。应新增CPU-only arena/reader/writer和incremental SHA、chunk producer；硬件仍只看到有界shared-DMA bounce，不能向DWC/SMMU直接交1GiB指针。Upload用bounded chunk streaming，source owner直到stream结束/取消才按明确契约退休，boot则将已验证blob ownership转交caller，不能在Device ClearFastboot时zero/free仍需使用的kernel/initrd。

当前4KiB transfer +每轮约1ms的串行poll，1GiB单向轮询下限约 **262.144秒**，尚未计cache/log/命令/host开销；90000-loop实验与120秒外层返回策略不能直接承载1GiB。需要progress-based timeout、真实时间/看门狗与可取消长期服务，仍保持quiescence错误路径。

真正boot必须验证/准备镜像与loader预算，发送并完成IN OKAY，再逆序清理USB/UFS所有owner，最后由caller执行明确的EFI/Android handoff。标准LoadImage潜在clone/SizeOfImage/relocations、initrd额外拷贝及FD/kernel/destination重叠也要计入预算；pure parser不提供这些保证。Boot success仍以真实入口/OS进展/退出/回归证据验收。当前没有启用任何新的boot或1024MiB广告。

## 离线运行

```sh
bash tools/test_fastboot_boot.sh
python tools/audit_fastboot_ram_pool.py --runtime-reserves private/analysis/dram-runtime-android-after-test-86.json --json docs/fastboot-ram-boot-memory-audit.json
python tools/audit_fastboot_ram_pool.py --runtime-reserves PHASE.json --json PHASE-AUDIT.json
```

统一入口只编译/tmp host binary、检查本地fixtures、运行loopback CLI与offline interval tests，不操作设备或运行固件build。后续profile注册、arena/MMU变更、MAX调整、boot command执行与实机均留给root审阅后独立实施。

## 下载池所有权适配（默认未接线）

`PianoFastbootDownloadBlob.c/.h` 将当前64MiB内的完整下载池适配到启动协调器的Take/Read/BorrowView/Unborrow/Restore/ZeroRelease回调。Bind与Take都要求调用者确认命令派发已经停止、复制的TX帧已经排空；该确认不是本模块提供的控制器关闭证明。绑定时记录exact source pointer与长度，再次检查完整下载、无receiving/reboot/exit、upload确为同一下载池的borrowed view；独立截图/日志upload或变化的source不能接管。

Take必须在现有Device ClearFastboot/Reset之前执行；如果先清理原状态，源镜像已不存在，本适配器只能拒绝。Take只移动所有权，清空Fastboot状态里的Download和借用Upload，不复制完整镜像。因此正常FastbootReset不会释放已转交的source。Read检查64-bit范围；Borrow只给CPU稳定view和递增opaque loan，不映射给DMA。旧loan和未结束loan拒绝释放/归还；Restore只可交回完全空的Fastboot状态，且不宣称USB服务已重新启动。目标已有新下载时，原镜像保留给显式ZeroRelease；该操作在无loan时全量清零，再调用必需的status-returning `EFI_FREE_POOL`（应绑定实际Boot Services FreePool，不能用VOID library wrapper）。只有exact Success才消费所有权；error/warning均保留原状态，置ReleaseAttempted，禁止进一步Read/Borrow/Restore/再次Free，避免用已清零或释放结果未知的镜像继续启动。适配器为一次绑定的持久ledger，不得覆盖仍bound/owned的实例。

主机actual Fastboot.c + adapter测试已通过ASan/UBSan与AArch64语法检查，覆盖exact-success quiet、quiet回调改变source、Reset后source存活、范围溢出、source替换、stale loan、恢复目标冲突、zero-before-free，以及FreePool error/warning后禁止重试/归还。入口：`python3 -m unittest discover -s tests -p 'test_fastboot_download_blob.py' -v`。没有注册boot命令，也没有增加1GiB广告或高地址分配。
