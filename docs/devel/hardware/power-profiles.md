# 电源模式（节能 / 平衡 / 性能）

Debian GNOME、Debian KDE、Nyarch 和 SteamOS 共用 `piano-power-profiles`。
GNOME 和 KDE 通过标准 PowerProfiles D-Bus 接口选档；SteamOS 的游戏模式
通过 Manager 接入同一个接口。`tuned-ppd` 将档位映射到 TuneD 配置，再由
TuneD 的 `sysfs` 插件设置 CPU、GPU 上限。

## 为什么不用 power-profiles-daemon

原版 `power-profiles-daemon` 在本机找不到 ACPI 电源模式接口，只能用
`placeholder` 后端登记 `balanced` 和 `power-saver`。两档不会实际限制
CPU、GPU；SteamOS 的 KDE 也不会把它们显示为可选档位：

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

| 电源模式 | TuneD profile | policy0 上限 | policy6 上限 | CPU boost | GPU 上限 |
| --- | --- | ---: | ---: | ---: | ---: |
| 节能 | `piano-power-saver` | 1785600 kHz | 1958400 kHz | 0 | 342 MHz |
| 平衡（默认） | `piano-balanced` | 3532800 kHz | 4089600 kHz | 0 | 1100 MHz |
| 性能 | `piano-performance` | 3532800 kHz | 4320000 kHz | 1 | 1100 MHz |

下限由 TuneD profile 固定为 384000 / 1017600 kHz，governor 统一 `schedutil`。
数值来自上游 piano 整合方案的 `device/power/tuned/profiles`（MIT）。
`policy6` 的 4320000 kHz 不在 `scaling_available_frequencies` 里
（该列表止于 4089600）；这是驱动接受的上限，不表示 CPU 一定运行在这个频率。
内核可根据温度或负载选用更低的频率。CPU 的温控来自片上温度传感器和
cpufreq 冷却设备，GPU 用 devfreq 冷却设备，档位不会修改内核温控阈值。

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
`/etc/tuned/ppd.conf` 本身仍归发行版包管理。卸载 `piano-power-profiles`
并重启服务后，TuneD 使用发行版的映射。

## 安装与打包

镜像预装配置包并启用 `tuned-ppd.service`。配置包发布在 SunUEFI 的
APT 软件源和 `sunuefi` pacman 软件源；Debian sid 与 Arch Linux ARM
提供 TuneD。Valve 的 ARM64 软件源没有 TuneD，SteamOS 使用
`sunuefi-steamos` 中按 Valve 的依赖版本构建的包。

配置源码位于 [`linux/bsp/power-profiles`](../../../linux/bsp/power-profiles)。
用 `./build.sh bsp package --power-profiles --target debian --format deb --output DIRECTORY`
构建 Debian 包；Arch 构建把目标和格式分别换成 `arch` 和 `pacman`。

从旧镜像安装配置包时，已有手工放入的 TuneD 文件应先核对。APT 可接管
内容一致的默认文件；pacman 遇到冲突时，先备份并移走报错的文件，再安装。
装完后将自定义配置放进 TuneD 的 `/etc/tuned/profiles` 或自己的 systemd
覆盖文件，不要覆盖 `/usr/lib` 下的包文件。

## 内核调度接口

各发行版共用的内核启用任务和 cgroup 的 `uclamp`，让应用通过
`sched_setattr()`、systemd 通过 `CPUUtilizationMin=`／`CPUUtilizationMax=`
提交性能提示。普通任务默认不要求最低利用率，也不限制最高利用率；实时任务
保留内核默认的性能保障。项目没有增加统一的利用率下限、锁核策略或调参服务，
已有的 `schedutil` 动态调频和 BORE 参数不变。启用接口本身不产生节能策略。

原厂最终设备树为 CPU0–5、CPU6–7 分别提供 `capacity-dmips-mhz` 1792、1894，
内核结合各组最高频率计算相对性能，用于选择运行任务的 CPU。发布用板级设备树
已包含这些值，原生 SM8750 DTS 也保留相同描述；这项源码对齐没有改变产品的
启动设备树。来源记在[内核补丁清单](../../../patches/linux/7.3/series.json)。

## 实机档位切换

在 SteamOS 实机用 pacman 安装后，KDE PowerDevil 读到三档。通过 KDE 的
接口逐档切换，读回的上限和 Debian GNOME 上通过 PowerProfiles 测得的一致：

```console
power-saver -> tuned=piano-power-saver p0=1785600 p6=1958400 boost=0 gpu=342000000
performance -> tuned=piano-performance p0=3532800 p6=4320000 boost=1 gpu=1100000000
balanced    -> tuned=piano-balanced    p0=3532800 p6=4089600 boost=0 gpu=1100000000
```

GPU 下限全程保持 160000000。开机默认由 `piano-ppd.conf` 的
`default=balanced` 决定。

在同一台平板的 Gamescope 会话安装新版 Manager 后，`steamosctl` 能读取
三档；从游戏侧切节能和性能、从 PowerProfiles 接口切回平衡，读回的上限
与上表一致。重启 `tuned-ppd` 后，原游戏会话不需要重启即可继续切档。

档位检查只确认 CPU/GPU 上限生效，不能用来推算待机续航。熄屏但仍在运行、
以及进入整机挂起的功耗要分别测量，见[桌面电源管理与睡眠](sleep.md)。
