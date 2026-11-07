# 原厂 Piano 刷新率切换

本记录对照 MiCode 显示驱动提交
`aa06fd1757c28dce96fbe4e04d4530ec21b52aac` 和同机 Android 最终 DT，
不将通用驱动中存在的分支直接当作本机运行结果。

## 最终 DT 的配置

原厂启动参数选择 `qcom,mdss_dsi_p81_35_02_0b_dualdsi_dsc_vid`。
该节点启用 `mi,video-mode-multi-timing` 和
`qcom,mdss-dsi-pan-enable-dynamic-fps`，更新方式为
`dfps_immediate_porch_mode_vfp`。

三个基础时序组为 120、144、90 Hz。120 Hz 组允许 120 和 60 Hz；
90 Hz 组还允许 50、48、30 Hz。低刷新率通过增加垂直前沿间隔
生成，不是所有刷新率各自重新初始化一次面板。
三个组的 DSI bit clock 均配置为 1,197,500,000 Hz。

CSOT 时序切换命令选择 page 0x10、解锁，然后按 B2、B3 顺序写入：

| 时序组 | B2 | B3 |
| --- | --- | --- |
| 120 Hz（含 60 Hz） | 0x91 | 0x40 |
| 144 Hz | 0x00 | 0x00 |
| 90 Hz | 0x00 | 0x80 |

`qcom,mdss-dsi-timing-switch-video-command-state` 为 `dsi_hs_mode`。
BOE 的命令顺序相反，不能将 CSOT 顺序用于所有面板。

## 代码路径

- [dsi_display.c](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_display.c#L5433)
  的 `dsi_display_dfps_update()` 首先异步更新时钟主控制器的时序，
  随后更新其他控制器。DFPS/VRR 路径设置 timing double buffer，
  更新 host config；不能等同于断开所有显示资源再重新启动。
- [dsi_panel.c](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_panel.c#L6594)
  的 `dsi_panel_post_enable()` 对 P81 发送
  `DSI_CMD_SET_TIMING_VIDEO_SWITCH`。
- [sde_encoder.c](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_encoder.c#L7075)
  的 VRR 提交流程等待视频进入 active 区域，提交新的显示数据，
  随后对 P81 发送该命令。退出 60 Hz 到非 120 Hz 模式还有独立的
  `DSI_CMD_SET_DISP_PEN_CLEAR` 和等待下一次垂直同步步骤。

## Linux 对照

当前 Linux 走完整 modeset，关闭并重新准备面板和显示链路。
将 CSOT B2/B3 顺序与原厂对齐、在 panel enable 中通过 HS 补发
时序命令后，120 Hz 启动正常，命令返回成功；60 Hz 切换仍灰屏。
因此只补发命令未复现原厂的完整动态切换流程。
该实验没有加入产品补丁序列，设备恢复正式模块。

下一步需要同机 Android 运行采集确定选用 DFPS/VRR 分支，
并对照主从控制器的提交、时钟和时序寄存器。Linux 应通过 DRM
标准 modeset 接口实现固定刷新率档位的切换，不能仅增加刷新率按钮。

## 2026-10-13 同机运行采集

在独立 ftrace instance 中开启 `sde:sde_evtlog`，结束后关闭并移除，
没有更改 Android 原有的 tracing instance。
事件中出现 `dsi_display_dfps_update` 进入和退出，
`dsi_display_set_mode_sub` 的模式标志为 `0x10`。
这证明本机使用动态时序更新分支，不能把源码中的分支当作推测。

两轮采集请求 60 Hz，但读取 DRM 当前状态是 50 Hz，
不是 60 Hz。第一轮还误用了横屏尺寸；第二轮按显示管理器登记的
2136×3200 设置偏好后，状态仍是 50 Hz。因此尚未完成准确的
120→60→120 对照，不能按命令请求值报告实测模式。

50 Hz 时序为 807067 kHz，水平总计 3882，垂直总计 4158。
采集的 DSI byte clock 为 149687500 Hz，pixel clock 为
159666666 Hz。原厂静止画面的刷新率策略需要与硬件切换区分开。
后续采集应观察模式并覆盖稳定 60 Hz，避免仅依赖设置项。

临时 peak/min 刷新率、用户显示模式偏好及自动灭屏时间已恢复，
随后通过固件代号匹配的原生请求工具回到 Linux。

## 本地来源校验

源码缓存的 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| dsi_display.c | `100d0074dd68cdbdb205d8296fe97c2ff532d8d6e24b3598c5592e6e9b38b1ef` |
| dsi_panel.c | `b3a2c00531c42465ef377b2c2d19797cd681862e84b60461c06485cf1929c090` |
| sde_encoder.c | `97cacaf4a41455990317a4ce58e521b1c76f0b2edc1644d574a482620e80c09b` |

最终 DT 的本地文本缓存 SHA-256：
`e243e388d76d329dad88ef92d1484de9acc9c6204359fc1d9a3524a4c53362f9`。
