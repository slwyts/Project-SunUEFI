# 同一固件的面板选择

Piano 的 BOE 和 CSOT 面板共用同一内核中的 NT36532 驱动，但初始化命令不同。每台设备应使用原厂 ABL 本次识别出的面板，不能把开发机的供应商当成所有设备的默认值。

UEFI 在早期读取原厂 DTB 的 `/chosen/bootargs`，保存匹配结果，交接 Linux 时在已分配、扩容的 DTB 副本上修改面板节点的 `compatible`。输入 DTB、CPU、供电、DSI 端点和其他属性保持原内容。原厂 DTB 不直接作为主线 Linux 的完整设备树使用。

| ABL 的 `msm_drm.dsi_display0` 标识 | Linux 面板配置 |
| --- | --- |
| `qcom,mdss_dsi_p81_42_02_0a_dualdsi_dsc_vid` | `xiaomi,piano-boe-nt36532` |
| `qcom,mdss_dsi_p81_35_02_0b_dualdsi_dsc_vid` | `xiaomi,piano-csot-nt36532` |

标识缺失、未知或重复时不猜供应商，保留输入 DTB 的配置并记录 `PIANO_PANEL_SELECT` 日志。新面板需要补充可靠的原厂标识和对应驱动配置后加入表中。日志同时记录原厂参数的长度和 CRC，便于核对读取来源。

`./build.sh package` 的 `--panel-vendor auto` 保留参考 DTB 中的配置，实际设备选择在 UEFI 交接时完成。打包工具保留显式 `boe` / `csot` 选项供诊断；它不代表能够识别其他设备。

2026-10-08 从 BOOT 普通重启、新 ESP 和 `PIANOROOT` 启动 7.2.9 后，实机 `/proc/device-tree` 读回为 `xiaomi,piano-csot-nt36532`，ESP 输入仍为 BOE，确认 CSOT 自动选择走通。BOE 设备尚未实测。定向主机检查也确认只修改交接副本的目标面板 `compatible`，原始输入不变。

开发机在之前使用 BOE 配置时和切换到 CSOT 后都能正常显示。两者共用 NT36532 驱动及主要显示参数，但供应商初始化命令和 ESD 配置不同；能够出画面不证明两套配置完全等价。自动选择让配置与原厂识别结果一致，不作为已证实的显示故障修复。UEFI 的物理白屏、刷新率切换和 HDR 仍需分别定位。

实现位于 `uefi/handoff/early-memory/PianoColdBootObjects.c`、`uefi/core/PianoPanelSelection.c`；Raw Linux 和 EFI Linux 加载路径均使用同一选择函数。Linux 驱动为 `drivers/gpu/drm/panel/panel-novatek-nt36532.c`。

设备树是固件向内核提供的硬件描述；构建和运行时的修改方式可参见 [Linux 的设备树文档](https://docs.kernel.org/devicetree/usage-model.html)。
