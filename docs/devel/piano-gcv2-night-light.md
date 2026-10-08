# GCv2 与标准 Night Light 后端

当前bde3版本已通过普通BOOT进入GNOME，实际CRTC报告1024项Gamma，DSI连接器报告固定10bpc。一次恒等曲线更新已取得真实REGDMA提交和完成：CTL1、DSPP mask `0x60000`、IOVA `0x1000`、1554 words，完成状态 `0x8`。此前a589的CTL越界已修正，当前未再出现该异常。

Mutter 48.7读取未设置的Gamma时返回三个空数组。旧版通过SetCrtcGamma恢复空数组时，size0对象进入缩放并读取空数组，导致GNOME Shell退出。`48.7-0+deb13u1+sunuefi1` 已安装到平板，并通过真实的“保存当前1024项曲线 → 设置空数组旁路 → 恢复原曲线”检查；同一用户会话保持运行。修复源码在 `linux/desktops/gnome/patches/mutter/0001-kms-empty-gamma-bypass.patch`，默认root构建也使用这一包。

这块DSI面板没有EDID，原版Mutter因此没有为其建立默认颜色配置。第二个补丁通过实际colord设备ID建立未校准的标准sRGB配置，保留用户已有配置，未伪造EDID。实机已经自动关联该配置，Gamma为三条各1024项的曲线。标准 `NightLightSupported` 为true，但暖色在屏幕两侧的实际输出仍待观察；恒等DMA完成或软件读回不能代替这一结果，也不能证明HDR已支持。

注销记录需要区分两件事：日志时间2026-10-08 14:11:07 UTC，旧版空Gamma恢复触发 `status=11/SEGV`；15:26:30 UTC，安装sunuefi1后主动重启GDM，显示会话正常退出并自动重新登录。后者的新GNOME Shell进程为PID2406、用户会话为15，后续未记录新的Shell崩溃，相机服务保持运行；这些日志不能支持相机导致注销的结论。平板的NTP未同步，墙钟存在偏差，不能将上述日志时间直接用于精确对齐用户操作；同一启动中的顺序和uptime仍可用于比较。对应记录在 `private/provisioning/recovery-priority-20261008/camera-logout-timeline-20261009.txt`，旧异常在同目录的 `bde3-session-journal.txt`。

GC地址为 `0x17c0`、窗口 `0x40`、版本 `0x20000`，四个DSPP通过共用`sblk`描述PCC/GC；当前`dpu_dspp_cfg`没有独立features成员。只有REGDMA资源、GEM映射及真实队列reset初始化成功后才绑定ops并发布Gamma。实际组合DT追加命名regdma资源，不能仅改上游dtsi。

四个DSPP共用此GCv2 subblock，KMS依次对真实RM资源调用绑定；每个绑定都检查该地址/版本及真正初始化成功的REGDMA对象，随后CRTC按实际setup_gc ops决定是否发布1024项Gamma。原子更新则要求全部assigned mixers都有GC2后端，合并它们的实际DSPP mask，一次提交；只有共同提交成功才给两侧stage GC flush。源码条件符合这个顺序，实际每个DSPP绑定、DMA完成与两侧输出仍须正常启动后查运行状态和画面，不能拿catalog或编译成功代替。

## 已有资源与差异

