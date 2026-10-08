# 电源键与保护套合盖

本地 GNOME 适配位于 `linux/desktops/gnome/power-overlay/`，复用 `piano-power-button.service` 和同名helper，基于固定 debian-piano `25babfe3ff5d8ddee98b1e0ea88152d69a0c01b1`；不修改upstream，不增加另一个daemon。

当前源码行为：短按切换背光，长按0.8秒打开GNOME关机/重启菜单；真实SW_LID闭盖关背光，开盖恢复此前保存的实际亮度。它只打开准确命名的`pmic_pwrkey`和`soc:piano-hall-ml`，仅grab电源键，hall不独占。hall打开时先用`EVIOCGSW`查询当前状态，所以服务启动前已闭盖也能处理；丢事件后跳过队列至SYN_REPORT，再查询当前开关状态。udev仅给这两个event设备添加当前会话的uaccess权限，不读取触摸或键盘事件。

通过标准logind Session.SetBrightness控制背光，不做DPMS/DRM modeset。保留HandlePowerKey/LongPress/LidSwitch=ignore和idle-delay=0，避免和桌面/系统重复处理。源码已接入完整 GNOME stager 和多发行版 GNOME 组装入口，并部署到当前 Linux。原有服务已重新加载且全局启用，实际 FD 仅指向电源 event3 和 hall event1；未操作触屏/键盘 FIFO。物理合盖、短按和长按仍待现场配合，未执行睡眠，也没有新增模拟测试。

**这还不是完整锁屏或睡眠。** 目标仍是“短按锁屏关屏、长按关机菜单、合盖锁屏关屏后睡眠”。目前短按/合盖不调用Lock或Suspend；锁屏不能用背光0冒充，睡眠暂缺的是底层恢复验证，与是否设置密码无关。

显示侧已准备独立 [关屏时序草案](../../patches/linux/7.2.9/drafts/piano-nt36532-stock-off-delays.patch)：原厂最终DT的BOE/CSOT各模式均要求DCS `0x28` 后20ms、`0x10` 后100ms，当前驱动使用10ms/65ms。草案只把Piano参数恢复为原厂值，未接入当前内核或实机验证。已有PLL/clock警告发生于初始probe；系统关机红屏记录也不是DPMS恢复测试，不能据此把此草案称作黑屏/睡眠修复。还需要同一模式下受控的OFF→ON日志，再处理具体时钟/复位故障。

2026-10-08的a8同模式OFF→ON已有实际故障记录：12:07:49关屏后，DSI1的 `pclk1`、`byte1_intf`、`byte1` 在 `dsi_link_clk_disable_6g` 返回偏移 `+0x34/+0x48/+0x5c` 报stuck-on；实际目标对象反汇编确认它们来自正常 `post_disable` 的第二个 `power_off(host1)`，host0此前已关。12:07:51恢复ON后反复 `status=4` 只证明FIFO错误；日志没有原始FIFO_STATUS子位，不能把它称为PLL失锁。固定[原厂clock manager](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_clk_manager.c#L742)要求先停slave link/core clocks、再停clock master，最后才关PLL。[对应小补丁](../../patches/linux/7.2.9/0008-dsi-bonded-stop-slave-first.patch)仅把正常bonded路径改为host1先、host0后，与已有错误回退路径一致；HALT检查、PLL/PHY保存恢复及面板时序不变。补丁尚未实机验证，不据此启用锁屏或宣称休眠恢复正常。

## 为什么暂不追加 GNOME Lock

对应现场版本的一手源码给出明确链路：

- [GNOME Shell 48.7 `js/ui/screenShield.js`, 603–628行](https://github.com/GNOME/gnome-shell/blob/48.7/js/ui/screenShield.js#L603)：手动Lock不检查`lock-enabled`；调用activate后，PasswordMode不是NONE便设置locked。它没有按AccountsService.Locked拒绝不可解锁账号。551–582行的activate会触发ActiveChanged，源码也说明该signal使gsd blank。
- [gnome-settings-daemon 48.1 `plugins/power/gsd-power-manager.c`, 2251–2267行](https://github.com/GNOME/gnome-settings-daemon/blob/48.1/plugins/power/gsd-power-manager.c#L2251)：收到ActiveChanged(true)立即进入BLANK；1803行调用backlight_disable，1306–1309行释放ALS并设置Mutter PowerSaveMode OFF。1937–1950行另加screensaver blank timer。`idle-delay=0`、`lock-enabled=false`及普通idle inhibitor均不会挡住这条锁屏后的DPMS路径。

现场已只读确认：piano账号Locked=true、PasswordMode=0，GNOME lock-enabled=false、GetActive=false、session1 LockedHint=false。**此时直接发Lock仍可能同时进入认证锁屏和DPMS off**；不能把“lock-enabled=false”理解成安全无动作。owner之后设置密码，显示侧也走同一DPMS路径，仍需先解决原生DSI重新启用问题。本轮未发Lock、未改密码或屏幕锁设置，不修改GSD来掩盖显示驱动问题，也不伪写LockedHint。

logind LockedHint是桌面提供的提示，并不是设置它就完成锁屏：[systemd v257 login1接口](https://github.com/systemd/systemd/blob/v257/man/org.freedesktop.login1.xml#L1317)。设备状态只是列出freeze/mem/disk与deep/s2idle时，不能据此宣称恢复可用。

## 持久接入

Root builder只在GNOME组装时覆盖同名helper/unit、udev、logind和dconf文件；helper0755，其他0644，root所有。使用已有graphical-session.target启用同一服务；dconf更新和udev权限刷新后由正常会话生效。当前BSP通用manifest排除了GNOME power/logind组件，因此不能把本目录悄悄加入通用配置包或其他桌面。完整 userspace stager 复制公开 overlay 后应用这组本地适配；发布 builder 和通用 GNOME 组装使用同一安装函数。实际显示恢复修复并验证后，再接标准Lock/Suspend，不另建功能profile。

后续标准策略已有待验证草案：停用独占evdev的旧helper，恢复GNOME的 `XF86PowerOff` 绑定，由media-keys请求logind Suspend，再由Shell的 `PrepareForSleep` 和delay inhibitor完成锁屏。覆盖原有dconf/logind路径，不能只删源码而让旧文件残留；新增全局用户unit mask后，已运行helper仍要等正常会话退出才释放EVIOCGRAB。当前设备hostname1已报告 `tablet`，不改machine-info或独立的 `vm-other` 检测事实。草案尚未应用，因为当前用户无可用解锁凭据，显示DPMS及完整睡眠恢复尚未验证；默认产品仍保持现有可用行为。
