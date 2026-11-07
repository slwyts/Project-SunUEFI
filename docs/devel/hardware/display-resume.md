# SM8750 双路 DSI 与刷新率切换

Piano 使用两路 DSI 驱动同一块面板，默认 120 Hz。30／48／50／60／120 Hz 采用无缝切换，90／144 Hz 按不同水平时序执行完整显示重设。刷新率切换及 144 Hz 显示和笔输入可用；完整重设是一条正常切换路径，不等于切换失败。

## 无缝切换

原厂 BOE、CSOT 设备树都提供基于垂直前沿空白的动态刷新率。`0035-panel-piano-low-refresh-rates.patch` 加入 30、48、50 Hz，与 60 Hz 一样沿用 120 Hz 的时钟和水平时序，通过延长每帧的空白时间降频。

`0037-drm-piano-seamless-vfp.patch` 在标准 atomic modeset 中识别同一时序族：更新 DSI 双缓冲时序和 DPU 接口的帧周期，在帧边界提交。面板电源、DSC 和触控固件保持运行；正常 Gamma 更新可以同时提交。改变水平时序、链路时钟或 CTM 路由时仍执行完整重设。

设备已反复完成 30／48／50／60／120 Hz 固定档位之间的无缝切换，触控固件 epoch 保持不变；稳定帧中断频率与请求档位相符，没有新增 underrun。按视频帧率或内容空闲自动选择档位仍需桌面和驱动配合。

## 关闭和恢复显示

以下修正共同处理之前的黑屏、灰屏和关闭时彩色残影：

| 补丁 | 作用 |
| --- | --- |
| `0024-dsi-pll-defer-unprogrammed-clock.patch` | 避免注册时钟提供者时提前启动未配置的 PLL |
| `0025-dsi-sm8750-restore-bonded-slave.patch` | 保存并恢复主、从两路 PHY 的分频状态 |
| `0026-dpu-video-stop-order.patch` | 在停止视频时序前清理 DPU 流水线，使清理能在帧边界完成 |
| `0034-dsi-phy-runtime-interface-clock.patch` | 通过 runtime PM 管理 PHY 接口时钟 |
| `0036-backlight-ktz8866-wait-disable-ramp.patch` | 等待两颗背光芯片渐灭完成，再关闭面板和视频链路 |
| `0038-panel-piano-vendor-refresh-order.patch` | 保留 CSOT 的 B2→B3 和 BOE 的 B3→B2 原厂命令顺序 |

PLL 注册、DPU 停止顺序和 PHY 接口时钟补丁分别参考 Junhao Xie 的 [67b3cbd4bab](https://github.com/BigfootACA/linux/commit/67b3cbd4bab5687c3aa3c9fe5a14c37433e28bbf)、[8f353c130fee](https://github.com/BigfootACA/linux/commit/8f353c130fee0fcf09afbe10e7fc58f901931418)、[b95037aef0a0](https://github.com/BigfootACA/linux/commit/b95037aef0a0bf5cd612e7517a67644887e9fd5d)，保留原作者署名。

正常启动、120 Hz 下关闭／恢复显示、RTC 和电源键唤醒已有实机结果。显示恢复后可以继续操作，触控和网络可恢复；这不等同于待机耗电已达到原厂水平。整机睡眠的其他驱动处理见[电源管理](power-button.md)。

## 144 Hz 与时序差异

90／144 Hz 与 120 Hz 的水平时序不同，不能直接套用上述 VFP 更新。之前的花屏和笔输入问题已解决；原厂命令顺序、双路状态恢复与触控扫描协调保留。历史故障用于解释实现，不再列为当前使用限制。

原厂切换流程见[原厂刷新率分析](oem-refresh.md)；显示位深、HDR 和 FRC 的边界见[色彩处理](display-depth.md)。
