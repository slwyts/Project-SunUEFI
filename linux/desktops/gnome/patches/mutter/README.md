# Mutter 48.7 本地适配

针对 Debian 13 的 `mutter 48.7-0+deb13u1`，本地版本为
`48.7-0+deb13u1+sunuefi1`。补丁保留上游源码和原 Debian `rules`、`control`：

- `0001` 将空 Gamma LUT 表示为已有的 NULL bypass，避免对零项数组重采样；非空曲线不变。
- `0002` 在 colord 真实连接完成且没有现有默认 profile 时，让无 EDID 的物理输出进入已有标准 sRGB fallback。生成文件以实际 colord device ID 的 SHA256 命名，ICC 内记录实际 Filename 和 MAPPING_device_id，供 colord 原有 soft 关联使用。已有默认 profile（包括 soft）不替换，EDID 路线不变。

sRGB fallback 标为 **uncalibrated**，没有面板实测校准、伪 EDID、HDR 声明或新用户服务。

标准包入口复用 GSD 的 Debian 源校验器。以完成的 Debian trixie ARM64
sysroot 为输入会先克隆，再在独立 chroot 使用签名 APT 的精确 Sources 记录
校验 `.dsc` 与两个 archive，通过正常 quilt 和 debhelper 构建源码及二进制包：

```sh
sudo python3 tools/build_mutter_packages.py \
  --sysroot build/distros/release-7.2.9/rootfs --jobs 4
```

已有独立 ARM64 package builder（原生或标准 QEMU binfmt）可复用，避免再次复制和安装依赖：

```sh
sudo python3 tools/build_mutter_packages.py \
  --builder-root build/mutter-package-builder-20261008/rootfs \
  --skip-install-dependencies --jobs 4
```

`--builder-root` 必须是独立构建目录，不传设备或已封存的产品 rootfs。
`--skip-install-dependencies` 仍运行实际 `dpkg-checkbuilddeps -Pnocheck`。
工具会在独立 builder 内读取实际 GBM/PipeWire 版本，从已签名 APT 索引选择
同版本开发包。对本项目 `+pianoN` 的 Mesa 包一起调整完整来源 cohort，
APT 模拟有删除则停止；不修改原输入或设备，也不猜测其他大版本。
实际版本、包 SHA 和 solver 记录随 `SOURCE.json` 保存。

输出 `build/mutter/runtime/` 中五个标准运行包，`all/` 保留正常构建产物，
`SOURCE.json`、`SHA256SUMS` 记录固定来源、实际补丁、包版本及 hash。
使用原配方与 `DEB_BUILD_OPTIONS=nocheck parallel=4`，不执行完整测试套件。
构建成功不证明设备颜色更新或空 LUT 恢复安全；安装和正常重启 GNOME 后，
仍需由设备 owner 核实际 profile、标准 Night Light、硬件完成及原状态恢复。
