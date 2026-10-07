# Piano 传感器

发布构建使用 [piano-sensors](https://github.com/blu-sharky/piano-sensors/tree/65a92202db65ad13493b43fbf15d6abb3b7bcf92) 的固定提交 `65a92202db65ad13493b43fbf15d6abb3b7bcf92`，源码位于 `upstream/piano-sensors-current`。它提供标准 FastRPC/SSC 用户空间、设备数据导入和 iio-sensor-proxy 启动顺序。本项目只补充 ADSP 服务依赖，不另写传感器 daemon。

## 构建与包来源

在文档规定的 Debian trixie root ARM64 builder 中运行：

```sh
./build.sh sources
./build.sh sensors
./build.sh release-rootfs
```

`sensors` 默认写入 `build/sensors`，先要求源码 HEAD 与固定提交一致、工作区干净，再调用上游 `scripts/build-sensors-debs.sh`。`all` 和公开 `debian-gnome` CI 的相关顺序均为 Mesa → sensors → release-rootfs。

官方 CI 的 runtime 集合为六个包：

| 包 | 当前固定源码对应的版本 |
| --- | --- |
| `piano-sensors` | `5`，架构 `all` |
| `fastrpc-support`、`libfastrpc1` | `1.0.7-2~bpo13+1`，架构 `arm64` |
| `libssc2`、`libssc-bin` | `0.4.4-2+piano1`，架构 `arm64` |
| `iio-sensor-proxy` | `3.9-1+piano1`，架构 `arm64` |

FastRPC 包使用上游脚本固定校验值的 Debian 二进制包；libssc 与 iio-sensor-proxy 从固定 `.dsc` 校验值的 Debian 源包重建。版本与六包集合会记录到发布 manifest。

`tools/build_release_rootfs.py --sensors-dir` 默认使用 `build/sensors/runtime`。六个 `.deb` 需直接位于该目录；`SOURCE` 和 `SHA256SUMS` 可一同放在它的父目录或目录内。顶层构建成功后写入的 `SOURCE` 为：

```json
{
  "source_url": "https://github.com/blu-sharky/piano-sensors.git",
  "source_commit": "65a92202db65ad13493b43fbf15d6abb3b7bcf92"
}
```

rootfs builder 核对这两个字段、每个实际 runtime 包的 SHA-256、包名、架构和 SSC 包的 `+piano` 版本，并要求六包齐全。上游 `SHA256SUMS` 同时包含 `all/` 与 `runtime/` 条目；CI 下载只保留 runtime 包时，builder 仍按对应 runtime 条目逐个校验，不要求下载开发包或调试包。校验文件中的路径以它所在目录为基准，解压后需保持这层关系。

这些包通过普通、保留签名检查的 APT 安装，以解析依赖。当前发布 rootfs 基线还需要 `erofs-utils`、`libqmi-glib5`、`libqmi-proxy`、`libqrtr-glib0`；APT 同时解析 `libmbim-glib4`、`libmbim-proxy`，其余运行库沿用已有 rootfs 的 ABI。离线安装也必须备齐这些依赖。

## 启动与设备数据

启动顺序为 `piano-adsp.service` 和 `piano-sensors-import.service` → `adsprpcd-sensorspd.service` → `iio-sensor-proxy.service`。上游 sensors PD 服务已依赖 import，并排在 ADSP 之后；本地只增加 `Requires=piano-adsp.service` drop-in。`adsprpcd sensorspd` 启动后，上游脚本最多等待 60 秒，依次查询 accelerometer、light、proximity、compass。超时仍继续启动并记录缺失类型，所以服务 active 不代表四类传感器均正常。

上游包 mask `adsprpcd.service`（根 PD）与 `adsprpcd_audiopd.service`，并用 `/etc/udev/rules.d/59-fastrpc-remoteproc.rules` 覆盖 FastRPC 自动启动所有 DSP 的规则。只由现有 ADSP 服务启动 ADSP，再运行 sensors PD；当前音频 GPR 路径不依赖 audio PD。

首次启动时，import 从这台平板自己的 ODM 配置与 persist 注册表/校准数据生成 `/var/lib/piano-sensors` 中的副本。构建不复制 `private/captures` 中的用户数据，也不随镜像分发这些配置或校准文件。ODM 用 read-only device-mapper 映射并由 `dump.erofs` 读取；persist 保留 `ro,noload` 挂载，ADSP 后续更新写入导入副本。

上游 importer 默认读取 `super` 的 primary metadata slot 0，并选择 `odm_a`。部署前需与设备当前 ROM 的逻辑分区布局核对；`PIANO_SENSORS_ODM_NAME` 可覆盖分区名，但不改变 metadata slot 0 的读取方式。不能仅凭包安装成功判断它导入了当前 ROM 的配置。

## 运行状态

构建集成和包检查不证明实机读数可用。当前说明未包含已通过的运行实测；还需在实际启动后核对 import 来源、ADSP/sensors PD 日志、四类 SSC 读数，以及桌面旋转和亮度行为。可先读取服务日志：

```sh
systemctl status piano-adsp piano-sensors-import adsprpcd-sensorspd iio-sensor-proxy
journalctl -b -u piano-adsp -u piano-sensors-import -u adsprpcd-sensorspd -u iio-sensor-proxy
```
