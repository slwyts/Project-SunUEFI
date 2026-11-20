# 电源键、合盖与睡眠

## 分工

内核上报电源键 `KEY_POWER` 和保护套开关 `SW_LID`，并负责面板恢复和系统唤醒。发行版的 udev 给按键和开关标记 `power-switch`，logind 读取这些事件。GNOME 会话开启时，电源键动作由 GNOME 设置中的「电源」选项决定；没有桌面接管时使用 logind 的配置。合盖动作也遵循发行版的策略，包括外接显示器和应用阻止挂起的情况。

GNOME 使用 `systemd-hostnamed` 报告的设备类型区分平板和虚拟机。产品保留标准 `CHASSIS=tablet` 设置，不用定制的长按关机菜单或单独的背光控制。

## 默认设置

[标准 dconf 配置](../../../linux/desktops/gnome/overlay/etc/dconf/db/local.d/00-power)设置 2 分钟空闲熄屏、使用电池时 5 分钟自动挂起；外接电源时不自动挂起。Plasma 用自己的 PowerDevil 配置实现相同的空闲行为，插电时合盖关闭显示，短按电源切换显示。键盘背光默认 80%，后续沿用用户保存的亮度。用户可在系统设置里修改这些偏好。

GSD 电源管理器启动时先读取 GNOME 已有的锁屏状态，再配置空闲计时和显示电源。会话已锁屏时保持灭屏；查询失败时保留原显示电源状态，避免重启服务把黑画面背后的背光重新点亮。正常输入仍沿用 GNOME 的唤醒与再次灭屏流程，不增加背光轮询服务。

Debian GNOME 和 KDE 镜像都允许挂起到内存（`mem/deep`）。磁盘休眠、混合睡眠及先挂起后休眠保持禁用。

## 内核适配

- 显示 PLL 保存恢复、双 DSI 关闭顺序和 PHY 运行时时钟由 `patches/linux/7.3/0064-EXTERNAL-drm-msm-dsi-bonded-piano.patch` 处理。DPU 停止顺序由 `patches/linux/7.3/0069-EXTERNAL-drm-msm-dpu-video-stop-order.patch` 处理。
- `0029-sm8750-pcie-active-wakeup.patch` 在唤醒链路上保留 PCIe 电源域，避免恢复后 Wi-Fi 的 PCI 配置不可读。
- `0030-soc-qcom-smp2p-sleepstate.patch` 向原厂固件通知 AP 睡眠状态；它不能单独解决全部 ADSP 恢复问题。
- `0032-asoc-q6apm-clear-global-on-remove.patch` 修正 ADSP 重启时的全局对象生命周期。
- `0033-fastrpc-freeze-inflight-invoke.patch` 处理睡眠时等待中的 FastRPC 调用。

设备已完成 RTC 和电源键唤醒。内核在当前充电保护无法跨挂起持续监测时会拒绝挂起。核对完整链路时先拔下充电线，分别测试电源键和合盖进入挂起、开盖或按键唤醒、恢复后锁屏。调试睡眠时以同一启动 ID、恢复错误和触控／网络状态判断结果。

## 从旧镜像升级

安装或更新 `piano-device-config` 会清理旧镜像生成且未修改的助手和配置，并更新未修改的 GNOME 电源默认值，保留用户编辑过或由其他包管理的文件。升级后重启，让系统重新读取 udev、logind 和 GNOME 配置。新装镜像不再包含旧助手。

旧 Debian 镜像可能还屏蔽了普通挂起。用 `systemctl is-enabled sleep.target suspend.target` 查询；如果显示 `masked`，并确认是镜像的默认设置，不是自己禁用的，解除这两项：

```sh
sudo systemctl unmask sleep.target
sudo systemctl unmask suspend.target
```

软件包更新不会解除这两项屏蔽，因为它无法区分旧镜像的默认设置和管理员主动禁用。
