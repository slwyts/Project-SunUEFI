# Mutter 适配

Debian sid 沿发行版原始 `debian/rules`、`debian/control` 和 quilt 构建，与 Nyarch 共用空 Gamma LUT 和取消触摸 grab 两项修复。共享系列在 `patches/mutter/51.0/series.json`，覆盖 51.0 和 51.1。源码版本从目标 ARM64 APT 候选的 `Source` 字段选择，包版本在该版本后追加 `+sunuefi1`，不固定旧 Debian 13 的 ABI。

`0001-kms-empty-gamma-bypass.patch` 将零项 Gamma LUT 表示为已有的 NULL bypass，避免重采样时除以零和访问空数组，非空曲线不变。签名 sid 源码的 `meta-kms-update.c` 仍将零项 LUT 传入 `meta_gamma_lut_copy_to_size()`，所以这项补丁仍需保留。

`0003-wayland-touch-ignore-cancelled-grab.patch` 在触摸序列取消、focus 已清除后拒绝交互移动和缩放请求，避免解引用空指针。sid 源码的 `touch_can_grab_surface()` 仍直接读取 `focus_surface->surface`。这两项均为 downstream 补丁，尚未声明上游接受；删除前按共享系列的条件核对发行源码和功能。

无 EDID 输出不再使用本地补丁。发行版 Mutter 的 `meta-color-store.c` 已按真实 device ID 生成 sRGB fallback，`meta-color-device.c` 保留 colord 默认 profile；内核内屏也提供匹配面板的合成 EDID。通用 sRGB 不是面板实测校准，不能据此声明 HDR 或色彩精度。

```sh
sudo ./build.sh mutter --sysroot build/distros/release/rootfs --jobs 4
```

输入是已完成的 sid ARM64 sysroot，保留签名 APT 二进制与源码索引。工具先克隆，再在独立 chroot 校验 InRelease、Sources、`.dsc` 与 archive 的 SHA256。缓存绑定当前源码、目标桌面 ABI 和补丁，不能复用旧 `libmutter-16` 包。

已有独立 builder 可用 `--builder-root 路径 --skip-install-dependencies`，仍执行实际 `dpkg-checkbuilddeps -Pnocheck`。不要传设备或已封存的产品 rootfs。

`build/mutter/runtime/` 提供五个标准运行包，包括 `libmutter-51-0` 和 `gir1.2-mutter-51`。输出同时保留 `.changes`、`.buildinfo`、`SOURCE.json` 与 `SHA256SUMS`。使用 `DEB_BUILD_OPTIONS=nocheck parallel=4` 不执行完整上游测试套件；设备验证仍需在正常安装、重启 GNOME 后检查标准 Night Light、空 LUT 往返和原状态恢复。
