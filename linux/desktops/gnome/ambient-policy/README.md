# Piano 自动亮度策略

在 GNOME 自有 power manager 内增加可选设备配置，不增加服务或改传感器值。`piano.ini`只匹配`xiaomi,piano`和测量单位`lux`，沿原厂实际的三条门限曲线分别等待1000/1000/5000ms。其他设备和缺配置保持上游行为。配置安装到`/usr/share/gnome-settings-daemon/ambient-profiles/piano.ini`；需要同时安装编译后的GSD包，仅复制配置不能使未打补丁的GSD获得此功能。

原厂 nits 曲线尚未校准到 Linux 背光，保留上游亮度归一化与平滑。当前实现范围与实际来源见[自动亮度文档](../../../../docs/devel/piano-auto-brightness.md)。

## 默认 release / CI

正常默认系统使用`tools/build_gsd_packages.py`生产并安装标准Debian包，Root构建入口消费`build/gsd/runtime`及相邻`SOURCE.json`。工具克隆已完成的trixie ARM64 base，不改输入；仅克隆内恢复DNS、配置签名源码APT源并默认安装构建依赖。

```sh
sudo python3 tools/build_gsd_packages.py \
  --output build/gsd --sysroot /absolute/project/build/completed-root --jobs 4
```

源码固定为Debian48.1-1，沿原始`debian/rules`构建，跳过上游测试套件但保留原功能。输出仅收集daemon/common两个正常runtime包，记录实际源码、补丁、版本、`.changes`与包SHA256；匹配成功产物直接复用。已有隔离job可使用`--collect-only --source-cache ... --prepared-source ...`收包，不再下载或编译。标准`dpkg-source --before-build`也处理构建后quilt自动撤销的真实生命周期。

## Debian 48.1

使用发行版签名 APT 源提供的`gnome-settings-daemon`48.1源码及正常构建依赖。在新目录准备副本，工具将补丁加入既有quilt series并由`dpkg-source`实际应用；不会改输入目录或安装包。

```sh
apt-get source gnome-settings-daemon
sudo apt-get build-dep gnome-settings-daemon
python3 linux/desktops/gnome/ambient-policy/packaging/prepare_package.py \
  debian /absolute/new/gsd-piano-build --source /absolute/gnome-settings-daemon-48.1
cd /absolute/new/gsd-piano-build
dpkg-buildpackage -b -us -uc
```

发行版源码需为48.1且使用标准`3.0 (quilt)`格式；上下文不同会在准备时失败，不能忽略补丁错误。生成标准`gnome-settings-daemon`包，包版本追加`+sunuefi1`。由发行版正常安装/升级流程替换包，重新登录使新 power manager 生效。

## Arch / Arch Linux ARM 51.0

官方当前51.0已改用Shell brightness接口，不能把48.1补丁硬套。此处提供同功能51.0补丁和正常`PKGBUILD`，固定官方release tar的SHA256，准备时填写本地补丁SHA256。应在具有相应51.x依赖的目标发行版构建；不会为较旧桌面自动降级或更换GNOME大版本。

```sh
python3 linux/desktops/gnome/ambient-policy/packaging/prepare_package.py \
  arch /absolute/new/gsd-piano-package
cd /absolute/new/gsd-piano-package
makepkg -s
```

两份补丁分别适配上游版本，设备配置和功能相同。真实官方48.1/51.0源均已校验并无fuzz应用；Debian48.1完整ARM64包已实际构建和收集，51.0尚未编译整包。设备亮度体验仍待默认系统集成后验证。
