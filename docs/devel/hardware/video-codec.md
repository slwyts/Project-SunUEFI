# Piano 视频编解码

视频编解码使用 Linux 的 Qualcomm Iris 驱动和标准 V4L2 接口。FFmpeg 的适配放在
[`linux/bsp/patches/ffmpeg/`](../../../linux/bsp/patches/ffmpeg/)，四个发行版的构建都会安装带补丁的完整软件包。

## 软件包与构建

| 系统 | FFmpeg | 构建入口 |
| --- | --- | --- |
| Debian sid | 9.0.2（Debian 源版本 `7:9.0.2-6`），保留 Debian 打包配置 | `tools/build_ffmpeg_packages.py` |
| SteamOS | 8.1.1，保持 Valve 8.x 库接口和编译选项 | `tools/build_arch_ffmpeg.py` |
| Nyarch | 9.0.2，保持当前 Arch 库接口 | `tools/build_arch_ffmpeg.py` |

Arch 构建器读取目标系统已安装包的编译选项、依赖和版本信息，保留完整编解码功能，
重新生成 pacman 包。不会把 FFmpeg 9 降级成 7，也不会用精简调试程序覆盖系统软件。
软件包和源码来源记录在对应发行版构建目录的 `packages/ffmpeg/SOURCE.json`。

Debian 的包在 sid ARM64 环境编译；其他主机提供同一目标 ABI 的根目录，由构建器复制到隔离环境：

```sh
sudo ./build.sh ffmpeg --sysroot build/distros/debian-sid/rootfs --output build/ffmpeg --jobs 8
```

构建器验证签名 sid Sources、`.dsc` 及全部归档哈希，使用未删减的 Debian `debian/rules`，同时构建标准与 extra codec 变体。运行集合包含独立的 `ffplay` 和七个 FFmpeg 9 库包，不再包含已移除的 `libpostproc`。Debian 的 orig 归档与 Nyarch 使用的 FFmpeg 9 官方归档相同，复用 `patches/ffmpeg/9.0.2/series.json`，不复制一套 Debian 专用补丁。Debian 打包版本单独记录在同目录 `debian.json`。

单独准备共用源码：

```sh
python tools/prepare_ffmpeg_source.py \
  --series patches/ffmpeg/9.0.2/series.json \
  --output build/ffmpeg-source-9
```

源码归档及补丁固定 SHA256，补丁禁止模糊匹配，不修改 `upstream/`。
FFmpeg 8 和 9 分别使用官方 8.1.1、9.0.2 归档。上游 9.0.2 及 Debian 的补丁系列尚未包含这些 V4L2 修正，因此继续重建标准发行版软件包。

SteamOS 和 Nyarch 的包缓存同时核对版本、补丁系列与二进制包哈希。
修改系列后重新构建完整包；复用缓存时重新安装该包，不保留被基础软件源替换的版本。

Debian 的 `SOURCE.json` 同时绑定签名 sid 源身份、目标已安装运行库 ABI、原始 `debian/rules`／`debian/control`、补丁和二进制哈希。源码或 ABI 改变不能命中旧包；同目录的 `--resume` 校验这些身份后复用完整 `.changes` 或继续未完成对象，不重下已验哈希的归档、不重新配置完整软件包。

## 解码流程修正

依据 [Linux stateful decoder 文档](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html)，
先向 OUTPUT 队列提交压缩数据，收到初始 SOURCE_CHANGE 后才配置 CAPTURE 队列。
后者负责交付解码后的画面。

视频中途改变分辨率时，必须先取完旧画面以及标有 LAST 的最后一个缓冲区，再恢复解码。
容量足够时复用缓冲区，否则只重建 CAPTURE，保留 OUTPUT 中尚未处理的数据。
空 LAST 表示切换边界，不应生成画面；LAST 携带有效数据时仍要交付最后一帧。

