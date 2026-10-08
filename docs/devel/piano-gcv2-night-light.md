# GCv2 与标准 Night Light 后端

当前实机a8内核没有SM8750 Gamma，Night Light仍不可用。下一默认release已接入[完整后端](../../patches/linux/7.2.9/0006-drm-msm-dpu-gcv2-regdma-backend.patch)、[SM8750 catalog](../../patches/linux/7.2.9/0007-drm-sm8750-gcv2-catalog.patch)和实际产品REGDMA资源overlay；准备器已复现tree `162d8ccbe21d8edbfff1685b8c6ff7ad426dfffd`、commit `281247916d39ff137c48954a762a11a28e976cbf`。正在进行正常完整构建，尚未部署或验证DMA与颜色。没有复用GCv1 AHB实现或强开用户空间设置。

GC地址为 `0x17c0`、窗口 `0x40`、版本 `0x20000`，四个DSPP描述真实PCC/GC能力；只有REGDMA资源、GEM映射及真实队列reset初始化成功后才绑定ops并发布Gamma。实际组合DT追加命名regdma资源，不能仅改上游dtsi。

四个DSPP共用此GCv2 subblock，KMS依次对真实RM资源调用绑定；每个绑定都检查该地址/版本及真正初始化成功的REGDMA对象，随后CRTC按实际setup_gc ops决定是否发布1024项Gamma。原子更新则要求全部assigned mixers都有GC2后端，合并它们的实际DSPP mask，一次提交；只有共同提交成功才给两侧stage GC flush。源码条件符合这个顺序，实际每个DSPP绑定、DMA完成与两侧输出仍须正常启动后查运行状态和画面，不能拿catalog或编译成功代替。

## 已有资源与差异

当前主线DPU已有1024项`GAMMA_LUT`验证、16bit DRM LUT到成对10bit值的转换、双mixer DSPP分配和GC flush bit5；实机a8的SM8750 catalog只有PCC6，GC为空。MDP时钟表虽列出REG_DMA的`0x2bc/bit20`，代码没有REGDMA传输驱动。原厂[GC2绑定](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_dspp.c#L97)只接受REGDMA，初始化失败不回退到旧GC AHB。

离线读取原厂最终FDT确定：REGDMA3独立MMIO为`0x0af80000/0x7000`，DB/SB子块偏移为`0/0x800`，XIN=7；GC2相对DSPP为`0x17c0`。原厂DSPP0偏移`0x55000`与主线`0x54000`并不矛盾：两者MDP映射起点分别为`0x0ae00000/0x0ae01000`，物理地址相同。草案在标准SM8750节点与绑定中加入可选第三段`regdma`，不把它算在mdp范围内。产品实际DT来自原厂base与共享display overlay，并不直接消费此dtsi；另有`linux/dts/piano-regdma-resource.dtso`在实际DPU节点追加该段，保留原mdp/vbif。它已对`vendor/piano-linux/board.dtb`组合并核对整个树，只有目标的reg/reg-names两项变化，没有接默认config或packaging。驱动也识别原厂命名`regdma_phys`；没有该资源就不初始化GC2。

## 最小编程路径

草案从固定[原厂GC2](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_reg_dma_v1_color_proc.c#L1214)与[REGDMA3](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_hw_reg_dma_v1.c#L1055)实现提取CPU命令格式：decode-select `0x18180114`，DSPP选择bits17..20；每通道先清index，再以HW-index opcode `0x30000000`写512个LUT word，最后swap、enable。常规命令1554 words/6216B，保持现有green/blue/red通道顺序。普通地址递增写或GCv1 MMIO循环不是同一个操作。

第一阶段可使用GC2真正的常规10bit模式提供Night Light；高精度BIT2保持关闭。原厂GC2另有128word/channel扩展表，本轮没有猜其位打包，也不因此声称HDR、14bit输出或12bit FRC已完成。[标准KMS颜色属性](https://docs.kernel.org/gpu/drm-kms.html#color-management-properties)仍是目标，不另做屏幕RGB覆盖。

草案已接上以下实际路径，仍需正常整包构建和硬件验证：

1. DPU驱动映射REGDMA命名资源，使用SM8750 catalog的版本、XIN7、REG_DMA clock、read OT及真实8级QoS。初始化必须具备这些ops并完成6个真实队列的reset自清ack；失败不绑定GC2，不发布Gamma。
2. 在真实DPU GPUVM分配并pin一个预分配8KiB WC GEM，明确限定32bit IOVA范围及256B对齐。LUT和256B对齐的最终descriptor共用该映射；没有将CPU物理地址当IOVA。
3. GC命令与实际decode-select空块最终descriptor排入CTL queue0，随后真实CTL软件trigger，轮询完成与错误状态。CTL enum从1开始；v3队列偏移`ctl_idx*0x1000`，done`+0x44/bit3`、clear`+0x48`、reset`+0x54`。全局错误寄存器是v1.1继承的`0x170/0x1b0`，不是v1.0的旧offset。没有合成IRQ或完成结果。
4. 提交位于正常原子commit的可睡眠进程上下文，用mutex串行化；完成后才复用GEM。错误/timeout必须得到reset自清ack才能放掉module/PM/clock vote。外层注销/解绑、KMS uninit和正常machine shutdown入口新增quiesce：先拒新提交，若仍有未知active queue，就在全部依赖尚在时只重试该queue的reset；没有ack不返回该退出入口，不能让后续component/devres/VM回收继续。没有在mutex下flush workqueue；健康退出立即通过。runtime suspend对未知inflight返回EBUSY。错误跳过GC flush并标记可用out-fence错误，后续颜色更新拒绝faulted engine。

本机最终原厂DT没有`qcom,sde-reg-dma-broadcast-disabled`或其他broadcast键。固定catalog用`of_property_read_bool`得到0，color processing直接传入该值；没有依据通用函数猜本机禁用标志。草案按此公开配置一次向实际assigned DSPP mask发送broadcast，成功后才同时stage两边GC flush。硬件广播结果仍待验证。

DT中的`qcom,sde-reg-dma-trigger-off=0x119c`是`trigger_sel_off`，不能误当作队列kickoff地址；固定DB驱动的CTL软件触发是`ctl+0xd4`。草案包含真正descriptor提交代码，但本轮没有运行它或验证硬件完成事件。

## 本轮结果

命令准备、真实传输、DSPP/CRTC/KMS、catalog和外层MSM KMS共7个实际C文件使用完整独立MSM header副本与现有内核ARM64参数做syntax检查，均通过；全16路径补丁对固定b8f07b7e9f9e源码无fuzz dry-run通过。独立QRD源码经正常cpp+dtc编译并读回`reg-names=mdp/vbif/regdma`；DT schema工具本机未安装。没有修改活动树/O、模拟颜色结果或改显示。完整内核link、REGDMA硬件完成/reset和实际标准Night Light仍待Root复核后统一默认构建测试。

固定源、原厂FDT资源、target检查和草案SHA记录在`private/analysis/piano-gcv2-night-light-20261008/`。下一步由Root复核外层quiesce、正常构建和启用实际GC2 catalog及资源overlay，再以默认镜像验证REGDMA、DRM恒等LUT与两侧色温。UI出现开关不能单独说明Gamma已正确编程。

若真实reset持续不ack，退出调用将继续等待，映射、控制器和依赖都不会被收走；这不是有时间上限的成功恢复，仍可能需要平台强制重启。旧草案的“void destroy早返回能拒绝外层退出”判断已删除。实际`msm_drm_uninit`在回调后还会执行component解绑、清dev_private及drm_dev_put，因此局部GEM/module引用不能被当作对这些路径的阻止机制。
