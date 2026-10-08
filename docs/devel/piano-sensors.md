# Piano 传感器

发布构建使用 [piano-sensors](https://github.com/blu-sharky/piano-sensors/tree/65a92202db65ad13493b43fbf15d6abb3b7bcf92) 的固定提交 `65a92202db65ad13493b43fbf15d6abb3b7bcf92`，源码位于 `upstream/piano-sensors-current`。它提供标准 FastRPC/SSC 用户空间、设备数据导入和 iio-sensor-proxy 启动顺序。本地补丁增加 vendor 文件服务配置的只读导入，并在 PD 前加载 socinfo；没有另写 daemon。

## 构建与包来源

在文档规定的 Debian trixie root ARM64 builder 中运行：

```sh
./build.sh sources
./build.sh sensors
./build.sh release-rootfs
```

`sensors` 默认写入 `build/sensors`，先要求源码 HEAD 与固定提交一致、工作区干净，将源码复制到 `build/sensors-workspace`，在副本应用 `config/release.json` 列出的导入配置和 libssc 属性类型补丁，再调用副本中的上游 `scripts/build-sensors-debs.sh`。上游 checkout 保持干净。`all` 和公开 `debian-gnome` CI 的相关顺序均为 Mesa → sensors → release-rootfs。

官方 CI 的 runtime 集合为六个包：

| 包 | 当前固定源码对应的版本 |
| --- | --- |
| `piano-sensors` | 上游 CI 为 `5`，本地补丁构建为 `5+sunuefi1`，架构 `all` |
| `fastrpc-support`、`libfastrpc1` | `1.0.7-2~bpo13+1`，架构 `arm64` |
| `libssc2`、`libssc-bin` | `0.4.4-2+piano1+sunuefi2`，架构 `arm64` |
| `iio-sensor-proxy` | `3.9-1+piano1`，架构 `arm64` |

FastRPC 包使用上游脚本固定校验值的 Debian 二进制包；libssc 与 iio-sensor-proxy 从固定 `.dsc` 校验值的 Debian 源包重建。版本与六包集合会记录到发布 manifest。

`tools/build_release_rootfs.py --sensors-dir` 默认使用 `build/sensors/runtime`。六个 `.deb` 需直接位于该目录；`SOURCE` 和 `SHA256SUMS` 可一同放在它的父目录或目录内。顶层构建成功后写入的 `SOURCE` 为：

```json
{
  "source_url": "https://github.com/blu-sharky/piano-sensors.git",
  "source_commit": "65a92202db65ad13493b43fbf15d6abb3b7bcf92",
  "patches": [
    {
      "path": "patches/piano-sensors/0001-import-vendor-reg-config.patch",
      "sha256": "<构建时记录的补丁 SHA-256>"
    },
    {
      "path": "patches/piano-sensors/0002-fix-libssc-property-types.patch",
      "sha256": "<构建时记录的补丁 SHA-256>"
    },
    {
      "path": "patches/piano-sensors/0003-libssc-raw-vector-reports.patch",
      "sha256": "<构建时记录的补丁 SHA-256>"
    }
  ]
}
```

rootfs builder 核对来源字段、补丁清单与实际补丁哈希、每个 runtime 包的 SHA-256、包名、架构和 SSC 包的 `+piano` 版本，并要求六包齐全及 `piano-sensors=5+sunuefi1`。原上游 CI 的无补丁 `5` 仍可用于对照，但不满足更新后的发布构建输入。上游 `SHA256SUMS` 同时包含 `all/` 与 `runtime/` 条目；CI 下载只保留 runtime 包时，builder 按对应 runtime 条目逐个校验，不要求开发包或调试包。校验文件中的路径以它所在目录为基准，解压后需保持这层关系。

这些包通过普通、保留签名检查的 APT 安装，以解析依赖。当前发布 rootfs 基线还需要 `erofs-utils`、`libqmi-glib5`、`libqmi-proxy`、`libqrtr-glib0`；APT 同时解析 `libmbim-glib4`、`libmbim-proxy`，其余运行库沿用已有 rootfs 的 ABI。离线安装也必须备齐这些依赖。

## 启动与设备数据

启动顺序为 `piano-adsp.service` 和 `piano-sensors-import.service` → `adsprpcd-sensorspd.service` → `iio-sensor-proxy.service`。上游 sensors PD 服务已依赖 import，并排在 ADSP 之后；rootfs builder 增加 `Requires=piano-adsp.service` drop-in。补丁中的标准 `ExecStartPre` 加载 `socinfo`，然后沿用 `fastrpc` 与设备节点等待；模块名是 `socinfo`。`adsprpcd sensorspd` 启动后，上游脚本最多等待 60 秒，依次查询 accelerometer、light、proximity、compass。超时仍继续启动并记录缺失类型，所以服务 active 不代表四类传感器均正常。

上游包 mask `adsprpcd.service`（根 PD）与 `adsprpcd_audiopd.service`，并用 `/etc/udev/rules.d/59-fastrpc-remoteproc.rules` 覆盖 FastRPC 自动启动所有 DSP 的规则。只由现有 ADSP 服务启动 ADSP，再运行 sensors PD；当前音频 GPR 路径不依赖 audio PD。

首次启动时，import 从这台平板自己的 ODM 配置、vendor 文件服务配置和 persist 注册表/校准数据生成 `/var/lib/piano-sensors` 中的副本。vendor 的 `/etc/sensors/sns_reg_config` 通过 read-only device-mapper 与 `dump.erofs` 读取，以临时文件检查非空后原子保存为 `/var/lib/piano-sensors/vendor/sns_reg_config`，并链接至 `/vendor/etc/sensors/sns_reg_config`。ODM 也使用只读映射；persist 保留 `ro,noload` 挂载，ADSP 后续更新写入导入副本。构建不复制 `private/captures` 中的用户数据，也不随镜像分发这些配置或校准文件。

importer 默认读取 `super` 的 primary metadata slot 0，并选择 `odm_a`、`vendor_a`。部署前需与当前 ROM 的逻辑分区布局核对；`PIANO_SENSORS_ODM_NAME` / `PIANO_SENSORS_VENDOR_NAME` 可覆盖名称，`PIANO_SENSORS_ODM` / `PIANO_SENSORS_VENDOR` 可直接提供已有只读 EROFS 设备或镜像，`PIANO_SENSORS_VENDOR_LINK` 可覆盖链接位置。名称变量不改变 metadata slot 0 的读取方式。不能仅凭包安装成功判断它导入了当前 ROM 的配置。

本机原厂 vendor 文本为 version 1，包含 input/output/property 路径，没有其他设备示例中的 hw_platform 映射；persist 下同名文件则是已解析 JSON 的时间戳缓存，二者不能互换。现场日志已确认 SSC QMI service 400 可通信，`registry` SUID 可发现，但 `accel` 查询返回空 UID。补齐配置后的实际读数仍需正常重启后的验证；不增加等待时间或添加固定平台值来掩盖空 UID。

`system_heap` 暂不作为产品服务依赖。2026-10-08 的冷启动启用该 heap 后，反向 RPC 返回 `0xe`；此前 fastrpc ioctl fallback 能读 JSON。已确认的两条路径差异如下：

* [FastRPC 1.0.7 rpcmem](https://github.com/qualcomm/fastrpc/blob/v1.0.7/src/rpcmem_linux.c) 优先打开 `/dev/dma_heap/system`；只有打开失败才改用 fastrpc 分配 ioctl。前者导入一般页组成的 DMA-BUF，后者来自 compute-cb 的 `dma_alloc_coherent`，受其 DMA mask 约束。
* 当前 [fastrpc 映射](../../upstream/linux-piano/drivers/misc/fastrpc.c) 只取首段 SG DMA 地址，并用一个连续地址区间传给 DSP。identity/direct domain 下非连续物理 SG 不满足这一表示；translated DMA domain 可由 IOMMU 提供连续 IOVA。
* listener 最小 buffer 为4 KiB，所以还需核对单段情况。DMA32 的高物理页可能经 SWIOTLB bounce；system_heap 提供 CPU/device 同步回调，但当前 rpcmem/listener 未调用 DMA-BUF CPU 同步接口，fastrpc 也没有相应 SG 同步。未取得失败 buffer 的实际物理/DMA地址、SG段数和同步状态前，不把 bounce 作为已确认根因。
* [AEE 错误定义](https://github.com/qualcomm/fastrpc/blob/v1.0.7/inc/AEEStdErr.h) 中 `0xe` 是 `AEE_EBADPARM`、`0x14` 是 `AEE_EUNSUPPORTED`；`mod_table` 日志不能直接解释为内核 `-EFAULT` 或 SMMU fault。

`patches/linux/7.2.9/0002-fastrpc-sm8750-translated-dma.patch` 为实际 SM8750 compute-cb 选择标准 DMA domain，保留其他设备的现有策略；只取消预加载不能覆盖摄像头等其他功能启用 heap 的情况。临时 namespace 只用于早先的一次分配路径对照，没有作为产品方案。

本地 native 包已用 Debian trixie ARM64 sysroot、QEMU 与标准 `dpkg-buildpackage -b -us -uc -aarm64` 构建为 `5+sunuefi1`，保留 socinfo，去掉 system_heap 预加载。`build/sensors-patched/runtime` 复用官方 CI 的另外五包；父目录的 SOURCE、SHA256SUMS 与 BUILD.json 记录真实源码/补丁/包哈希及构建来源，可作为 `--sensors-dir` 输入。该包构建通过不代表上述 DMA 映射问题已解决。

## 运行状态

2026-10-08，当前a8+CPUCP版本通过普通BOOT启动后，标准SSC客户端已实际返回
陀螺仪三轴、罗盘连续读数和光线0lux。陀螺仪在现有libssc中已有实现，CLI帮助
漏列但 `ssccli --sensor gyroscope --timeout 3` 可以使用；其输出中的`m/s`
不是已核验的角速度单位，后续修正须依据协议，不据该字符串换算测值。

从本机原厂`sensors.qsh.so`核对数据类型后，通过正常libssc SUID查询确认
`rgb`（STK3BCX）、`cct_front`（STK3BCX前置色温）、`ambient_light_back`
及`ambient_light_back_strm`（SIP1328后置光线）、`flicker`（SIP1328）可用。
前置色温、后置光线和防闪烁均已限时订阅并正常关闭，收到原始SSC报告。
前两者使用1025标准浮点载荷；防闪烁为769独立格式，不把未知字段强标成
lux、Kelvin或Hz。后置事件包含六个浮点分量，原厂handler原样转交Android。
这证明通道和数据到达，尚未完成全部语义、桌面策略或校准验证。

实际库另有三个属性声明错误：`available`/`sample-rate`/`stream-type`注册为
字符串，getter却写boolean/float/uint，读取触发GLib类型检查失败。标准软件包
已通过 Debian quilt 修正，并使用原有 `debian/rules` 构建标准 ARM64 库、CLI、开发包和 GIR。默认构建已选择此补丁；只重建库可用 `./build.sh sensors build/libssc-only --libssc-only`，但这两包不能单独代替发布需要的完整六包。修正不修改传感器载荷或用常量补值。该问题与QMI能返回真实测量
是不同层次；不能把声明修正直接称为硬件校准完成。

本机已安装同源的 `libssc2` 和 `libssc-bin` 新版本，并重启标准 SensorProxy。
实际属性读取返回 `available=true`、`sample-rate=26.0`、`stream-type=0`，
陀螺仪仍能返回三轴数据，stderr 不再出现上述类型断言。完整六包输入已通过
真实 rootfs 消费函数检查；此时下一份 root 镜像尚未生成，不把当前平板安装
结果等同于新镜像已部署。GIR 在同一源码构建中生成，库的 SONAME 仍为 2。

后续 `+sunuefi2` 增加[公开向量接口及限时客户端](piano-ssc-vector.md)，
默认发布配置已选择0003补丁，平板上前置色温、后置光线和防闪烁订阅均收到
真实数据并正常关闭。它保留全部分量，不向未知数据指定物理单位，不新增
daemon或改变GNOME的既有光线接口。标准包依赖阻止新客户端混用旧库。

本轮原始记录位于`private/provisioning/recovery-priority-20261008/`的
`a8-ssc-gyroscope.txt`、`a8-ssc-vendor-inventory.jsonl`及
`a8-ssc-vendor-payloads.jsonl`。原厂ELF只做静态读取，没有在Linux执行。

2026-10-08，`7.2.9-piano-gnome-g086a94c4529d` 经普通 BOOT 启动，六个实际 compute-cb 的 `iommu_group/type` 均为 `DMA`。标准 `5+sunuefi1` 服务运行，`/dev/dma_heap/system` 可见，`InaccessiblePaths`、`RootDirectory`、`RootImage` 均为空，未使用此前隐藏 heap 的临时方案。

SensorProxy 的 HasAccelerometer/Light/Proximity/Compass 均为 true；一次三秒采样实际返回加速度约 `(8.49, -0.08, 4.75) m/s²` 和罗盘约 `195°`，LightLevel 为 `153`。这证明本次正常 heap 条件下 SSC 数据读取已工作，不能扩大为所有 buffer、全部传感器或待机恢复都已验证。桌面自动旋转仍需现场核对。一次早先的 g086 启动没有形成可用调试接口；本轮成功也没有解释那次失联原因。

GNOME 的实际 DBus 订阅包括 gnome-shell、gsd-power 和 gsd-media-keys，`ambient-enabled=true`。一次自然光照变化中 LightLevel从138降到124，KTZ8866背光及实际背光从1256降到1135，未手动写亮度，确认标准桌面自动亮度已有响应；完整范围尚未测试。自动旋转当前未由 Mutter 管理：键盘套提供鼠标/触控板且没有 tablet-mode switch，GNOME48.7按标准策略退出 touch-mode。摘下键盘后的实际旋转仍待确认，不强制绕过这项策略。

本地原始记录位于 `private/analysis/recover-dma-20261008-131916/g086-state.json`、`g086-sensor-samples.txt` 和 `g086-live-journal.txt`。可读取标准服务日志：

```sh
systemctl status piano-adsp piano-sensors-import adsprpcd-sensorspd iio-sensor-proxy
journalctl -b -u piano-adsp -u piano-sensors-import -u adsprpcd-sensorspd -u iio-sensor-proxy
```
