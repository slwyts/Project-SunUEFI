# Piano Iris / FFmpeg stateful decoder

2026-10-08：Iris encoder 已实际产生 640×480 H.264，软件 ffprobe 确认 12 帧。
Decoder 曾在 strace 下输出多帧，但透明 recorder 再次捕获到提前结束。当前不能
将 decoder 标记为可用；新增 FFmpeg 适配还需设备恢复后用同一码流验证。

## 已捕获的失败

一次未改变 ioctl 参数、重试或延时的记录显示：

```text
SOURCE_CHANGE changes=RESOLUTION
DECODER_CMD START cmd=0 -> success
CAPTURE DQBUF flags=0x104041 (ERROR|LAST|timestamp-copy|mapped)
plane[0] bytesused=0 length=462848 data_offset=0
CAPTURE DQBUF -> EPIPE
```

这次没有 STOP。旧 FFmpeg 在 CAPTURE 先开流的情况下，收到初始 source-change
后立即 START，没有先消费 LAST。Iris START 清掉 vb2 的 last-buffer 状态；随后
旧 LAST 被取出，vb2 再次记录 last-buffer-dequeued，下一次 DQBUF 返回 EPIPE。
FFmpeg 将 ERROR 映射为 `FF_DECODE_ERROR_INVALID_BITSTREAM`；这个标签不能单独
证明输入码流坏。

Iris HFI gen2 的空输出错误判据只排除了 LAST，没有排除 PSC_LAST。独立修正应
让空 PSC_LAST 保留边界含义，不能把真正的数据错误或 NO_SHOW 一并屏蔽。用户态
plane 的零 bytesused 不能反推原始 HFI data_size，因为 Iris 完成 ERROR buffer
时也会将 bytesused 清零；若仍有 ERROR，下一份真实证据应含 HFI flags、data_size、
picture_type 和相关 info 值。

## BSP 补丁

通用源码适配位于
[`linux/bsp/patches/ffmpeg/0001-v4l2m2m-follow-stateful-source-change.patch`](../../linux/bsp/patches/ffmpeg/0001-v4l2m2m-follow-stateful-source-change.patch)，
固定版本与 SHA256 位于同目录的 `series.json`。不修改封存内核或 `upstream/`。

补丁按 [Linux stateful decoder 文档](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html)
区分初始元数据解析和后续 DRC：先 OUTPUT，收到初始 SOURCE_CHANGE 后才配置并
开流 CAPTURE；之后的 source-change 必须消费旧 LAST，再以 START 复用足够大的
现有缓冲区，或仅重建 CAPTURE。空 LAST 不交付 AVFrame，有负载的 LAST 仍交付
真实最后一帧。旧帧的 stride、格式、可见裁剪区域在边界前保持旧值。

输入 EOF 与 DRC 分开记录。STOP 仅在 OUTPUT/CAPTURE 同时 streaming 时发送；
DRC 不清掉已接受 STOP 的 drain 状态。EOS 事件不提前结束 CAPTURE；最终 LAST
才决定已开流解码的结束。真实 DQBUF/poll/STOP 错误向调用方返回。

初始元数据尚未建立而输入已 EOF 时，先回收 OUTPUT 并再读取事件。所有输入
都已归还且没有 SOURCE_CHANGE/EOS 时报告 `AVERROR_INVALIDDATA`，不依靠超时
制造 EOF。若驱动永久保留输入且不报事件或错误，客户端仍可能阻塞；这不能靠
假 EOF 掩盖，需设备上的真实失败状态定位。

重建 CAPTURE 仍等待调用方归还旧 AVBufferRef。补丁修正每 buffer 的 context
引用计数和释放回调唤醒顺序，并在 STREAMOFF 失败时立即返回，保留映射。它不
主动解除调用方的帧引用；在同一线程持有旧帧并请求下一帧的调用方，重新分配
路径仍有上游已有的等待限制。可复用缓冲区的路径不进入该等待。

## 可复现的源码副本

```sh
python tools/prepare_ffmpeg_source.py --output build/ffmpeg-source-7.1.5-sunuefi1
```