输入文件结束与分辨率切换分别处理。STOP 只在两个队列都运行时发送，EOS 通知
不会直接丢弃尚未取出的画面。非阻塞 DQEVENT 返回 ENOENT 表示事件队列为空，
不作为解码错误；其他 I/O 错误仍返回给调用者。

## 已验证与剩余问题

FFmpeg 7 的适配和 Nyarch 完整 FFmpeg 9 软件包，均已在 Iris 硬件上完成
640×480 H.264 样本的 12 帧解码和 12 帧编码。这个结果不能推导为所有格式、
分辨率和播放器都已正常。

640×480 → 1280×720 的连续码流暴露了两处问题：

1. **旧画面引用阻塞重新分配。** 播放器可以保留上一帧，同时等待下一帧；
   原来的 FFmpeg 却要等旧引用全部释放，才创建新缓冲区。默认补丁使用
   驱动声明的 `V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS` 能力，将旧映射保留到最后一个引用释放，
   同时建立新一代缓冲区，不提前释放播放器仍在使用的内存。
2. **Iris 重复发送结束标记。** 驱动把正常的空 PSC_LAST 标成 ERROR，并在处理
   ERROR 时跳过 LAST 状态更新。随后新队列又收到一次 LAST，使播放器提前结束。
   内核补丁修正了空 PSC_LAST 的判断，并让带错误的最后一帧也正确结束旧队列。

FFmpeg 缓冲区补丁与 Iris 的 PSC_LAST 修正均已纳入默认构建；内核改动收在
[`0004-media.patch`](../../../patches/linux/7.3/0004-media.patch) 中。
FFmpeg 7 和 9 在实机完成 640×480 → 1280×720 → 640×480 的连续切换，
36 帧全部解码、零解码错误，两版也各完成 12 帧 H.264 硬件编码。

SteamOS 的完整 FFmpeg 8.1.1 包复用同一份 NV12 行复制、序列头合并和未启动编码器
退出修正。输入帧只提供可见高度，驱动的对齐高度仅用于目标缓冲区布局，
不能按后者读取源图像。实机已完成 1280×720 的 30 帧 H.264 编码和全部 30 帧硬解。
裸码流的帧数检查使用 `-fps_mode passthrough`，避免输出同步策略丢弃时间戳相同的画面。

同一内核补丁在取得核心锁之前先等待中断线程结束，
修复实机发现的驱动退出死锁。设备已完成模块卸载、重新加载和正常重启。

Iris 的 V4L2 帧率协商在设置 OUTPUT 时同步 CAPTURE，单独设置 CAPTURE
不反向改 OUTPUT。开流中不能应用的编码帧率变更返回 `EBUSY`，原值保留。
平板已核对协商值、超限拒绝和停流后重新设置。

后摄 3840×2160 画面可由 Iris 编成 H.264，与内置麦克风的 AAC 一起直接写 MP4。
实测 8 秒的文件能完整解码 423 帧，平均约 53 fps；码流报告 H.264 High Level 5.0。
3840×2160@60 至少需要 H.264 Level 5.2。两套 FFmpeg 补丁共用同一改动，
让 `h264_v4l2m2m -level:v 52` 在开流前把 Level 5.2 请求交给 V4L2。
不指定级别时保留驱动默认值；驱动拒绝指定值时报告错误。平板以 720p 测试图案
编码时，读回默认 Level 5.0、显式 Level 5.2，两段各成功解码 30 帧。
3840×2160 的合成输入也完整读回 120 帧、60 fps 和 Level 5.2。
这只核对编码和封装，不包含相机采集、实时帧间隔或长时间音画同步。

FFmpeg 的 `h264_v4l2m2m` 随第一包提供 H.264 的 SPS/PPS，而直写 Matroska
在首包之前就需要这组参数。TS 可在码流内接收它们，录好后可以通过 `-c copy` 转封。

后续验证必须确认完整输出切换前后的全部帧，不能仅看进程退出码。其他编码格式、
容器播放时序以及播放器自动选择硬解的行为，也需要分别检查。
