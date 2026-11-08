# GCv2／IGCv5 与标准色彩接口

默认内核通过高通 REGDMA 提交硬件色彩查找表，提供标准 `GAMMA_LUT`、`DEGAMMA_LUT`，与现有 `CTM` 配合。GNOME 夜灯可用；这组接口是 HDR 处理的基础，完整 HDR 和 12-bit FRC 仍未实现。

## 硬件资源

实现对照 [MiCode 固定显示源码](https://github.com/MiCode/vendor_opensource_display-drivers/tree/aa06fd1757c28dce96fbe4e04d4530ec21b52aac)和原厂最终设备树：REGDMA3 的 MMIO 为 `0x0af80000/0x7000`，XIN 为 7，GCv2 相对 DSPP 的偏移为 `0x17c0`。启动设备树通过 [piano-regdma-resource.dtso](../../../linux/dts/piano-regdma-resource.dtso)提供命名资源。

驱动使用正常 DPU GPUVM 中的 16 KiB GEM 缓冲区与 IOVA，不把 CPU 物理地址当作 DMA 地址。LUT 命令与最终描述符共享映射；描述符按照命令长度对齐。CTL 只使用 catalog 中存在的实例，寄存器范围在访问前核对。

## 曲线提交

- GCv2 暴露均匀 65536 项 `GAMMA_LUT`，映射为覆盖完整输入范围的 1024 个主采样点和暗部的 256 个扩展点。扩展点的输入步长约为 `1/65536`，输出仍为十位。两组采样通过同一个 HW-index 端口依次写入，再交换表并开启高精度模式；旧版 GC 仍使用 1024 项接口。
- IGCv5 使用 1025 项均匀采样的标准 Degamma 曲线，每色保留 16 位值，映射为原厂的 257 个普通点和 128 个暗部点，通过 LUTBUS 写入。高精度扩展已启用，Linux 7.3 进一步按原厂配置打开 IGC 内部抖动；它不改变 DSC 传输位深。
- 两类曲线组成一批完整颜色状态，提交到当前分配的两路 DSPP。只有共同完成才更新相应 flush；关闭曲线使用旁路状态。
- DMA 队列失败时保留既有的生命周期和退出规则，不能释放仍可能被硬件使用的内存。

IGCv5 实现位于[显示补丁](../../../patches/linux/7.3/0002-display.patch)，GCv2 暗部扩展位于 [`0052`](../../../patches/linux/7.3/0052-dpu-gcv2-dark-range.patch)。设备上普通 GC 配合 IGC 的恒等、非线性和旁路恢复均取得 REGDMA 完成，双 DSPP mask 为 `0x60000`。加入 GC 暗部扩展后，完整组合命令为 2532 个 word，最终描述符对齐到 10240 字节，仍在 16 KiB 缓冲区内；暗部扩展尚未启动验证。

## 桌面兼容

Mutter 的空 Gamma 恢复曾触发空数组访问，修复位于 [0001-kms-empty-gamma-bypass.patch](../../../linux/desktops/gnome/patches/mutter/0001-kms-empty-gamma-bypass.patch)，Debian 和 Nyarch 构建都包含它。

内屏没有 EDID，GNOME 的颜色配置使用 colord 设备建立未校准的 sRGB 默认配置，保留用户自定义配置。它用于正常的 SDR 和夜灯联动，不声明额外 HDR 能力。色深和原厂处理的边界见[显示位深与 HDR](display-depth.md)。
