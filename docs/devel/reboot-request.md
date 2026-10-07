# 独立 UEFI 请求与原厂重启行为

普通开机和 `reboot recovery` 应继续进入原厂 Android/Mi Recovery。UEFI 使用独立请求，不把原厂 Recovery 模式当作 UEFI 请求。

## 当前状态

合体 BOOT 的普通 Android 路径和原厂 `reboot recovery` 已实测。最新产品也已通过普通 BOOT 冷启动进入磁盘上的 Linux；原厂 `fastboot continue` 仍可作为调试启动入口。

`piano-boot-request` 提供 `status`、`set`、`consume`，只读写合体 BOOT 自有的两份请求页，不重包原厂内核。Linux 安装路径为 `/usr/local/sbin/piano-boot-request`。本机已手动消费 Linux 请求、正常重启回 Android；自动消费、记住上次路线及 Android 模块按钮的完整联动尚未完成。不要把手动验证当作已经具备一次性自动请求。

```sh
sudo piano-boot-request status --device /dev/disk/by-partlabel/boot_a
sudo piano-boot-request set --device /dev/disk/by-partlabel/boot_a --target linux
sudo piano-boot-request consume --device /dev/disk/by-partlabel/boot_a
```

目标为 `android`、`menu`、`linux`、`setup`。明确指定实际使用的合体 BOOT 分区；命令不会对原厂 BOOT 或其他格式猜测写入。

原生 C 命令位于 `android/native/piano-boot-request.c`，与前置选择器复用同一 CRC 实现。可用 `./build.sh boot-request --sysroot ARM64根系统目录` 生成静态 AArch64 ELF，Android/Linux 使用同样的 `status / set / consume` 参数。它恢复块设备原来的只读状态，只更新自有请求页，不重包 BOOT，也不发出重启。

原生工具已与真实合体镜像、Python 工具和固件实际读取函数进行主机互操作检查；尚未在 Android 实机验证。它是独立请求命令，不等于模块需要的完整 OTA 重打包／还原工具，也不会自动清除一次性请求或补齐模块 WebUI。

以下保留前置选择器的实现与历史定位记录，实验时的“未完成”描述不代表上述当前状态。

## 当前选择器

前置选择器支持合体镜像自有区域中的请求页。两份 4 KiB 页位于原厂 GKI 工作区之后、selector 之前；每份含 64 字节记录：magic、版本、序号、目标、CRC32、当前 APP 标识和保留字节。最高有效序号决定目标；同序号冲突、旧载荷标识、两份均损坏时保留原厂路径。默认两份都是“无请求”。

主机/QEMU 诊断仍接受 `/chosen/bootargs` 中唯一、准确的 `sunuefi.boot=uefi`；普通 normal/recovery 参数本身不选择 UEFI。请求页的读取和包布局已接入原型，写入、消费及 Linux/Setup 目标分发尚未完成。

这只是已实现的请求**读取端**。Android 原生请求工具和 UEFI 消费/清除流程尚未完成。打包器的 `--uefi-request` 会生成持久的诊断请求，不能当作一次性请求安装到日常设备；模块打包会拒绝它。前置读取通过并不能代替真实 BOOT 更新和掉电测试。

合体布局保留原厂 GKI 在入口位置，在其 `image_size` 工作区之后放请求页、选择器、独立栈、BootShim、同一份产品 FD 和带摘要的 APP。当前 NORMAL 直接跳原厂 code1 所指的 `primary_entry`，不修改代码页或绘制早期画面；保持 DTB、initrd、x0–x3、SP 和 DAIF。UEFI 请求进入 BootShim，APP 通过固定交接记录提供，不依赖原厂 init_boot/recovery ramdisk 包含 APP。

主机/QEMU 已验证两条分支的寄存器、入口、范围、交接记录。2026-10-07 的第一份正常 BOOT 候选使用了头部恢复与早期画面，实测停在小米 Logo，没有进入 HyperOS，**Android 旁路尚未验证成功**；随后恢复原厂 BOOT 并确认 Android 正常启动。

静态 ABL 核对确认 raw ARM64 路径按 Android header 的 kernel_size 复制完整载荷，不按旧 PE SizeOfImage 裁剪。因此尚不能归因于“只复制了原厂内核”。简化后的直接入口方案先通过了 QEMU，再进行了下述实机检查。缺少本次有效运行轨迹时，不能把 framebuffer、缓存、64 MiB 或代码页权限当作已确认原因。

随后直接入口候选增加了早期阶段标记，再做一次普通 BOOT 实测，仍停在小米 Logo。已恢复原厂 BOOT，并确认 Android 启动完成。候选标识 `29d2e9b3db220f74` 未出现在恢复后的 pstore 中，黑匣子也没有对应的新 ABL 错误；不能由缺失标记推断选择器未执行。

