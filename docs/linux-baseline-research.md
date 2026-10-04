# Piano Linux 基线核查

核查日期：2026-10-05（Asia/Shanghai）。这里记录源码、Git ref 和迁移建议；没有执行设备操作，也没有把参考项目的硬件报告计作本项目验收。

## 已固定的来源

| 来源 | ref | 实际提交 | 用途 |
| --- | --- | --- | --- |
| `blu-sharky/linux-piano` | `piano-7.2.6` | `7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8` | piano 功能基线 |
| 同仓库 | `master` | `500df175a7f9e6bc1a9c328590ca5150f84f9ff0` | 与 vanilla `v7.2.6` peeled commit 相同 |
| `torvalds/linux` | `master` | `7704c4c5bb127673b4f0ead839919db573559e38` | next 的主线基线；Makefile 为 `7.3.0-rc5` |
| Linux stable | `linux-7.2.y` / `v7.2.9` peeled | `5fce161649b4d779d1b76d9fcd52dc77779774b8` | 当时最新 stable 更新 |
| `blu-sharky/debian-piano` | `main` | `e4004f5d5f08f79433b61a892c8b4f783c4c2e37` | 最终 overlay、initramfs 和驱动加载顺序 |
| `blu-sharky/xiaomi-piano-linux` | `main` | `f852795b9ae400e31fbda388a6150a658d98d5df` | 参考构建编排与说明 |

