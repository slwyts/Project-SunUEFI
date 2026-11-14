# Piano 自动亮度策略

在 GNOME 自有 power manager 内增加可选设备配置，不增加服务或改传感器值。`piano.ini`只匹配`xiaomi,piano`和测量单位`lux`，沿原厂实际的三条门限曲线分别等待1000/1000/5000ms。其他设备和缺配置保持上游行为。配置安装到`/usr/share/gnome-settings-daemon/ambient-profiles/piano.ini`；需要同时安装编译后的GSD包，仅复制配置不能使未打补丁的GSD获得此功能。

亮度幅度使用原厂 108 点 lux→nits 曲线与 KTZ8866 指数码值的相对变化，以用户手动亮度为基准，保留 GNOME 平滑。关屏期间停止调光。曲线来源、系数、零亮度恢复和绝对亮度边界见[自动亮度文档](../../../../docs/devel/hardware/auto-brightness.md)。

## 默认 release / CI

正常默认系统使用 `./build.sh gsd` 生产标准 Debian sid 包，rootfs 构建消费 `build/gsd/runtime` 及相邻 `SOURCE.json`。工具克隆已完成的 sid ARM64 base，不改输入；仅在副本中恢复 DNS、配置源码 APT 源并安装构建依赖。输入需保留 sid 的二进制与源码索引，两者均使用 Debian archive keyring。

```sh
sudo ./build.sh gsd \
  --output build/gsd --sysroot /absolute/project/build/completed-root --jobs 4
```

源码版本从目标 sid ARM64 二进制候选的 `Source` 字段选择，再核对签名 Sources、`.dsc` 与 archive 的 SHA256。缓存绑定该源码、目标桌面 ABI 和共享补丁；不复用其他 Debian suite 或旧 GNOME 的包。沿原始 `debian/rules`、`debian/control` 和 quilt 构建，跳过上游测试套件但保留原功能。输出收集 daemon/common 两个运行包、`.changes`、`.buildinfo` 与来源记录。

## Debian sid / GNOME 51

使用发行版签名 APT 源提供的 `gnome-settings-daemon` 源码及正常构建依赖。Debian 和 Nyarch 共用 `51.0/series.json` 的 ambient 策略、键盘热插拔及启动锁屏状态补丁；该系列覆盖已核对的 51.0 和 51.1，不为发行版复制补丁。在新目录准备副本时，工具将这些补丁加入既有 quilt series 并由 `dpkg-source` 应用；不会修改输入目录或安装包。

```sh
apt-get source gnome-settings-daemon
sudo apt-get build-dep gnome-settings-daemon
python3 linux/desktops/gnome/ambient-policy/packaging/prepare_package.py \
  debian /absolute/new/gsd-piano-build --source /absolute/gnome-settings-daemon-51.0
cd /absolute/new/gsd-piano-build
dpkg-buildpackage -b -us -uc
```

发行版源码需为该系列支持的版本且使用标准 `3.0 (quilt)` 格式；上下文不同会在准备时失败，不能忽略补丁错误。生成标准 `gnome-settings-daemon` 包，原始源码版本后追加 `+sunuefi1`。由发行版正常安装/升级流程替换包，重新登录使新 power manager 生效。

## Arch / Arch Linux ARM 51

GNOME 51 使用 Shell brightness 接口。Arch 配方从目标 pacman 仓库选择版本，要求目标已安装的 GSD 与仓库同版，再从共享系列读取对应官方 release tar 的 SHA256。准备结果同时记录源码和补丁摘要，不再把仓库的 51.1 降到 51.0。

```sh
python3 linux/desktops/gnome/ambient-policy/packaging/prepare_package.py \
  arch /absolute/new/gsd-piano-package --rootfs /absolute/project/build/distros/nyarch-gnome/rootfs
cd /absolute/new/gsd-piano-package
makepkg -s
```

Nyarch 包保留仓库的上游版本，在仓库 pkgrel 后追加 `.1`；例如仓库为 `51.1-1` 时生成 `51.1-1.1`。显示关闭与输入唤醒仍通过桌面和内核的标准接口处理。