早期标记仅向已经匹配的 `0xA3500000` 前半 2 MiB DBG_C 环追加，不初始化或清空。EL1 调用建立私有栈后才开始；NS 访问权限、强制恢复后的保留情况仍未证明。7 项诊断边界检查与7项选择器检查（含 QEMU）通过，不能替代本机运行证据。

QEMU 使用通用 `virt` ARM 平台及检查寄存器/栈/交接记录的入口 stub，只运行本项目选择器。原厂 XBL、ABL、GKI 和 HyperOS 没有在此仿真中运行。下一步先准备原厂内核仅绕过一个纯汇编分支再进入原入口的隔离诊断，减少 C、私有栈、日志和额外载荷的干扰；它不属于产品功能 profile。

私有隔离镜像位于 `private/analysis/minimal-stock-bridge-20261007/`：只在原厂 `image_size` 工作区后增加一条4字节直接分支，再跳原始 `primary_entry`。两个分支的目标、原厂内核字节重建以及 AVB 元数据/载荷哈希均核对通过；完整安装容器为96 MiB。实机已正常进入 Android，启动完成、活动槽 A 和整分区读回均匹配，用户也确认正常开机。它证明本 ROM 的这次基础重包和两分支桥接可用，不作为另一份产品固件发布。

大尺寸对照也已正常启动 Android。它保持67,579,904字节内核包、相同两条分支和同一 FD/APP，入口不执行 C/私有栈/trace/图形或 EDK2；启动完成、活动槽 A 和96 MiB BOOT读回均匹配。该结果排除了这份大尺寸数据包本身作为启动失败原因，调查重点转向选择器实际执行。该对照包不作为产品发布；原厂 Recovery 未改动。

实际生产 `selector.bin`（而非重新编译的测试替身）也在 QEMU 中通过了普通启动和显式 UEFI 请求。仿真日志区全零、没有真实显示保留区，不能证明本机入口时的 MMU、缓存、栈或日志区访问条件。拟将栈移入原 GKI 尾部的方案暂未采用：该处与原内核早期页表重叠。

随后只把 `PianoBootSelectTrace` 首条指令改成 `RET`，不改变选择器其余字节、栈、地址布局或 FD/APP；实际修改后的二进制再次通过 QEMU 两条分支。普通 BOOT 实测仍停在小米 Logo，用户进入原厂 Fastboot 后已恢复原厂 BOOT。这排除了早期日志区访问作为唯一故障原因；不能据此证明实际栈或其他内存访问正常。实际产品元数据通过主机校验，与此前采集的原厂运行时 DTB 地址组合也通过布局检查，但失败启动的真实寄存器状态仍缺失。

恢复后 Android 启动完成，原厂 BOOT 读回一致。本次黑匣子启动计数204，最新失败记录仍是194，没有取得对应的新 ABL 错误；pstore 未发现内核 panic，但不能把保留的上一轮内核日志当成失败候选已进入内核的证据。匹配生产二进制的反汇编中未发现 SIMD/FP 指令，已检查的原 GKI 早期入口自行初始化 x19–x30 和条件标志，没有发现对其传入值的依赖。下一步需要直接区分追加栈写入、C 调用和 FDT 读取，不能以现有仿真结果宣称实机权限正常。

### DTB 对齐读取修复

真实原厂运行时 DTB 的 `/chosen/linux,initrd-end` 值长8字节，位于文件偏移 `0x634`；`/reserved-memory/splash_region/reg` 位于 `0x366c`。两者只按4字节对齐。旧生产二进制把源码的字节拼装函数 `Be64` 优化成 `LDR X8, [X0]`，在 ABL 关闭 MMU 后的早期运行条件下存在对齐异常风险。FDT 格式本身不要求属性按8字节对齐，不能改用要求64位对齐的指针访问。