脚本校验固定 Debian orig archive 和 BSP patch 的 SHA256，只在新的 build 目录
应用标准 patch，并写入 SOURCE.json。版本是 FFmpeg 7.1.5；Debian 参考源码版本
`7:7.1.5-0+deb13u1`，拟定 Debian 包版本
`7:7.1.5-0+deb13u1+sunuefi1`。BSP 补丁不包含发行版打包元数据。

## ARM64 原型与真实验证

最小 CLI 产物放在 `build/ffmpeg-prototype/7.1.5-sunuefi1/`，其中 SOURCE.json、
BUILD.json 和 SHA256SUMS 记录实际源码、补丁、configure 参数和二进制。CLI 的
原型配置不代表 Debian/Arch 完整功能，不能作为最终 rootfs 的 ffmpeg 替代包。

构建使用已有 Debian 13 ARM64 rootfs 的隔离 OverlayFS 副本和 QEMU，调用其 GCC
及 FFmpeg 标准 configure/make；不向 Arch host 安装 Debian 包。配置命令：

```sh
./configure --disable-autodetect --disable-everything --disable-programs \
  --enable-ffmpeg --disable-doc --disable-debug --disable-x86asm --enable-v4l2-m2m \
  --enable-decoder=h264_v4l2m2m,rawvideo \
  --enable-encoder=h264_v4l2m2m,wrapped_avframe,rawvideo \
  --enable-bsf=h264_mp4toannexb --enable-demuxer=h264,rawvideo \
  --enable-muxer=null,rawvideo,h264 --enable-parser=h264 --enable-protocol=file,pipe \
  --enable-filter=buffer,buffersink,null,format,scale,testsrc2 \
  --extra-version=sunuefi1-stateful-prototype
make -j2 ffmpeg
```

设备恢复后，复制 CLI 至 /run，由负责实机的人使用原来已记录的码流运行一次：

```sh
/run/ffmpeg-stateful-prototype -hide_banner -loglevel verbose \
  -c:v h264_v4l2m2m -i /run/piano-codec-test.h264 -an -f null -
```

如需同一轮透明 ioctl 记录，在原有 recorder 前缀后接这条命令，保留其 CMD、
SOURCE_CHANGE、DQBUF planes/flags；另外保存 dmesg 和 CLI 的真实帧数与返回码。
初始应出现 OUTPUT → SOURCE_CHANGE → CAPTURE 开流，后续若有 DRC 则 LAST 在
START 前被消费，最终 EOF 仍有真实 LAST。不能以返回码或 object 编译单独断定
decoder 已工作。编码器源码入口保持原样，共用 buffer 生命周期改动仍需真实
encoder 回归，构建成功不能替代它。

## 标准发行版包的最小接入

Debian 的具体入口是：

```sh
./build.sh ffmpeg --output build/ffmpeg --jobs 2
```

须在 Debian trixie ARM64 builder 中运行。`tools/build_ffmpeg_packages.py` 使用已
固定的 orig archive 和 Debian packaging archive，在副本的 `debian/patches/series`
中记录并通过标准 quilt 应用同一 BSP patch，增加 `+sunuefi1` changelog，保留
完整原有 `debian/rules`，调用 `dpkg-buildpackage -B` 构建相互匹配的九个标准
runtime 包。来源及包 hash 写入 SOURCE.json/SHA256SUMS。该入口已加入，但当前
没有运行正式包构建；`-B` 是架构 binary 构建，不能写成已产出 Debian source 包。

完整 Build-Depends 应先在隔离 builder 核对实际缺口和体积；可选
`--install-dependencies` 使用标准 signed APT 安装 builder 构建依赖，不向 Arch
host 安装 Debian 包。rootfs 的 `--ffmpeg-dir build/ffmpeg/runtime` 仅在显式指定时核对
并安装这组标准包，当前不默认启用；设备上的真实 decoder/encoder 对照仍须完成。

Arch 在固定 FFmpeg 7.1.5 PKGBUILD 的副本里加入同一补丁来源和 SHA256，在
`prepare()` 中应用并提高 pkgrel，保留其功能配置，按标准 makepkg 构建。若发行版
配方已换 FFmpeg 版本，需先核对新源再调整，不能让 7.1.5 补丁带 fuzz 静默套用。
两个发行版都从 BSP 的 `series.json` 读取或核对同一补丁 SHA，分别保留包来源与
最终 package SHA；当前没有构建正式 FFmpeg 包，也没有变更 rootfs 默认安装。
