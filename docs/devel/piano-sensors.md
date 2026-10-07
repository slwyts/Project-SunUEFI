# Piano 传感器

发布构建使用 [piano-sensors](https://github.com/blu-sharky/piano-sensors/tree/65a92202db65ad13493b43fbf15d6abb3b7bcf92) 的固定提交 `65a92202db65ad13493b43fbf15d6abb3b7bcf92`，源码位于 `upstream/piano-sensors-current`。它提供标准 FastRPC/SSC 用户空间、设备数据导入和 iio-sensor-proxy 启动顺序。本地补丁增加 vendor 文件服务配置的只读导入，并在 PD 前加载 socinfo；没有另写 daemon。

## 构建与包来源

在文档规定的 Debian trixie root ARM64 builder 中运行：

```sh
./build.sh sources
./build.sh sensors
./build.sh release-rootfs
```

`sensors` 默认写入 `build/sensors`，先要求源码 HEAD 与固定提交一致、工作区干净，将源码复制到 `build/sensors-workspace`，在副本应用 `config/release.json` 列出的 `patches/piano-sensors/0001-import-vendor-reg-config.patch`，再调用副本中的上游 `scripts/build-sensors-debs.sh`。上游 checkout 保持干净。`all` 和公开 `debian-gnome` CI 的相关顺序均为 Mesa → sensors → release-rootfs。

官方 CI 的 runtime 集合为六个包：

| 包 | 当前固定源码对应的版本 |
| --- | --- |
| `piano-sensors` | 上游 CI 为 `5`，本地补丁构建为 `5+sunuefi1`，架构 `all` |
| `fastrpc-support`、`libfastrpc1` | `1.0.7-2~bpo13+1`，架构 `arm64` |
| `libssc2`、`libssc-bin` | `0.4.4-2+piano1`，架构 `arm64` |
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

`system_heap` 暂不作为产品服务依赖。2026-10-08 的冷启动在启用该 heap 后出现反向 RPC `0xe`（FastRPC 的 `AEE_EBADPARM`，不能直接解释为内核 `-EFAULT`），此前 fastrpc ioctl fallback 能读 JSON。临时 namespace 对照使 fallback 返回真实加速度，但没有作为产品方案。两条分配/映射路径的差异仍需定位：FastRPC 使用首个 SG DMA 地址描述一个连续区间，identity domain 的非连续 SG 和 DMA32 高物理页的 bounce/sync 都需核对。后续需修真实 DMA/IOMMU 映射；只取消预加载不能覆盖摄像头等其他功能启用 heap 的情况。

本地 native 包已用 Debian trixie ARM64 sysroot、QEMU 与标准 `dpkg-buildpackage -b -us -uc -aarm64` 构建为 `5+sunuefi1`，保留 socinfo，去掉 system_heap 预加载。`build/sensors-patched/runtime` 复用官方 CI 的另外五包；父目录的 SOURCE、SHA256SUMS 与 BUILD.json 记录真实源码/补丁/包哈希及构建来源，可作为 `--sensors-dir` 输入。该包构建通过不代表上述 DMA 映射问题已解决。

## 运行状态

构建集成和包检查不证明实机读数可用。当前说明未包含已通过的运行实测；还需在实际启动后核对 import 来源、ADSP/sensors PD 日志、四类 SSC 读数，以及桌面旋转和亮度行为。可先读取服务日志：

```sh
systemctl status piano-adsp piano-sensors-import adsprpcd-sensorspd iio-sensor-proxy
journalctl -b -u piano-adsp -u piano-sensors-import -u adsprpcd-sensorspd -u iio-sensor-proxy
```