Makefile 和产品打包器现均使用 `-mstrict-align`，实际产物中的 `Be64` 改为逐字节读取。QEMU 检查开启 `SCTLR_EL1.A`，使用只按4字节对齐的64位 initrd 属性；Makefile 和真实产品打包器两条构建路径均覆盖普通启动与显式 UEFI 请求。编译选项说明见 [Clang 文档](https://clang.llvm.org/docs/ClangCommandLineReference.html)，对齐限制见 [Arm 内存属性说明](https://support.arm.com/documentation/102376/latest/Alignment-and-endianness/Alignment)。

旧生产二进制解析实际原厂 DTB 时，离线捕获 `ESR=0x96000021`、`FAR=0xB5B76634`、`ELR=0xA8001428`，与未对齐属性及64位读取指令完全对应；严格对齐新版通过相同检查。沿用早期日志禁用条件的修复包随后实机正常启动 Android，读回一致。由正式源码默认关闭极早期日志、重新构建的产品包也正常启动；标准 `reboot recovery` 显示原厂 Mi Recovery，并已返回同一产品包的 Android，读回一致。

当前产品候选位于 `private/provisioning/piano-combined-entry-20261007/product-normal/`，完整安装容器 SHA256 为 `9ecf61c36b3715d69195b55004bdd2bd6e458de19d5acca0b0ca48534600f916`。早期探针默认不访问 `0xA3500000`，EDK2 建立内存映射后的运行日志仍保留。对齐修复已解决本次普通启动卡住，但不表示 UEFI 请求消费、Linux/Setup 分发或完整发布包已完成。

显式请求诊断仅修改合体 BOOT 自有请求页：第一份记录为序号1、目标UEFI菜单、匹配当前 APP 的标识及 CRC；第二份保留原“无请求”记录。真实 C 读取器配合原厂运行时 DTB 验证决策从普通启动变为 UEFI。自动消费尚未实现，诊断后必须恢复无请求产品包，不能把该诊断当作日常模块安装。

## 本机重启理由通道

当前 Android `qcom-reboot-reason.ko` 的 build-id 与读取的驱动副本一致。DTB 指向 PMK8550 `sdam@7100/restart@48`，字段是 bit 1 起的 7 位，驱动经 nvmem 写入，并非任意 32 位 MMIO Scratchpad。

| 驱动接受的字符串 | 逻辑值 |
| --- | --- |
| recovery | 0x01 |
| bootloader | 0x02 |
| rtc | 0x03 |
| dm-verity device corrupted / enforcing | 0x04 / 0x05 |
| keys clear | 0x06 |
| ffus / ffuf | 0x40 / 0x41 |
| panic | 0x21 |
| 未匹配字符串 | 0x20 |

当前表中没有 `uefi`。ABL 的 ResetReason 路径读取该原因后调用清除接口，未处理原因会默认 Normal Boot；未找到原始 SDAM 值完整传递给内核选择器的证据。因此给内核加一个字符串映射，也不能单独证明 `reboot uefi` 可用。

DTB 还描述了 IMEM `restart_reason`（base `0x14680000`、offset `0x65c`、4 字节）。其跨重启保留、固件使用、清除与当前 EL1 访问权限均未验证。没有向该位置或其他 PMIC 地址试写魔数。

产品目标仍是 `reboot uefi`。若没有经验证的原生通道，可以由模块提供命令包装：先提交受控、可消费的 UEFI 请求，再执行受支持的正常重启。命令包装与新增内核重启模式是不同实现，文档必须说明；system/recovery 的原厂语义都要保留。

KernelSU 支持 [`webroot` WebUI](https://kernelsu.org/guide/module-webui.html)；本机已确认 KernelSU 4.2.0。官方 Magisk 提供 [`action.sh`](https://topjohnwu.github.io/Magisk/guides.html)，WebUI 可由 [MMRL](https://mmrl.dev/) 等宿主提供。当前模块界面只读预览，尚未绑定可执行的启动请求。

### 显式入口与存储检查

CRC 请求实机出现 Logo 后白屏，未枚举 UEFI Fastboot 或 Linux 调试接口。恢复无请求产品包并确认 Android 正常后，保留控制台的最后记录为 `PIANO_PRODUCT_STORAGE_DISCOVERY status=Security Violation`。它对应 LUN4 可选 FAT／NV 后端，不是 LUN0 的 ESP／root；恢复后的当前 LUN4 快照被旧检查接受，不能用该事后数据还原失败时的具体差异。

产品已经删除运行时历史 GPT 指纹、整表／原 header 比对与固定 Android 启动属性规则，同时解除生产 vendor 输入对 GPT 采集的依赖。标准 GPT 与本项目可写范围检查保留；未配置可写容器时不检查写能力、不隔离整个存储，继续只读 BlockIO／USB。新固件需要重新构建并验证，该源码改动不能继承此前镜像的设备结果。

删除历史检查后的新核心已经编译，并再次通过 BOOT 进行显式请求验证。保留日志明确显示 `owned-entry-absent` 与 `STORAGE_DISCOVERY status=Not Found`，已继续到 USB 初始化；现场 `boot_a` 属性为 `0x0037000000000000`，而历史原表为 `0x0077000000000000`，差异超出旧优先级掩码。下一停点为 `PIANO_PRODUCT_USB_START status=Time out`，USB 清理记录未保留上下文且时钟／电源释放成功。没有发出 Linux 启动请求。

诊断后已恢复 `product-cleanup-normal` 无请求合体 BOOT，Android 启动完成，读回 SHA256 `284008b2af4c043321af6327d309a8922e8f5253589c69c440e915143b27da36`。新核心的普通 Android 路径通过；本轮未重复原厂 Recovery 检查，不能直接继承旧核心的所有设备结果。
