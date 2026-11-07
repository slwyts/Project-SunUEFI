# 电源键、合盖与睡眠

## 分工

内核负责电源键和保护套开关上报、面板断电与恢复、系统挂起及唤醒。桌面策略使用 GNOME、logind 和 DRM 的标准接口；锁屏认证和长按关机菜单不放进内核。

GNOME 的[电源键助手](../../../linux/desktops/gnome/power-overlay/usr/lib/piano/power-button)只补充桌面尚未提供的按键策略：

- 短按电源或合盖：锁屏并关闭显示。
- 再次短按或开盖：显示锁屏页面，仍需正常解锁。
- 长按电源：打开 GNOME 关机菜单。

助手观察 `pmic_pwrkey` 的 `KEY_POWER` 和 `soc:piano-hall-ml` 的 `SW_LID`，不独占设备，不读取触摸或键盘内容。显示通过 `org.gnome.Mutter.DisplayConfig.PowerSaveMode` 控制，不以单独关闭背光代替面板断电。

## 唤醒后再次灭屏的修正

旧助手同步等待 `org.gnome.ScreenSaver.Lock`，超时后仍继续灭屏；同时只记住自己上次发出的显示命令，无法识别 GNOME 自动熄屏或先处理了唤醒按键。这两种情况都可能把一次唤醒误处理成灭屏。

现在使用异步 D-Bus：同时查询屏保和图形会话的 `LockedHint`，确认已锁屏时不重复请求 `Lock`；锁屏期间仍能处理按键和开盖。每次唤醒会取消尚未完成的灭屏请求。显示状态来自 Mutter 的属性通知，并用输入事件的单调时钟区分刚唤醒屏幕的那次按键。

设备上的显示电源状态已按 OFF → ON 切换，恢复后没有再次执行本助手的延迟灭屏；立即取消灭屏请求后也保持 ON，图形会话仍为锁定状态。按键、触摸及 GNOME 自身空闲计时的组合仍需观察；本修正不禁用桌面自动熄屏。

## 默认设置

[标准 dconf 配置](../../../linux/desktops/gnome/power-overlay/etc/dconf/db/local.d/00-power)设置 2 分钟空闲熄屏、使用电池时 5 分钟自动挂起、外接电源时 10 分钟自动挂起。键盘背光默认 80%，后续沿用用户保存的亮度。用户可在系统设置里修改这些偏好。

构建启用 `sleep.target` 和 `suspend.target`，使用 `mem/deep`。休眠、混合睡眠及先睡眠后休眠保持禁用。

## 内核适配

- 显示 PLL 保存恢复、双 DSI 关闭顺序、DPU 停止顺序和 PHY 运行时时钟由 `patches/linux/7.3/0002-display.patch` 处理。
- `0029-sm8750-pcie-active-wakeup.patch` 在唤醒链路上保留 PCIe 电源域，避免恢复后 Wi-Fi 的 PCI 配置不可读。
- `0030-soc-qcom-smp2p-sleepstate.patch` 向原厂固件通知 AP 睡眠状态；它不能单独解决全部 ADSP 恢复问题。
- `0032-asoc-q6apm-clear-global-on-remove.patch` 修正 ADSP 重启时的全局对象生命周期。
- `0033-fastrpc-freeze-inflight-invoke.patch` 处理睡眠时等待中的 FastRPC 调用。

设备已完成 RTC 和电源键唤醒；待机耗电仍需优化。调试睡眠时以同一启动 ID、恢复错误和触控／网络状态判断结果，不能仅凭 USB 断开就认定挂起或重启成功。

## 构建接入

GNOME 组装流程覆盖同名 helper、user unit、udev、logind 和 dconf 文件，仍使用原有 `piano-power-button.service`。更换程序后重启该用户服务即可生效；更换内核或设备树后正常重启。其他桌面使用各自的标准电源管理，本目录不作为通用 BSP 的额外守护进程安装。
