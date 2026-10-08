# 相机手动控制

相机服务提供曝光、增益、白平衡和后摄对焦的手动控制接口。正常 runtime 构建会同时生成相机服务和 `piano-camera-ctl` 客户端，rootfs 构建会将它们一起安装；手动控制复用已有相机服务的硬件连接和帧循环。

`tools/patches/piano-camerad-manual-controls.patch` 应用在固定上游 `debian-piano a75f8c5d5fa099d65c171ac839c2e3bb6c63ec45` 的 CCM、启动/稳定 3A、帧完整性三补丁之后。准确前置 camera 源码 SHA256 是 `0a96e7d4aa2dd23bd1f49cbdfae37578327f47d3a9118464cbdfd924bf9f6f1d`。它只修改已有 camerad，不改变帧格式和缓冲路径。CLI 的唯一正式源码在 `linux/userspace/piano-camera-ctl.c`，后续正常构建安装到相机 BSP 的 `/usr/bin/piano-camera-ctl`；不要把源码文件作为 rootfs overlay 安装。

现有 camerad 在自己的 poll 循环处理 `/run/piano-camerad/control.sock`，socket 为 `root:video 0660`，目录为 `root:video 0750`。没有额外服务或相机硬件 owner。先用正常相机应用开流；未开流时所有命令返回 `ENODEV`，不会暗中启动相机。

```sh
piano-camera-ctl caps rear
piano-camera-ctl get rear
piano-camera-ctl set rear ae manual
piano-camera-ctl set rear exposure INTEGER
piano-camera-ctl set rear analog-gain INTEGER
piano-camera-ctl set rear digital-gain INTEGER
piano-camera-ctl set rear awb manual
piano-camera-ctl set rear red-balance INTEGER
piano-camera-ctl set rear blue-balance INTEGER
piano-camera-ctl set rear af manual
piano-camera-ctl set rear focus INTEGER
```

将 `INTEGER` 换成 `caps` 实际允许的数值。`rear` 可换为 `front`，但前摄没有 VCM，不支持 `af` 和 `focus`。对应的 `set … ae|awb|af auto` 恢复自动控制。

| 控件 | 呈现单位 | 硬件接口 |
| --- | --- | --- |
| exposure | sensor lines | 传感器 `V4L2_CID_EXPOSURE` |
| analog-gain | 驱动原生整数 | 传感器 `V4L2_CID_ANALOGUE_GAIN` |
| digital-gain | Q10，1024 为 1× | TFE `V4L2_CID_DIGITAL_GAIN` |
| red-balance / blue-balance | Q10，1024 为 1× | TFE 红/蓝平衡控件 |
| focus | 驱动原生 DAC 值 | 后摄 `V4L2_CID_FOCUS_ABSOLUTE` |

`caps` 用 `VIDIOC_QUERY_EXT_CTRL` 返回实际 min/max/step/default/flags，数值写入前再次查询；不硬编码某个模式的曝光上限。`get` 用 `VIDIOC_G_CTRL` 返回驱动控制状态，不宣称读取物理寄存器。没有校准依据的 ISO、Kelvin、物理焦距或对焦距离换算。

AE、AWB、AF 各自拥有自动/手动状态；数值写入必须先把对应域切为 manual，自动算法不会覆盖该域。模式切换先读回该域的现有控件，恢复 auto 从这个状态继续。手动曝光仍保留帧统计供后摄 AF 判断稳定性。每个数值请求只写一个白名单控件，随后读回；失败保留 errno，读回不一致同时给出 requested/value 并返回错误。多个传感器/TFE 控件不构成硬件原子事务；回复丢失或读回失败后应先 `get`，再决定是否重试。

两个正常 producer 顺序应用这份补丁、继续编译同一 camerad，并另编 CLI；stage 的完整文件集合包含客户端。CLI 的 main 使用 argc/argv，不复用 camerad 的 void-main wrapper。二进制不存入 Git，无需新增 systemd unit。

2026-10-09 已使用完成的 Debian rootfs 作为 ARM64 sysroot 编译完整 runtime。相机服务与客户端随后随 `7.2.9-piano-gnome-gc8bf8df4d2ca` 部署；客户端 SHA256 为 `a2039e1415fea9a7d2640fc5a330faa3d848e0fdc780f99c6586232975de1097`。正常构建与 stage 都包含这两个静态 AArch64 程序。

实机后摄开流期间，`caps/get` 成功查询控件；AE 切为 manual 后，将当前曝光写回并读到相同值，再恢复 auto，所有步骤均成功。该模式的曝光范围为 8–4266 行、模拟增益 1–64、数字增益 1024–16383（Q10）、对焦 DAC 0–1023。前摄查询也成功，并正确报告没有对焦马达。这些范围对应当前模式，应用应重新查询，不能写成所有模式的常量。测试只丢弃帧数据，没有保存图像或改变永久参数。

Snapshot 首次远程打开时可能停在概览中。该版本等待窗口获得焦点后才开始相机发现；退出 GNOME 概览后，前摄可进入正常预览。不要仅凭窗口的加载动画判断相机服务没有工作。

这套接口还不是原厂相机专业模式。原厂 Android 的小米扩展公布后摄 ISO 50–6400，QTI 视频表另有 3840×2160/60 fps；前摄标准输出包含 1920×1080/30 fps。当前 Linux 相机输出仍固定为 1920×1440/30 fps，不能因原厂能力表存在这些条目就直接宣布支持 4K60。ISO 换算需要传感器增益标定，快门秒数需要所选模式的真实积分行时钟，Kelvin 色温需要白平衡和色彩标定。测光模式、实际裁切变焦、闪光灯同步和实体隐私指示灯也需要分别接入真实硬件路径。
