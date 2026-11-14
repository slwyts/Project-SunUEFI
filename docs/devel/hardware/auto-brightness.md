# 自动亮度与原厂策略

GNOME 电源管理器按 Piano 的环境光门限和相对亮度曲线调光，保留用户手动选择的亮度，不修改 SensorProxy 的照度读数。显示关闭后停止调光，唤醒时恢复保存的亮度，再处理新的环境光。

## 原厂配置与来源

主环境光传感器为 Sensortek STK3BCX，原厂采样间隔为 250 ms。HyperOS 的显示服务还使用 1000／3000 ms 快慢环境窗口、辅助背面传感器、距离传感器和场景策略；GNOME 补丁只使用主传感器，不移植整套场景判断。

`linux/desktops/gnome/ambient-policy/piano.ini` 中的三条照度门限来自同机 HyperOS `OS3.0.309.0.WPYCNXM` 显示服务的运行配置：大幅增亮 15 点、变暗 19 点、小幅增亮 15 点，分别要求连续越过门限 1000／1000／5000 ms。这些是绝对 lux 曲线，不是通用的百分比回差表。例如，已接受照度为 136.01251 lux 时，三条曲线分别给出 314.8489、27.4224、243.3144 lux，与原厂显示服务保留的阈值一致。

108 点目标亮度曲线来自同机 HyperOS 4.0.6 的 `product.img` 中 `/overlay/AospFrameworkResOverlay.apk`：`config_autoBrightnessLevels` 前补 0 lux，与 `config_autoBrightnessDisplayValuesNits` 逐点配对。Linux 使用这些点做线性插值，不把 Android 的背光数组直接作为 Linux 百分比。

| 环境光 | 原厂目标亮度 |
| --- | --- |
| 0 lux | 3.5 nit |
| 7 lux | 24 nit |
| 30 lux | 97 nit |
| 100 lux | 103 nit |
| 197 lux | 113.7 nit |
| 300 lux | 123 nit |
| 500 lux | 144 nit |

背光使用 KTZ8866 指数电流映射。芯片的 [04b 数据手册](https://kinet-ic.com/uploads/web/KTZ8866/KTZ8866-04b.pdf)给出 `I ∝ 1.003040572^code`；设备两颗芯片的 `BL_CFG1=0x53`，其中 `BLED_MAP=0`，与该模式一致。原厂的普通亮度／高亮度区间和 `mi_bl_level_remap` 转换见[显示亮度说明](display-depth.md#hdr-亮度与背光)。

## 相对亮度映射

[GSD 48.1 上游实现](https://github.com/GNOME/gnome-settings-daemon/blob/48.1/plugins/power/gsd-power-manager.c#L2833)将亮度百分比按 lux 线性缩放。照度从 197 降到 7 lux 时，60% 会变为约 2%；而原厂目标亮度仅从 113.7 降到 24 nit，直接按照度比例计算会过度变暗。

Piano 策略先用三条门限确定已接受的照度，再取得目标亮度 `N(lux)`。用户在 `L0` lux 下选择 `P0` 百分比后，后续目标为：

```text
P(lux) = clamp(P0 + K × ln(N(lux) / N(L0)), 0, 100)
K = 100 / ((2047 - 20) × ln(1.003040572))
  = 16.249888161019676
```

`20..2047` 是 GSD 在该背光接口上使用的码值范围。这里计算相对电流变化，不宣称 Linux 输出已校准到绝对 nit；内容相关背光、面板透射率和原厂 HDR 补偿也不包含在内。未来改变内核背光码值含义时，必须同步调整该系数。

例如，197 lux 下手动设为 60%，降到 7 lux 后目标约为 35%，升到 300 lux 后约为 61%。7 lux 下手动设为 61%，降到 0 lux 后约为 30%，不再因为照度为零直接落到最低档。用户仍能主动选择最低亮度；零百分比和零 lux 都可作为有限的基准，之后光线变亮仍能恢复。

持续门限后沿用 GNOME 的 0.1 Hz 指数平滑，时间常数约 1.59 秒。定时器补全没有后续传感器通知的过渡；相同整数目标不重复提交。手动调亮度立即重建基准，释放或丢失传感器时取消定时器并清除门限计时。显示关闭时不接受新的调光目标，保留亮度基准和用户请求值。

## 桌面与构建接入

只在设备树包含 `xiaomi,piano` 且 SensorProxy 单位为 `lux` 时选择 Piano 曲线。其他设备、其他单位或缺配置时保留上游映射。配置和程序都安装在标准 GSD 软件包里，不增加调光服务。

GSD 48.1 使用原 backlight API，以当前用户百分比建立基准；51.0 使用标准 Shell brightness API，保留 Shell 的用户亮度偏移，由 GSD 输出相对自动亮度目标。Debian 版本为 `48.1-1+sunuefi4`，Nyarch 为 `51.0-1.4`；同时包含键盘背光热插拔刷新和[启动时锁屏状态同步](power-button.md#默认设置)。

`./build.sh gsd` 从发行版签名 APT 源校验 Debian 48.1 源码，在隔离的 ARM64 builder 中沿原 `debian/rules` 构建。`build/gsd/runtime` 提供 daemon／common 两包，`SOURCE.json` 记录源码、补丁和软件包摘要。Nyarch 使用对应 51.0 配方。构建命令见[策略打包说明](../../../linux/desktops/gnome/ambient-policy/README.md)。

## 标准设置界面

GNOME 设置的电源页面使用原生自动亮度开关。它要求 SensorProxy 的 `HasAmbientLight=true`，并且 GSD 提供可控制的屏幕亮度；`ambient-enabled` 控制启用状态。没有强制显示硬件能力或增加第二个设置入口。
