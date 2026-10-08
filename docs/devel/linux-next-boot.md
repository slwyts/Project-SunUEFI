# Linux 下次启动入口

`piano-next-boot` 使用现有合体 BOOT 请求页选择 Android、启动菜单、Linux 或 UEFI 设置。默认只保存下次路线；`--reboot` 才请求 systemd 正常重启。本工具不重包内核、不切槽、不写 misc/PMIC，也不增加后台服务。

当前选择会持续保留，后续正常开机继续沿用，直到改选其他路线。一次性请求自动消费尚未实现。原厂 Recovery 重启保持原语义。

已部署到研究设备并完成配置、只读 status 和当前桌面管理员的 polkit 授权查询；新入口的实际重启验证仍待执行。菜单/Setup路线仍须与实际镜像前置选择器行为核对，不能因为helper保存成功就宣称菜单已显示。

## 使用

```sh
piano-next-boot status
piano-next-boot android          # 保存下次进入原厂 Android
piano-next-boot menu             # 保存下次进入启动选择菜单
piano-next-boot android --reboot # 保存成功后正常重启
```

普通用户通过pkexec调用这一次操作。新增的有限polkit规则允许本地、当前活动会话中的sudo或wheel管理员免密码使用此启动action；其他用户仍须管理员认证。piano账号没有工厂密码，本入口不会修改或解锁账号，不增加通用sudo免密或强制锁屏流程。root可直接调用同一CLI。

2026-10-08 对当前 g086 只读检查：piano 在 sudo 组，密码locked；`sudo -l -U piano` 仅 `(ALL : ALL) ALL`，没有NOPASSWD，`/etc/sudoers.d`只有README。polkit默认管理员为sudo组。单独auth_admin会让未设置密码的owner无法使用按钮，因此本任务明确新增`49-piano-boot-request.rules`这一有限能力，不把它描述为已有NOPASSWD继承。规则只匹配`org.sunuefi.boot-request`，并同时要求local、active和sudo/wheel组；其他用户/action由原policy处理。规则源码已完成，安装和当前会话实际授权检查由root执行；本轮没有改guest权限或测试BOOT写入。

两个`.desktop`入口为“返回 Android”和“SunUEFI 启动选择菜单”。按钮先显示GTK确认对话框，提醒保存当前工作，再走相同CLI/helper。取消不保存请求；失败显示错误。桌面环境需要标准polkit认证代理及Python GI/GTK3，CLI仅需Python标准库、pkexec和原生工具。

## 安装配置

安装器/provisioning在合体镜像安装成功并读回后，必须把**实际安装分区和该镜像APP generation**记录为root所有、普通用户不可改写的`/etc/piano/boot-request.json`：

```json
{
  "version": 1,
  "boot_device": "实际 /dev/disk/by-partlabel/boot_a 或 boot_b",
  "app_generation": "实际 16-byte APP generation，32位十六进制"
}
```

上面是字段说明，不能直接当有效配置。仓库的`boot-request.example.json`采用null占位，发布镜像不应预填某台测试设备的boot_a。缺配置/分区不存在/generation不同均明确报错，不扫描分区、读current-slot猜测或自动改到另一槽。之后更新合体BOOT时，安装器须同步更新generation。

APP generation为SPLITv1元数据+96处16字节，也是NEXTv1+40处的绑定值。原生`piano-boot-request status --device ...`新增`app_generation` JSON字段，便于安装读回核对。`set/consume --expect-generation HEX32`在最初定位后与打开可写FD、重新定位时两次核对该值；不匹配时不写请求页。旧参数仍兼容，但产品helper总是传pin，不重复实现SPLIT parser或CRC。

Android=0、menu=1、Linux=2、Setup=3；设置Android以更高sequence写target0，取消旧Linux请求。共享CRC、双请求页、fsync/readback和原块设备ro恢复仍由原生工具负责。

## 打包位置

| 仓库源 | guest安装位置 |
| --- | --- |
| `linux/userspace/piano-next-boot` | `/usr/bin/piano-next-boot`，0755 root |
| `linux/userspace/piano-next-boot-helper` | `/usr/libexec/piano-next-boot-helper`，0755 root |
| `linux/userspace/piano-next-boot-configure` | `/usr/sbin/piano-next-boot-configure`，0755 root，仅安装器/root设置配置 |
| 原生 `piano-boot-request` ELF | `/usr/local/sbin/piano-boot-request`，0755 root |
| `org.sunuefi.boot-request.policy` | `/usr/share/polkit-1/actions/org.sunuefi.boot-request.policy`，0644 root |
| `49-piano-boot-request.rules` | `/usr/share/polkit-1/rules.d/49-piano-boot-request.rules`，0644 root |
| 两份 `org.sunuefi.*.desktop` | `/usr/share/applications/`，0644 root |
| `boot-request.example.json` | `/usr/share/doc/piano-next-boot/boot-request.example.json`，仅示例 |

polkit action只绑定固定helper executable；有限规则仅放行local+active+sudo/wheel管理员，其他active用户仍要求auth_admin，inactive/any拒绝。不保留长期授权。helper只接受四个固定target/status及可选--reboot，不接受外部device/config/binary/任意命令。配置只容许实际boot_a/boot_b partlabel并验证为块设备；原生工具实际检查该分区内的SunUEFI布局、request CRC及generation。helper不是setuid，不需新daemon。卸载该rules文件即可回到全体active用户auth_admin；不会改变sudoers或账号密码。

release root builder 已接入这些文件、静态原生命令及 Python GI/GTK3、pkexec 依赖，不预填设备配置。安装器或受信任 root 终端应明确指定已安装入口：

```sh
piano-next-boot-configure --device /dev/disk/by-partlabel/实际已安装的boot分区
piano-next-boot status
```

配置命令从实际分区读回 APP generation，原子保存配置，不写 BOOT 请求；它没有扫描当前槽或覆盖原厂镜像。2026-10-08 已在研究设备部署，实际 boot_a 配置与只读 status 匹配，当前桌面管理员的 polkit 授权查询返回成功。尚未通过新入口执行重启，UI 预览仍在准备，不能把这些检查当作完整切换成功。

本轮必要检查仅三例真实C regular-file执行：stale generation拒绝且文件未变、status/set/consume共享CRC并仅请求头改变、坏hex/非法helper参数提前拒绝。另检查Python语法、desktop格式、polkit XML精确绑定；没有设备写测试。
