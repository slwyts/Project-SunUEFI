# 电源模式（节能 / 平衡 / 性能）

GNOME 的三档电源模式由 `tuned-ppd` 提供，映射到三个 TuneD profile，
再由 TuneD 的 `sysfs` 插件写 CPU、GPU 上限。全程纯配置，没有自研守护进程。

## 为什么不用 power-profiles-daemon

Debian 的 `power-profiles-daemon` 在本机只暴露 **两档**。它在 ARM 上找不到
ACPI platform profile，退化成 placeholder 驱动，而 placeholder 只注册
`balanced` 和 `power-saver`：

```console
$ busctl get-property org.freedesktop.UPower.PowerProfiles \
    /org/freedesktop/UPower/PowerProfiles \
    org.freedesktop.UPower.PowerProfiles Profiles
aa{sv} 2 ... "Profile" s "power-saver" ... "Profile" s "balanced"
```

`tuned-ppd` 提供同一套 D-Bus 接口（`org.freedesktop.UPower.PowerProfiles`，
另一个别名 `net.hadess.PowerProfiles`），但 driver 名是 `tuned`，三档齐全：

```console
$ busctl get-property ... Profiles
aa{sv} 3 "power-saver" "balanced" "performance"
```

两者互斥，镜像只装 `tuned-ppd`（它依赖 `tuned`）。

## 档位与上限

| GNOME 模式 | TuneD profile | policy0 上限 | policy6 上限 | CPU boost | GPU 上限 |
| --- | --- | ---: | ---: | ---: | ---: |
| 节能 | `piano-power-saver` | 1785600 kHz | 1958400 kHz | 0 | 342 MHz |
| 平衡（默认） | `piano-balanced` | 3532800 kHz | 4089600 kHz | 0 | 1100 MHz |
| 性能 | `piano-performance` | 3532800 kHz | 4320000 kHz | 1 | 1100 MHz |

下限由 TuneD profile 固定为 384000 / 1017600 kHz，governor 统一 `schedutil`。
数值来自上游 piano 整合方案的 `device/power/tuned/profiles`（MIT），
在本机三档全部实测通过。`policy6` 的 4320000 kHz 不在
`scaling_available_frequencies` 里（该列表止于 4089600），但 SoC 接受写入。

## 两个必须记住的约束

**GPU 只写上限，不写下限。** devfreq 的 `min_freq` 不能先于 `max_freq` 写：
先把下限写成 160 MHz、再把上限降到 342 MHz，驱动会把下限一起抬到上限，
GPU 就被钉在 342 MHz。只写上限时下限保持 160 MHz 默认值。
另外驱动会自行改写 `min_freq`，读回值不可信（写 160000000 读回 320000000
是正常现象），所以 profile 里根本不出现这条路径。

**映射文件不覆盖 conffile。** `tuned-ppd` 把 `/etc/tuned/ppd.conf` 作为
dpkg conffile 发布，直接替换会让每次包升级都询问保留还是替换，选"保留"
又会把我们的映射丢掉。改为把映射放 `/etc/tuned/piano-ppd.conf`，
用 drop-in 在服务内挂到该路径：

```ini
# /usr/lib/systemd/system/tuned-ppd.service.d/50-piano-ppd.conf
[Service]
BindReadOnlyPaths=/etc/tuned/piano-ppd.conf:/etc/tuned/ppd.conf
```

`systemctl show tuned-ppd -p BindReadOnlyPaths` 应输出
`/etc/tuned/piano-ppd.conf:/etc/tuned/ppd.conf:rbind`，而
`/etc/tuned/ppd.conf` 本身仍是包里的内容。删掉 drop-in 并重启即恢复原厂映射。

## 文件位置

| 路径 | 作用 |
| --- | --- |
| `linux/desktops/gnome/power-overlay/usr/lib/tuned/profiles/piano-*/tuned.conf` | 三档上限 |
| `.../etc/tuned/piano-ppd.conf` | GNOME 模式 → TuneD profile 映射 |
| `.../usr/lib/systemd/system/tuned-ppd.service.d/50-piano-ppd.conf` | 把映射挂进服务 |
| `config/release.json` 的 `debian.extra_packages` | 装 `tuned-ppd`（KDE 构建会剔除） |

## 2026-11-04 实测记录

在 Debian 13 + GNOME 48.7 上，`busctl set-property ... ActiveProfile`
依次切三档，每档等 3 秒读 sysfs：

```console
power-saver -> tuned=piano-power-saver p0=1785600 p6=1958400 boost=0 gpu=342000000
performance -> tuned=piano-performance p0=3532800 p6=4320000 boost=1 gpu=1100000000
balanced    -> tuned=piano-balanced    p0=3532800 p6=4089600 boost=0 gpu=1100000000
```

GPU 下限全程保持 160000000。开机默认由 `piano-ppd.conf` 的
`default=balanced` 决定。

**注意**：这套改动只影响上限，不改变待机功耗——锁屏后的 1.07 W 与
CPU/GPU 频率无关，见[硬件/睡眠](sleep.md)。