当前主线DPU已有1024项`GAMMA_LUT`验证、16bit DRM LUT到成对10bit值的转换、双mixer DSPP分配和GC flush bit5；实机a8的SM8750 catalog只有PCC6，GC为空。MDP时钟表虽列出REG_DMA的`0x2bc/bit20`，代码没有REGDMA传输驱动。原厂[GC2绑定](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_dspp.c#L97)只接受REGDMA，初始化失败不回退到旧GC AHB。

离线读取原厂最终FDT确定：REGDMA3独立MMIO为`0x0af80000/0x7000`，DB/SB子块偏移为`0/0x800`，XIN=7；GC2相对DSPP为`0x17c0`。原厂DSPP0偏移`0x55000`与主线`0x54000`并不矛盾：两者MDP映射起点分别为`0x0ae00000/0x0ae01000`，物理地址相同。后端在标准SM8750节点与绑定中加入可选第三段`regdma`，不把它算在mdp范围内。产品实际DT来自原厂base与共享display overlay，并不直接消费此dtsi；`linux/dts/piano-regdma-resource.dtso`在实际DPU节点追加该段，保留原mdp/vbif。它已对`vendor/piano-linux/board.dtb`组合并核对整个树，只有目标的reg/reg-names两项变化，现已接入下一默认release。驱动也识别原厂命名`regdma_phys`；没有该资源就不初始化GC2。

## 最小编程路径

草案从固定[原厂GC2](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_reg_dma_v1_color_proc.c#L1214)与[REGDMA3](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_reg_dma_v1.c#L1055)实现提取CPU命令格式：decode-select `0x18180114`，DSPP选择bits17..20；每通道先清index，再以HW-index opcode `0x30000000`写512个LUT word，最后swap、enable。常规命令1554 words/6216B，保持现有green/blue/red通道顺序。普通地址递增写或GCv1 MMIO循环不是同一个操作。

第一阶段可使用GC2真正的常规10bit模式提供Night Light；高精度BIT2保持关闭。原厂GC2另有128word/channel扩展表，本轮没有猜其位打包，也不因此声称HDR、14bit输出或12bit FRC已完成。[标准KMS颜色属性](https://docs.kernel.org/gpu/drm-kms.html#color-management-properties)仍是目标，不另做屏幕RGB覆盖。

草案已接上以下实际路径，仍需正常整包构建和硬件验证：

1. DPU驱动映射REGDMA命名资源，使用SM8750 catalog的版本、XIN7、REG_DMA clock、read OT及真实8级QoS。初始化只接受catalog列出的CTL，分配GEM前核对每个reset寄存器的offset+sizeof(u32)完整落在资源内，再逐个做reset自清ack；失败不绑定GC2，不发布Gamma。SM8750实际为编号1..6，不能以CTL_MAX代替该表。
2. 在真实DPU GPUVM分配并pin一个预分配8KiB WC GEM，明确限定32bit IOVA范围及256B对齐。LUT和256B对齐的最终descriptor共用该映射；没有将CPU物理地址当IOVA。
3. GC命令与实际decode-select空块最终descriptor排入CTL queue0，随后真实CTL软件trigger，轮询完成与错误状态。CTL enum从1开始；v3队列偏移`ctl_idx*0x1000`，done`+0x44/bit3`、clear`+0x48`、reset`+0x54`。全局错误寄存器是v1.1继承的`0x170/0x1b0`，不是v1.0的旧offset。没有合成IRQ或完成结果。
4. 提交位于正常原子commit的可睡眠进程上下文，用mutex串行化；完成后才复用GEM。错误/timeout必须得到reset自清ack才能放掉module/PM/clock vote。外层注销/解绑、KMS uninit和正常machine shutdown入口新增quiesce：先拒新提交，若仍有未知active queue，就在全部依赖尚在时只重试该queue的reset；没有ack不返回该退出入口，不能让后续component/devres/VM回收继续。没有在mutex下flush workqueue；健康退出立即通过。runtime suspend对未知inflight返回EBUSY。错误跳过GC flush并标记可用out-fence错误，后续颜色更新拒绝faulted engine。

本机最终原厂DT没有`qcom,sde-reg-dma-broadcast-disabled`或其他broadcast键。固定catalog用`of_property_read_bool`得到0，color processing直接传入该值；没有依据通用函数猜本机禁用标志。草案按此公开配置一次向实际assigned DSPP mask发送broadcast，成功后才同时stage两边GC flush。硬件广播结果仍待验证。

DT中的`qcom,sde-reg-dma-trigger-off=0x119c`是`trigger_sel_off`，不能误当作队列kickoff地址；固定DB驱动的CTL软件触发是`ctl+0xd4`。a589首次运行停在初始化队列越界，尚未到自有descriptor提交，未验证硬件DMA完成事件。

## 本轮结果

此前7个后端C文件的target syntax及补丁应用检查通过，281完整构建仍捕到后来catalog小补丁使用不存在`features`成员；0007已修正，a589随后正常完整编译。实机又捕到本次CTL队列越界，因此编译通过没有证明硬件初始化完整。日志来源是`private/provisioning/recovery-priority-20261008/a589-boot-kernel.txt:1741`。新0006 SHA256为`60768e66de73e32d0f61aad302b01d8111bb10120ca91b435064001b93533587`，独立单对象和队列地址边界结果在`private/analysis/piano-regdma-catalog-queues-20261008/result.json`。未修改已完成a589源码/O或操作设备；新硬件初始化、DMA完成和标准Night Light仍需正常构建后验证。

固定源、原厂FDT资源、target检查和SHA记录在`private/analysis/piano-gcv2-night-light-20261008/`。修正后的0007 SHA256为`38689e09ba31b55b077794e6e7e5b67fa67dee7b9c4c12f996c9bf28eeb441f1`；`catalog-fix-281/result.json`记录实际281全部MSM头/源的独立副本及单对象编译结果。先前7个文件syntax检查没有覆盖后来加入的错误DSPP成员，不能代替这次真实catalog编译。后续按[最小实机检查步骤](piano-gcv2-validation.md)验证REGDMA、DRM恒等LUT与两侧色温。UI出现开关不能单独说明Gamma已正确编程。

若真实reset持续不ack，退出调用将继续等待，映射、控制器和依赖都不会被收走；这不是有时间上限的成功恢复，仍可能需要平台强制重启。旧草案的“void destroy早返回能拒绝外层退出”判断已删除。实际`msm_drm_uninit`在回调后还会执行component解绑、清dev_private及drm_dev_put，因此局部GEM/module引用不能被当作对这些路径的阻止机制。

实机a589通过普通BOOT进入内核并出现Linux USB，但显示绑定在 `queue_reset+0x50` 异常：访问位于映射末端之外的第七个队列（`0x7054`），ESR为`0x96000047`，为CPU level3 translation fault，并非已提交DMA后的SMMU fault。通用 `CTL_MAX` 枚举包含八个控制器，SM8750 catalog只列六个，REGDMA资源为0x7000。此轮没有发布可用Gamma，也没有进入GNOME。已恢复ESP中的上一可用a8启动文件；异常后正常重启未完成，需要长按恢复。后续修正只遍历实际catalog的CTL编号，并在初始化和每次提交前检查编号及最后一个u32寄存器是否在真实映射范围内，不能靠扩大MMIO区域掩盖越界。原始日志位于本机 `private/provisioning/recovery-priority-20261008/a589-boot-kernel.txt`。

修正后的bde3源码为 `bde3f2e9e9156875e08c81a4df75a369f5518d8a`，tree为 `16fe3d5fa0d8d2da8e66e69fde394d7850fb7f23`，已经完成构建并部署。它同时包含固定DSC位深报告和标准torch档位转换；a589失败产物不作为可用版本推荐。下一版本95b72加入下述DSI分期修正，尚在构建。

本轮实机结果保存在 `private/provisioning/recovery-priority-20261008/bde3-drm.json`、`bde3-identity-probe.json`、`bde3-session-journal.txt`。内核的真实提交和完成时间为659.136439/659.136542秒，桌面恢复空数组时在662秒附近退出；无本轮SMMU或REGDMA timeout。标准显示恢复、暖色两侧输出及正常Night Light关开仍需分别验证。

bde3同次DPMS关屏两秒再开屏仍出现 `dsi_err_worker status=4`，两次D-Bus调用返回不能证明物理链路恢复。随后正常重启已恢复GNOME。slave-first变更不足以解决此问题；status4是软件FIFO分类，尚无raw FIFO子位，不能确定具体underflow/overflow。暂不部署锁屏/睡眠策略，不重复切刷新率，下一步对照原厂video-enable与面板reset/DCS/PPS分期。

[DSI分期修正](../../patches/linux/7.2.9/0011-dsi-bridge-video-phases.patch)使用标准bridge四阶段：pre-enable准备PHY/clock/command，panel自行prepare完成DCS/PPS，enable再开启video；disable先停video并保留command，panel unprepare之后post-disable再清IRQ/clock/PHY。已有从链路先关时钟的顺序保留，未改变复位脉冲、off延时或刷新策略。IRQ日志复用已经读取的FIFO寄存器并限速输出，不新增MMIO读取。源码 `95b72a5bd7eb7fb31b2b385faa915edeee5a31cf`、tree `70a83a85970d3c6b50afe8886b4f6b855bd32d6a` 已完成完整内核、匹配模块及成品打包，并从ESP正常启动；GNOME用户会话、相机和触屏服务已运行。

这次唯一的两秒DPMS OFF/ON仍失败。请求在uptime237.23–241.40秒均返回成功，CRTC之后也报告ACTIVE=1，但关屏时主host0在video=0状态读到raw FIFO `0x99991090`、worker status `0xd/0xc`；开屏时从host1在video=1状态持续读到 `0xaaaa1010`、`0xdddd1011`、`0xeeee1011`、`0xcccc1011`、worker status4。因此四阶段调整不足以解决恢复，不能拿回调返回或ACTIVE属性称为显示恢复成功。原始记录在 `private/provisioning/recovery-priority-20261008/dsi-95b72-dpms-kernel.txt`，软件状态在同目录的 `dsi-95b72-dpms-result.txt` 和 `dsi-95b72-drm-after.json`。已将ESP启动文件恢复到bde3并发出正常重启，没有连续重复关屏检查。

原始位定义进一步确认：关屏时置位的是CMD_MDP欠流bit7和四路HS欠流，不是CMD_DMA欠流bit10；开屏后从链路还置位VID_MDP溢出bit0。下一份源码 `ea3ddda5a6a7bc4c650459953fd9e527e5f871ca` 只修正视频面板的待机状态：保留controller ENABLE，清除VID/CMD时序位及对应done中断，由已有xfer_prepare/restore在真正发送命令时临时启用CMD及CMD_DMA_DONE。命令模式面板的常开CMD行为保持。该差异符合固定原厂按面板类型选择时序引擎的实现，但物理恢复效果仍未验证；此次没有调整复位、延时、刷新率或电源。补丁已合并回同一份0011及默认源码准备流程，不增加独占功能的产品profile。

ea3d完整增量构建约66秒完成，匹配root/ESP及安装脚本也已生成；实机普通重启后GNOME、触屏、相机和蓝牙服务正常启动。在uptime133.36–137.53秒的一次相同OFF/ON中，关屏阶段未再观察到主链路CMD_MDP欠流或worker `0xd/0xc`，但开屏后host1仍报告 `0xeeee1011/0xdddd1011`、worker status4，包含VID_MDP溢出。因此待机修正只解决了本次观察到的一类错误，双DSI恢复仍未完成。日志在 `dsi-idle-ea3d-dpms-kernel.txt`、`dsi-idle-ea3d-dpms-result.txt` 和 `dsi-idle-ea3d-drm-after.json`，目录与前述相同。已恢复bde3启动文件并正常重启；不重复原操作，下一步只核对从链路PLL/PHY恢复与真实视频启用顺序。

普通IGCv5的[标准DEGAMMA候选](../../patches/linux/7.2.9/drafts/0012-drm-msm-dpu-igcv5-degamma.patch)已经准备，尚未加入默认补丁序列。它提供257项16bit RGB输入曲线，与现有GC在同一REGDMA缓冲区中合并提交，保留实际DSPP分配，并在共同完成后更新两类flush。七个ARM64对象及实际CPU命令打包的长度、端点和容量检查已通过；最大普通双LUT为7800B，末描述符位于7936，完整落在8KiB缓冲区内。没有模拟DMA完成，也没有实机验证此候选。高精度扩展采样域仍不明确，因此没有启用高精度、抖动、HDR或12bit FRC。固定MiCode来源、对象和边界结果保存在 `private/analysis/piano-igcv5-degamma-20261009/`。
