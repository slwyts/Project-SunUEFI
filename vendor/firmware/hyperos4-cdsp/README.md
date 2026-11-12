# Piano CDSP — HyperOS 4

来自官方完整 OTA `OS4.0.6.0.XPYCNXM` 的 `modem` 分区 `/image`，
与 Android 的 `/vendor/firmware_mnt/image` 内容一致。包含 CDSP、CDSP-DTB 和
`cdspr.jsn` 服务描述，保留原始字节和文件名；每个文件的大小及 SHA256
记录在 `manifest.json`。

Linux 使用标准 Qualcomm remoteproc、RPMh 电源域和 FastRPC 接口。
配套的 `piano-dsp-resources.dtso` 提供 SM8750 要求的 CX、MXC、NSP
电源域，并将 ADSP／CDSP 的 XO 时钟接到主线 RPMh 时钟提供者。
固件加载到 Linux 使用的保留内存，不写入 Android 固件分区。

当前固件通过加载和签名认证，但 CDSP 仍未发出启动就绪通知；
remoteproc 返回超时，计算服务尚不可用。共享同步内存的权限配置已按
原厂驱动补齐，后续仍需定位固件启动等待的资源或通信条件。

这些文件是 Qualcomm／小米的专有固件，不适用 SunUEFI 的源码许可。
未取得额外再分发授权；保留原权利人的权利，仅用于该设备的硬件运行。
