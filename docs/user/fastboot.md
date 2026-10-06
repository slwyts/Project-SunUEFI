# 驻留 Fastboot 调试端使用速查

SunUEFI 固件在运行期间内置了轻量级 USB Fastboot 调试服务。它可以在白屏或没有图形交互的情况下，通过电脑终端协助开发者和玩家排查问题、抓取日志甚至启动系统。

> ⚠️ **重要提示**：在固件运行时，设备向电脑暴露的标识为 **`SunUEFI-piano`**。请注意区分它与原机 Bootloader（返回真实数字序列号）的不同。

---

## 🛠️ 1. 基础连接与状态查询

在电脑终端中执行以下命令（所有命令均带 `-s SunUEFI-piano` 前缀）：

```sh
# 确认连接状态（应返回 piano-sunuefi）
fastboot -s SunUEFI-piano getvar product

# 查看协议版本与存储保护策略
fastboot -s SunUEFI-piano getvar version
fastboot -s SunUEFI-piano getvar storage-policy

# 查看综合状态摘要
fastboot -s SunUEFI-piano oem status
```

---

## 📸 2. 导出运行日志与屏幕截图

固件在内存（RAM）中维护了环形日志缓冲区与当前 Framebuffer 显存映射。即使物理屏幕白屏，可以尝试导出显存中的内容；是否成功仍需检查返回结果。

### 导出日志快照（最大 256 KiB）
```sh
# 冻结并暂存当前日志
fastboot -s SunUEFI-piano oem ramlog

# 将暂存日志取回电脑保存为文本文件
fastboot -s SunUEFI-piano get_staged uefi-ramlog.txt
```

### 导出当前显存画面截图（BMP 格式）
```sh
# 请求固件将当前 GOP 显存封装为 BMP
fastboot -s SunUEFI-piano oem screenshot

# 取回截图文件
fastboot -s SunUEFI-piano get_staged uefi-screenshot.bmp
```
*导出的 `uefi-screenshot.bmp` 是标准的 24 位位图文件，可直接用电脑图片查看器打开。*

---

## 🚀 3. 系统调度与控制命令

| 终端命令 | 说明 |
| :--- | :--- |
| `fastboot -s SunUEFI-piano oem boot-stable` | **引导已安装的 Linux**：从平板内部 ESP 加载内核，请求启动已部署的 Stable，实际结果取决于文件和当前固件状态 |
| `fastboot -s SunUEFI-piano oem setup` | 切换进入标准 UEFI BIOS 设置界面 (UiApp) |
| `fastboot -s SunUEFI-piano oem shell` | 切换进入 UEFI Shell 命令行环境 |
| `fastboot -s SunUEFI-piano oem simpleinit` | 切换回 SimpleInit 图形菜单主界面 |
| `fastboot -s SunUEFI-piano reboot` | **退出**：正常清理路径冷重启回 Android |

---

## 🔒 4. 固件的安全限制

为了防止误操作损坏平板，SunUEFI 的 Fastboot 调试端在底层设置了安全红线：

* ❌ **完全拒绝所有刷写/擦除指令**：`flash`、`erase`、`flashing unlock` 等直接操作分区的命令均被底层阻断并返回拒绝。
* ❌ **内存下载上限限制**：当前仅允许最大 64 MiB 的测试内存载荷传输。

`continue` 当前也执行冷重启，不继续启动 Linux。一般 `boot 文件` 尚不是通用系统/EFI 引导入口；只读 `fetch` 的设备请求上限为 64 KiB。菜单切换命令已接入，但当前物理画面和所有界面后台服务尚未联合验证。
