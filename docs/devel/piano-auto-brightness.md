# 自动亮度与原厂策略

2026-10-08，本轮离线解析已有`private/captures/2026-10-08-android-display-pen/display.txt`并对照GNOME源码，没有改设备亮度、SensorProxy lux、锁屏、DPMS或内核构建输入。

## 实际原厂配置

原厂主ALS为Sensortek STK3BCX，采样250ms，warm-up0；快/慢环境窗口1000/3000ms，weighting intercept3000。增亮/变暗持续门限均1000ms，小幅增亮5000ms。还有辅助背面ALS、proximity和场景策略，不能把主ALS配置冒称整套算法。

真正起作用的Miui滞回是dump末段的**绝对lux线性曲线**，不是前面的通用HysteresisLevels百分比表。已提取大幅增亮15点、变暗19点、小幅增亮15点；用实际mAmbientLux=136.01251代入，得到314.8489396/27.4223739/243.3143733，和dump保留的三个阈值一致。小幅增亮直接使用695行的整条`mHysteresisSmallBrightSpline`，没有根据单点推断比例。此捕获在display OFF，说明配置与保留阈值吻合，不是主动测试回放。

| 已接受lux | 原厂变暗阈值 | 原厂增亮阈值 | 原厂目标nits |
| --- | --- | --- | --- |
| 100 | 23.4844 | 262.2467 | 103 |
| 136.01251 | 27.4224 | 314.8489 | 107.6013 |
| 197 | 34.0913 | 403.9313 | 113.7 |
| 300 | 43.8588 | 554.38 | 123 |
| 500 | 145.4692 | 846.5133 | 144 |

完整lux→nits曲线有108点，100..300lux段只从103升到123nits，包含多个低光平台。dump还给出backlight/nits点`(0.001709819,2)`、`(0.49975574,600)`、`(1,800)`。当前PhysicalMapping的current(bl)与另一个DDC normalized map不同；不得将这些nits或Android UI百分比直接当Linux背光百分比。完整原始数组、两条Miui曲线及复现结果在`private/analysis/oem-auto-brightness-20261008/actual-configuration.json`。

[Android16参考ABC](https://android.googlesource.com/platform/frameworks/base/+/android16-release/services/core/java/com/android/server/display/AutomaticBrightnessController.java)用权重积分`x*(x/2+intercept)`计算快/慢窗口，两者和持续时间都越过阈值后更新已接受lux。这有助于解释dump里的窗口/门限；没有因此把AOSP源当完整Miui实现。

## GNOME为何更敏感

[gsd48.1 ALS处理](https://github.com/GNOME/gnome-settings-daemon/blob/48.1/plugins/power/gsd-power-manager.c#L2833)使用0.1Hz指数低通，时间常数约1.59155s；目标是`lux*100/ambient_norm_value`，根据用户手动亮度归一化，再截到0..100。没有上述Miui lux滞回和1000ms持续门限，每次properties通知都提交整数百分比，包括相同目标。

例如197→300lux，原厂目标nits只约增加8.2%，原厂打印的UI档位约62→64；现GSD线性目标增加约52.3%（之后受100%截断）。这说明策略差异，不能替代Linux面板实测nits校准。

本轮已在现有GSD backlight policy实现可选设备配置。`linux/desktops/gnome/ambient-policy/piano.ini`保存三条真实门限曲线和1000/1000/5000ms时序；只在DT包含`xiaomi,piano`且SensorProxy单位为`lux`时选择。缺配置、其他机器、其他单位保持原行为，不修改SensorProxy的读数。

策略保持一个“已接受lux”，三路分别计算越过门限的连续时间。回到范围内取消相应计时；大门限要求1000ms、小增亮门限要求5000ms。之后仍用GSD原有的用户亮度归一化和指数平滑；定时器保证稳定lux没有后续notify时也能完成持续门限和过渡。手动调亮度、释放传感器、传感器消失和退出服务都取消定时器并重置。匹配设备还会跳过相同目标的重复背光提交；建立非零归一化之后允许真实0lux进入变暗判断。

这部分解决频繁小变动触发的问题，但还不是完整原厂自动亮度：未接原厂双窗加权、辅助ALS/场景策略，也未把108点nits曲线映射到Linux KTZ背光。亮度幅度仍受GSD线性映射影响，不能宣称已达到原厂完整亮度轨迹。GNOME之外的桌面仍可直接使用真实SensorProxy和背光接口，不依赖此补丁。

同一配置有针对官方GSD48.1和51.0两种接口的补丁：48.1使用原backlight API，51.0使用标准Shell brightness API。两份真实官方发布包均校验SHA256并完成`patch --fuzz=0`应用；helper实际严格C编译通过。当前还未完成GSD整包编译/安装或实机体验验证，本机缺其GTK/UPower/notify/Meson构建依赖。下一默认DT打包已接正式板级身份，当前旧DT不会误启策略。普通Debian/Arch包构建入口见[策略打包说明](../../linux/desktops/gnome/ambient-policy/README.md)。

## 标准设置界面

[cc-power-panel48.4 `als_enabled_state_changed`](https://github.com/GNOME/gnome-control-center/blob/48.4/panels/power/cc-power-panel.c#L405)要求SensorProxy HasAmbientLight且`has_brightness`，后者由Power.Screen.Brightness≥0决定（638行）。ambient-enabled决定开关状态，不决定隐藏。电池决定分页布局，ALS在Power Saving组，不在Display页面。

当前实际HasAmbientLight=true、ambient-enabled=true、Screen.Brightness=99。已通过标准AT-SPI动作进入GNOME设置的Power Saving分页，并查看真实3200×2136截图：Automatic Screen Brightness原生行可见，开关开启。没有强制显示能力行或另做设置界面。
