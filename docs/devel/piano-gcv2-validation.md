# GCv2 最小实机检查

这份步骤供主任务在正常默认内核构建完成、恢复包可用之后执行。bde3已实测恒等Gamma的硬件提交与完成，但未修补的Mutter48.7在恢复原空LUT时使用户会话SEGV；当前客户端已阻止这种写入。温色先通过GNOME标准Night Light入口检查，不能将恒等DMA成功扩大为全部颜色路径可用。

## 只读起点

1. 对照本次完整构建manifest、已安装的实际MSM模块hash与加载状态。只看`uname -r`无法区分同为7.2.9的a8和新包。读实际final DT确认DPU有`mdp/vbif/regdma`，第三资源为`0x0af80000/0x7000`。
2. 保存原始`dmesg --color=never`和当前DPU DRM节点的属性。节点由实际驱动绑定确定，不假定card0。已有`piano-drm-snapshot /dev/dri/cardN --blobs`只用只读DRM ioctl；保存原始JSON，包括错误计数。
3. 在实际活动GNOME用户的session bus下运行`piano_gamma_probe.py`，不带`--probe`。核对真实CRTC API ID、KMS ID、活动模式与三个Gamma数组。活动CRTC应有1024项`GAMMA_LUT_SIZE`；缺少此属性或size不匹配时停止，不强开GNOME Night Light开关。

```sh
# 主任务在guest实际活动GNOME会话环境执行；默认只读。
python3 /run/piano_gamma_probe.py > /tmp/gcv2-before-mutter.json
/run/piano-drm-snapshot /dev/dri/cardN --blobs > /tmp/gcv2-before-drm.json
dmesg --color=never > /tmp/gcv2-before-kernel.txt
```

