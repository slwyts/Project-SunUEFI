# 同一固件的面板选择

Piano 的 BOE 和 CSOT 面板共用同一内核中的 NT36532 驱动，但初始化命令不同。每台设备应使用原厂 ABL 本次识别出的面板，不能把开发机的供应商当成所有设备的默认值。

UEFI 在早期读取原厂 DTB 的 `/chosen/bootargs`，保存匹配结果，交接 Linux 时在已分配、扩容的 DTB 副本上修改面板节点的 `compatible`。输入 DTB、CPU、供电、DSI 端点和其他属性保持原内容。原厂 DTB 不直接作为主线 Linux 的完整设备树使用。

| ABL 的 `msm_drm.dsi_display0` 标识 | Linux 面板配置 |
| --- | --- |
| `qcom,mdss_dsi_p81_42_02_0a_dualdsi_dsc_vid` | `xiaomi,piano-boe-nt36532` |
| `qcom,mdss_dsi_p81_35_02_0b_dualdsi_dsc_vid` | `xiaomi,piano-csot-nt36532` |

标识缺失、未知或重复时不猜供应商，保留输入 DTB 的配置并记录 `PIANO_PANEL_SELECT` 日志。新面板需要补充可靠的原厂标识和对应驱动配置后加入表中。日志同时记录原厂参数的长度和 CRC，便于核对读取来源。

`./build.sh package` 的 `--panel-vendor auto` 保留参考 DTB 中的配置，实际设备选择在 UEFI 交接时完成。打包工具保留显式 `boe` / `csot` 选项供诊断；它不代表能够识别其他设备。

定向 host 检查使用真实 1.1 MiB 原厂 Android DTB，经受限 SEC collector、typed HOB 和 DXE 缓存识别 CSOT，再修改实际 Linux DTB 的交接副本。同步遍历 5804 个节点及其全部属性，确认只有一个 Piano `compatible` 改变，fallback 字符串与原输入字节保持原值；未知选择保持输入配置。

目前已从基准机的原厂 Android 启动参数确认 CSOT。产品构建和实机启动验证仍需完成；尚不能据此宣布 BOE 和 CSOT 两种设备都已实测。该选择也不等同于已修复 UEFI 的物理白屏、刷新率切换或 HDR。

实现位于 `uefi/handoff/early-memory/PianoColdBootObjects.c`、`uefi/core/PianoPanelSelection.c`；Raw Linux 和 EFI Linux 加载路径均使用同一选择函数。Linux 驱动为 `drivers/gpu/drm/panel/panel-novatek-nt36532.c`。

设备树是固件向内核提供的硬件描述；构建和运行时的修改方式可参见 [Linux 的设备树文档](https://docs.kernel.org/devicetree/usage-model.html)。