ref 由 `git ls-remote` 实时核对；release 信息另核对 [kernel.org releases.json](https://www.kernel.org/releases.json)。kernel.org 当时公布的 mainline release 为 `7.3-rc5`，stable 为 `7.2.9`。移动 ref 后续会变化，构建清单应保存完整 SHA。

`piano-7.2.6` 在 vanilla `v7.2.6` 上增加 60 个提交、115 个最终变更文件：[固定提交比较](https://github.com/blu-sharky/linux-piano/compare/500df175a7f9e6bc1a9c328590ca5150f84f9ff0...7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8)。内容包括 piano DTS/config、NT36532 面板/触摸、Adreno 830、Peach v2 WLAN/BT、电源排序、AudioReach/FS19xx、WN8030 键盘套、MCA 和 SC8541，以及多轮 PCIe/PHY 诊断和实验改动。

首次调查没有发现可直接复用的本地主线 Linux Git 仓库。`upstream/reference-kernel` 是 Xiaomi 下游参考源码目录，没有 `.git`，不能作为主线历史来源。随后主任务准备的独立内核仓库位于 `/home/slwyts/linux-piano`；内核源码和构建不应混入 UEFI 平台目录。

## 静态 piano DTS 不是参考项目实际启动的最终 DT

固定内核提交的 [`sm8750-xiaomi-piano.dts`](https://github.com/blu-sharky/linux-piano/blob/7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts) 仍包含早期假设：

- 面板是 `novatek,nt37801`，注释称 AMOLED、1440×3200；这不能代表本机已核对的 3200×2136 双 DSI NT36532 LCD。
- BT/PMU 仍写 WCN7850，而实际参考 overlay 使用 `qcom,wcn7861-pmu` / `qcom,wcn7861-bt`。
- 扬声器仍有猜测的 WSA884x SoundWire 节点；参考音频 overlay 已改用 I²C 上四个 FourSemi FS19xx 与 LPAIF TDM。
- 多项电源轨、GPIO 与 UFS 配置仍标记为从 MTP 继承、待设备验证。

因此，`make piano_defconfig Image dtbs` 产物存在只证明编译成功。这个 DTS 不能直接替代参考项目的运行时 DTB。

参考项目的实际路线是 **原厂 vendor DTB + 自有 DTBO**，保留许多原厂节点，再加入主线节点。固定来源与路径为：

| 环节 | `debian-piano` 路径 | 作用 |
| --- | --- | --- |
| 最终 overlay 入口 | [`boot/dtbo-piano-cpufreq.dts`](https://github.com/blu-sharky/debian-piano/blob/e4004f5d5f08f79433b61a892c8b4f783c4c2e37/boot/dtbo-piano-cpufreq.dts) | SCMI CPUCP mailbox 修正，并包含键盘/视频/音频等前置 overlay |
| 显示/GPU | [`boot/dtbo-piano-display.dts`](https://github.com/blu-sharky/debian-piano/blob/e4004f5d5f08f79433b61a892c8b4f783c4c2e37/boot/dtbo-piano-display.dts) | NT36532 双 DSI、KTZ8866、MDSS、GPU/GMU 与第二套主线 SMMU 节点 |
| 子系统/音频 | `boot/dtbo-piano-subsys.dts`、`boot/dtbo-piano-audio.dts` | ADSP 依赖、GPR 服务端口、FS19xx 与音频 DMA 流 |
| WLAN/BT | `boot/dtbo-piano-wlanbt.dts` | Peach v2 / Brahma 的实际节点与 firmware-name |
| DTBO 构建 | [`scripts/build-dtbo.py`](https://github.com/blu-sharky/debian-piano/blob/e4004f5d5f08f79433b61a892c8b4f783c4c2e37/scripts/build-dtbo.py) | 编译 overlay 与 Android DTBO table |
| 参考 boot 封装 | `scripts/build-test-bootimg.sh` | 原厂 ABL 合成协议；不等同自主 UEFI 的 DTB 装载 |
| early userspace | [`initramfs/rootfs-init`](https://github.com/blu-sharky/debian-piano/blob/e4004f5d5f08f79433b61a892c8b4f783c4c2e37/initramfs/rootfs-init) | 先 USB NCM/SSH，再 SMMU 流、UFS 和 userdata root |
| 显示分阶段接管 | [`rootfs/overlay/usr/lib/piano/display-start`](https://github.com/blu-sharky/debian-piano/blob/e4004f5d5f08f79433b61a892c8b4f783c4c2e37/rootfs/overlay/usr/lib/piano/display-start) | SMMU → GPU 时钟 → MSM → backlight → DSI panel |
| 禁止提前 autoload | `rootfs/overlay/etc/modprobe.d/piano.conf` | 确保受控 DMA 驱动按服务顺序加载 |

include 主链为 `cpufreq → keyboard → video → audio → subsys → display → rootfs → wlanbt → touch-v2`；触摸以下还包含 USB/provider 前置改动。umbrella 的 [`scripts/build-rootfs-image.sh`](https://github.com/blu-sharky/xiaomi-piano-linux/blob/f852795b9ae400e31fbda388a6150a658d98d5df/scripts/build-rootfs-image.sh) 选择 cpufreq 入口。

原始 overlay 对 stock base 的 labels/phandles、1-cell `/soc` bus、原厂 reserved-memory 和 ABL phandle 修正有具体依赖。自主 UEFI 应在电脑上将经过核对的 base/overlay 合成完整 DTB，再反编译核验实际节点、phandles、interrupts、memory-region 与地址范围；不能把 Android DTBO table 当作完整 FDT 传给 Linux。最终 board DTS 可逐步把核对后的节点收敛到内核仓库，保留值来源和实际测试状态。

`build-dtbo.py` 实际只执行 C 预处理、`dtc -@`，再加 Android DTBO table/entry 头；它没有合成完整 DTB。参考最底层 USB/bringup overlay 内嵌了原厂 dtbo entry 0。若本项目输入是已经合成过 stock overlay 的运行时 FDT，不能把整份参考 overlay 再应用一遍；应提取自有新增/修正 fragments，核对 base 的 `__symbols__`、overlay 的 `__fixups__` 及最后的 phandle 解析，或者转成完整自主 board DTS。

`rootfs-init` 直接写 SMMU bypass stream matches 和 UFS CLKREF，并要求 `root=PARTLABEL=userdata`。这些是参考环境的实验顺序，不能整体复制到自主 UEFI 的 RAM initramfs。UEFI 自有 UFS/USB 上下文应先由各自 owner 停止 DMA 并完成交接；Linux 随后接管的行为需另验收。

## 已上游与仍需携带的补丁

对 115 个变更文件逐字节比较上述固定 mainline/stable 提交：18 个与 mainline 完全相同、22 个在 mainline/stable 都没有、另外 75 个与 mainline不同；没有一个最终变更文件与 stable `7.2.9` 完全相同。**文件相同是迁移线索，不能证明 squash 内每个补丁已上游，也不能证明实机行为相同。**

另外对以下原始提交做了 GitHub compare ancestry 核查；它们确实是 `torvalds/master` 固定提交的祖先：

| 内容 | 已验证的原始主线提交 | next 处理 |
| --- | --- | --- |
| MIPI DSI 一包多 DSC slice | [`ffa88de8ddb64067df49e4d9f253d09a9c247059`](https://github.com/torvalds/linux/commit/ffa88de8ddb64067df49e4d9f253d09a9c247059) | 使用主线实现 |
| MSM DSI `slice_per_pkt > 1` | [`ce73a5db44e3d5f9c0c061f0868ae209b59605f1`](https://github.com/torvalds/linux/commit/ce73a5db44e3d5f9c0c061f0868ae209b59605f1) | 使用主线实现，保留 piano 特有 DSC/时序修正 |
| q6v5 handover IRQ one-shot | [`bb7c5d6f5b41d192fa81ce404e463f5d3ce70cb3`](https://github.com/torvalds/linux/commit/bb7c5d6f5b41d192fa81ce404e463f5d3ce70cb3) | 使用主线实现 |
| PAS attach 不 enable handover IRQ | [`34b8b2d78b6276dc2dc4ebc06625a39956f266e4`](https://github.com/torvalds/linux/commit/34b8b2d78b6276dc2dc4ebc06625a39956f266e4) | 使用主线实现，检查 piano 剩余 remoteproc 差异 |
| q6apm TDM DAI operations | `4d084017589312a0da2bec3474ef2035c5f4a407` | 使用主线实现 |
| QAIF clock IDs rename | `c41ac86802fc0a22a886915a43bcad2e8d482b02` | 使用主线实现 |
| TDM slot 缺失/无效区分 | `0a9e00d5ebdfcf460902f463e765f737d3fe935e` | 使用主线实现 |
| sc8280xp TDM hw_params 错误处理 | `8593dc5f052e791748eaa76397ad95b9e32edac3` | 使用主线实现 |

stable `7.2.9` 的 DSI 源码仍只支持每包一个 slice，q6v5/PAS 仍缺少上述 handover 管理实现；不能因为 next 已具备就从 stable 丢弃 backport。

18 个与 mainline 完全一致的文件是：

```text
Documentation/devicetree/bindings/sound/qcom,q6apm-lpass-dais.yaml
Documentation/devicetree/bindings/sound/qcom,q6dsp-lpass-ports.yaml
drivers/clk/qcom/common.c
drivers/clk/qcom/gxclkctl-kaanapali.c
drivers/remoteproc/qcom_q6v5.h
include/drm/drm_mipi_dsi.h
include/dt-bindings/sound/qcom,q6dsp-lpass-ports.h
sound/soc/qcom/common.c
sound/soc/qcom/common.h
sound/soc/qcom/qdsp6/q6afe-dai.c
sound/soc/qcom/qdsp6/q6apm-dai.c
sound/soc/qcom/qdsp6/q6dsp-lpass-clocks.c
sound/soc/qcom/qdsp6/q6dsp-lpass-ports.c
sound/soc/qcom/qdsp6/q6prm.c
sound/soc/qcom/qdsp6/q6routing.c
sound/soc/qcom/sdm845.c
sound/soc/qcom/sm8250.c
sound/soc/qcom/storm.c
```

NT36532 原始 binding/driver `1c523f0a9301` / `47b823940e38` 和 SM8750 GPU DT `f695831a7af1` 在 subsystem 树有提交，但不是本次 `torvalds/master` 的祖先。主线原始文件中没有 NT36532 driver/binding，也没有 Adreno 830 `0x44050001` catalog entry 或 SM8750 GPUCC/IOMMU nodes。因此 next 仍需移植这些内容及 piano 适配，不能称为已有完整 A830/piano 支持。

明确仍需携带或重新移植：piano board/config、NT36532 panel/touch 与固件装载、双 KTZ8866、Peach v2/Brahma power sequence、piano SMMU handover、FS19xx/MCA、WN8030 与 SC8541。其余混合文件要按 hunk/功能核查；不要整段覆盖更新后的通用驱动。[音频 squash](https://github.com/blu-sharky/linux-piano/commit/2f6c29345edf991b04ce15d2231e6060412335aa) 混合了大量通用 backport 和 piano 改动，next 需要拆开；[显示/GPU squash](https://github.com/blu-sharky/linux-piano/commit/5c2c95fddbe9c29720b7b4e84119357358e9f0cb) 亦同。

## 独立仓库和分支建议

`/home/slwyts/linux-piano` 作为独立 kernel repository，UEFI 主仓库仅保存精确引用和自身 config/build/profile 工具。

- `origin`：自主维护的 linux-piano 仓库，发布 `stable`、`next` 与版本 tag。
- `blu-sharky`：参考设备移植来源，保留 `piano-7.2.6` 的完整来源 SHA，不在该来源 ref 上修改。
- `upstream`：`https://github.com/torvalds/linux.git`，跟踪主线 master；它与本项目的 `next` 分支是不同概念。
- `linux-stable`：`https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`，获取受支持 stable 系列 bugfix。
- `stable` 初始接在固定 `piano-7.2.6` 之后，添加自主 RAM config/board/交接改动。通过独立提交更新到 7.2.9；是否称“本机已稳定”由硬件验收决定。
- `next` 从固定主线 SHA 起步，按依赖移植剩余 platform patch，删除确认由主线替代的 backport。迁移期间可用 integration 分支，保留可启动 stable。

每次产物记录 base SHA、patch 来源、最终 config、DTB/Image/modules/initramfs 的 hash、编译器版本及硬件测试状态。发布分支保留提交历史；整理可重放 patch series 时在专用分支进行。

## 最小自主 UEFI RAM profile

参考 [`piano_defconfig`](https://github.com/blu-sharky/linux-piano/blob/7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8/arch/arm64/configs/piano_defconfig) 有 1970 行、强制 `rdinit=/beaconinit`、`panic=-1` 和大量无关平台。参考 `piano_rootfs.config` 又强制 `/pianoinit` 与 userdata root，并假设 stock SMMU bypass。先合并自主 fragment，运行 `olddefconfig`，再检查实际 `.config`：

| 类别 | 第一次 RAM 启动所需配置/行为 |
| --- | --- |
| 启动 ABI | `ARM64`、`OF`、`EFI=y`；ARM64 EFI 会自动选择 `EFI_STUB` 和 `EFI_GENERIC_STUB`；原生 handoff 另记录 EL/FDT/initrd/entry |
| 命令行 | `CMDLINE=""`、`CMDLINE_FORCE=n`；有 fallback 时使用 `CMDLINE_FROM_BOOTLOADER=y`；采用本项目独立 RAM init，而不是参考 userdata init |
| RAM userspace | `BLK_DEV_INITRD`、实际压缩对应的 `RD_GZIP`/`RD_LZ4`、`BINFMT_ELF`、`DEVTMPFS`、`PROC_FS`、`SYSFS`、`TMPFS` |
| 可见输出 | `PRINTK`、`TTY`、`VT`、`FRAMEBUFFER_CONSOLE`、`DRM_SIMPLEDRM`、`SYSFB_SIMPLEFB`；仅在实际 GOP/FDT framebuffer 交接正确后成立 |
| 回收日志 | `PSTORE=y`、`PSTORE_CONSOLE=y`、`PSTORE_RAM=y`；ramoops reserved-memory 与本机保留区核对；保留 `IKCONFIG`、`IKCONFIG_PROC`、`KALLSYMS`、`DEBUG_FS`、`DYNAMIC_DEBUG` |
| 初期 DMA 范围 | UFS/PHY 可禁用，或保持模块且不放入初期 initramfs；MSM、SMMU、remoteproc、radio、touch 按后续阶段受控加载 |
| USB 调试阶段 | DWC3/QCOM glue、实际 PHY/clock/ICC providers、configfs ACM/NCM 与匹配 modules；不能仅凭 UDC/driver bind 宣称电脑已枚举 |
| 恢复 | kernel `panic=10`；PID 1 明确记录阶段并在 180 秒后重启。UEFI timer 在 ExitBootServices 后不能代替 OS 恢复路径 |

第一次 init 只挂载 RAM/虚拟文件系统，打印 uname、cmdline、FDT model/compatible、EFI 状态、内存、PID 1、module/deferred-probe/IOMMU 状态与阶段标记。不要加载原机 6.6 vendor `.ko` 到 7.2/7.3；每个分支的模块都由对应源码/config 编译，并保留 modules.dep/firmware 清单。EFI capsule loader、pstore block/efivars backend 与 gadget mass-storage 对这个 RAM profile 没有必要。

设备树首先保留本机已核对的 RAM、reserved-memory、PSCI、GIC、CPU 与当前 framebuffer；只对下一测试阶段启用所需节点。显示接管时要核对 UEFI 仍活跃的 display stream 和 Linux qcom-SMMU 的保留策略，不能全局重置 SMMU，不能把参考 bypass 等同本项目自有页表交接。

## 当前证据与下一步

本轮直接只读核对了 `private/analysis/ramlog-test-16/linux.txt`：第 2 行为原机 `6.6.118-android15-8`，1183 行为 `SUNUEFI_RAM_INIT BEGIN pid=1`，1184–1190 行为 RAM/虚拟文件系统挂载，日志有 71 条 module OK，2052 行为 `USB_NO_CONTROLLER`，2060–2061 行记录 180 秒超时和重启。日志同时有 module BTF mismatch 与 SPMI warning，不能把 module OK 当作全部驱动工作正常。这是 **原机 6.6 GKI + 同机 vendor modules + RAM PID 1** 的证据，不是这个 7.2.6 源码基线的测试。已验证的 UEFI 只读 UFS 与 DMA 能力也不能证明主线 Linux 驱动接管成功。

参考项目的 commit 文本报告过 GNOME/msm-kms、144/120/90/60 Hz、freedreno 和其他设备测试；同一项目部分文档还停留在“built offline / not validated”的旧状态。这些属于参考来源报告，需由本项目的固定镜像、完整日志和实际输入/屏幕/电脑观察重新验收。

优先顺序：固定 stable 源码与自主 RAM config → 准备经过反编译校验的最终 DTB → 主机编译/产物 hash/EFI PE 与 ARM64 header 检查 → RAM PID 1 与恢复 → USB 枚举和 PC 往返 → SMMU/显示/触摸分阶段接管 → 独立下一主线版本验证。先保持每阶段可回收的日志和 known-good 镜像，再扩展 UFS 与完整发行版；next 的编译成功始终单独标注。