`/run`是临时部署路径，源文件是[Gamma客户端](../../tools/host/piano_gamma_probe.py)和[只读DRM采集器](../../tools/android/piano_drm_snapshot.c)。脚本使用[Mutter48.7实际DisplayConfig接口](https://raw.githubusercontent.com/GNOME/mutter/48.7/data/dbus-interfaces/org.gnome.Mutter.DisplayConfig.xml)的`GetResources`、`GetCrtcGamma`和`SetCrtcGamma`；API ID来自当前配置，不能拿DRM对象ID直接代入。

**三个空数组可以表示原本的bypass，不能据此安全恢复。** bde3初始CRTC104/105的`GAMMA_LUT_SIZE=1024`、`GAMMA_LUT=0`，活动CRTC104对应Mutter API ID0。Mutter48.7 [read_crtc_gamma](https://github.com/GNOME/mutter/blob/48.7/src/backends/native/meta-kms-crtc.c#L147)先记录支持的size，在blob0时保留NULL；[getter](https://github.com/GNOME/mutter/blob/48.7/src/backends/native/meta-crtc-kms.c#L98)把NULL返回为size0 LUT。真实DRM snapshot可解释后端容量，不能证明完整SetCrtcGamma路径能恢复空数组。当前客户端在原状态为空时拒绝probe，`--restore-from`也拒绝空备份；不会以恒等曲线冒称恢复原bypass。

bde3恒等提交在boottime659.136439秒记录`ctl1 dspp_mask0x60000 iova0x1000 words1554`，659.136542秒记录`completed status0x8`，Set/Get读回一致。约3秒后的空恢复使UID1000的GNOME Shell以`status11/SEGV`退出，GDM重新建立会话；这次日志没有kernel Oops或REGDMA timeout。源码可达路径是SetCrtcGamma缓存非NULL的size0对象，[meta_kms_update_set_crtc_gamma](https://github.com/GNOME/mutter/blob/48.7/src/backends/native/meta-kms-update.c#L554)将其送入[meta_gamma_lut_copy_to_size](https://github.com/GNOME/mutter/blob/48.7/src/backends/meta-crtc.c#L278)，后者用零size计算slots并访问空数组的`i-1`项。未取得core stack，不能把源码定位写成已确认的崩溃栈。

[Mutter最小修复](../../linux/desktops/gnome/patches/mutter/0001-kms-empty-gamma-bypass.patch)在KMS更新入口将size0与NULL一同表示为bypass，非空曲线仍走原重采样。适用官方48.7；当前root记录`libmutter-16-0 48.7-0+deb13u1`、`gnome-shell 48.7-0+deb13u2`。只完成源码补丁及固定官方文件上的fuzz0应用检查，未构建正常Debian包、未实机验证修复后的空恢复。内核atomic层可写blob0这一事实不替代上层完整验证，也无需为此新增legacy gamma回调。

当前后端在真实提交与完成处有`drm_dbg_kms`日志，但没有成功初始化、逐DSPP绑定和健康退出的正向日志。`GAMMA_LUT`出现可以说明代码已安装可用ops，无法单独说明每个物理DSPP的输出结果。此前独立[日志草案](../../patches/linux/drafts/0003-drm-msm-dpu-log-gcv2-init-bind-quiesce.patch)基于旧281的CTL枚举循环，未加入a589；正式0006本次修复改为catalog循环后，该草案不可直接应用，须先更新再正常编译。所需正向消息仍应只在真实reset成功、资源初始化、实际ops绑定和quiesce完成之后输出，不增加寄存器读取、假完成事件或新控制路径。

主任务可在测试窗口保存当前`/sys/module/drm/parameters/debug`值，仅增加KMS bit`0x04`，结束时恢复原值。不开启该bit时没有提交debug消息不能被当作没有DMA；已有error日志仍应保留。

## 标准Night Light入口

GSD48.1的[NightLightPreview](https://github.com/GNOME/gnome-settings-daemon/blob/48.1/plugins/color/gsd-color-manager.c#L284)临时使用当前`night-light-temperature`；超时后恢复正常策略。[set_forced](https://github.com/GNOME/gnome-settings-daemon/blob/48.1/plugins/color/gsd-night-light.c#L573)在夜灯关闭时将Temperature恢复6500K。Mutter的[update_white_point](https://github.com/GNOME/mutter/blob/48.7/src/backends/meta-color-device.c#L1384)使用真实Gamma容量，[generate_gamma_lut](https://github.com/GNOME/mutter/blob/48.7/src/backends/meta-color-profile.c#L502)在有无VCGT两条路径都生成非空数组，不从GetCrtcGamma的空数组推size0。需要已ready的assigned color profile；缺少profile时源码直接不更新Gamma。`NightLightSupported=true`只说明Gamma容量可用。

主任务可在用户准备观察且实际GNOME会话存活时执行一次标准预览，不改变夜灯启用或时间设置：

```sh
gdbus call --session --dest org.gnome.SettingsDaemon.Color \
  --object-path /org/gnome/SettingsDaemon/Color \
  --method org.gnome.SettingsDaemon.Color.NightLightPreview 3
```

调用环境必须是实际活动用户的session bus。预览结束后的6500K是非空的正常颜色曲线，可能包含profile校准；不等于恢复最初`GAMMA_LUT=0`。是否真正变暖仍看实际面板和新硬件完成记录。

bde3后续一次标准10秒预览已accepted，Temperature由6500过渡到6395，但during/after的Gamma blob仍为0，没有新REGDMA。实际colord的DSI设备已Enabled且Embedded，却没有关联profile；包与服务齐全。官方48.7 [ensure_device_profile](https://github.com/GNOME/mutter/blob/48.7/src/backends/meta-color-store.c#L492)在没有EDID checksum时提前return FALSE，未进入已有的无EDID sRGB fallback；无default profile使assigned profile为空，白点更新直接返回。正常夜灯硬件更新尚未完成。最小源码方向是以真实device ID建立无EDID缓存，给标准sRGB fallback补实际Filename和MAPPING_device_id，沿colord原有soft关联形成default profile；不能通过伪EDID或强开UI代替。

## 非空备份的恒等后温色

以下直接Gamma检查只适用于当前三个原始数组均为1024项的状态；原状态为空时客户端拒写，须等待正常Mutter修复包及空恢复验证。先检查现有Night Light和其他颜色客户端状态是否稳定，避免两个客户端同时改Gamma。分别执行恒等、温色两个窗口，每个都备份真实原始Gamma并在`finally`恢复。以下`ACTUAL_ID`来自只读列表，两个backup路径必须尚不存在：

```sh
python3 /run/piano_gamma_probe.py --crtc ACTUAL_ID --probe identity \
  --drm-snapshot /tmp/gcv2-before-drm.json \
  --backup /tmp/gcv2-identity-original.json --hold-seconds 10 \
  > /tmp/gcv2-identity-mutter.json
python3 /run/piano_gamma_probe.py --crtc ACTUAL_ID --probe warm \
  --drm-snapshot /tmp/gcv2-before-drm.json \
  --backup /tmp/gcv2-warm-original.json --hold-seconds 10 \
  > /tmp/gcv2-warm-mutter.json
```

两个窗口间先检查恒等结果、恢复记录和内核日志，没有故障再进行温色。温色是明确定义的RGB增益`1.0/0.8/0.6`，用来检验硬件通路，不是校准后的某个Kelvin色温。在每个hold窗口内另行保存一次只读DRM blob和dmesg；报告里的`start_uptime`可筛选这一窗口的新内核消息。脚本收到正常终止信号也尝试恢复；进程被强杀或DBus失联时保留backup，恢复命令为：

```sh
python3 /run/piano_gamma_probe.py --restore-from /tmp/gcv2-warm-original.json
```

恢复只接受非空备份，重新查当前配置，只匹配原来的实际KMS CRTC。目标已不活动时明确失败，不向猜测的输出写入。软件读回不一致或恢复失败返回status2；status0仍只表示接口往返和软件读回完成。

## 同时看四类结果

离线[报告脚本](../../tools/host/piano_gcv2_report.py)解析实际采集，不修改系统：

```sh
python3 tools/host/piano_gcv2_report.py \
  --snapshot ACTUAL_DRM_JSON --kernel-log ACTUAL_DMESG_TEXT \
  --since-uptime ACTUAL_PROBE_START
```

- **软件属性**：实际CRTC的8192字节blob包含1024个RGB/reserved记录，恒等与温色变化符合请求，非空备份恢复回原曲线。原bypass的blob0恢复需Mutter修复包单独验证。JSON有错误则保留原错误，不计作完整采集。
- **真实DMA**：本次提交日志的`ctl`、非零且包含全部assigned DSPP的`dspp_mask`、IOVA和word数，与随后真实`REGDMA completed`消息配对。bits17..20对应物理DSPP0..3，不预设本次一定使用哪两个DSPP。没有completion或出现timeout/SMMU fault均不能算成功；日志中的done来自硬件轮询。
- **两侧输出**：直接看实际面板，恒等不应改变色彩；温色应覆盖两侧、交界无不同步，恢复后回到原色。物理mixer分界要结合实际资源分配与当前屏幕旋转确认。桌面截图、PipeWire和EFI framebuffer导出发生在硬件Gamma之前，不能证明面板已经变暖。本轮没有新增画面覆盖或软件着色替代硬件。
- **退出**：主任务确认健康颜色更新后，再按已有恢复入口进行一次正常返回Android/重启。收集上一启动日志或pstore检查quiesce与fault。日志补丁的`quiesce complete inflight=0`必须在真正退出处出现；没有该日志的现版不能虚称正向退出日志齐全。

第一次颜色检查不注入SMMU故障、不任意读写MMIO、不测试sysfs解绑、DPMS或睡眠。若真实reset持续不ack，退出路径会保留映射并继续等待，后续component/devres回收不能继续；这不是一次成功恢复。恢复操作由主任务依据实际设备状态执行，可能需要平台强制重启。

完成这轮以后再用GNOME的标准Night Light确认桌面联动。GC2本轮使用常规10bit LUT；不能把温色变化扩大解释为HDR、IGC5、12bit FRC或面板高精度校准已完成。
