# Recovery 持久启动的加载问题

当前原厂 Recovery 已恢复，Android 正常启动。独立 UEFI Recovery 尚未成功；不要把 Fastboot 的 `Writing ... OKAY` 当作启动成功。

这次通过只读读取原厂 `blackbox` 中的 ABL 日志，区分了三个失败阶段：

| 镜像 | 实际错误 | 所处阶段 |
| --- | --- | --- |
| 没有有效 AVB 尾部的早期镜像 | `ERROR_INVALID_METADATA` | AVB 元数据解析失败 |
| 带有效 AVB 尾部的 v3/v4 镜像 | `Err: line:1752 LoadImageAndAuthVB2() status: Not Found` | 查找已加载的 `vendor_boot` 失败 |
| v2、自带 DTB、带有效 AVB 尾部的镜像 | `ERROR: BootLinux: Get pvmfw Image failed!` | 通过镜像认证和加载后，缺少已加载的 `pvmfw` |

v2 日志同时记录 `Authenticate complete! boot state is: orange` 和 `Loaded Partition: recovery`。因此它解决了前一个加载错误，但没有完成启动。三个版本均未证明进入本次构建的 UEFI。

## 二进制定位

本次 ABL PE 的 SHA256 为 `3c40148ca732e962c1f2593619aeaf83b56850e21871c76f8859fba702aef9cd`。以下地址为该 PE 的 RVA，只适用于这份固件：

* `0x3b530`：Recovery 主镜像的 header version 小于 3 时，跳过 `vendor_boot` 查找。
* `0x3b538–0x3b54c`：以字符串 `vendor_boot`（`0x99731`）调用 `GetLoadedImageData`。
* `0x3b590`：为上述失败记录源代码行号 1752。
* `0x78c4–0x78e4`：若启动信息标记启用 pVM 固件，则查找 `pvmfw`。
* `0x794c–0x7970`：查找失败后记录 `Get pvmfw Image failed!` 并退出启动。

原厂 Recovery 的 v4 header 中 kernel size 为 0，会借用 boot 分区的内核。实验镜像含自己的 UEFI kernel，进入了不同的加载路径。修复需要让这条路径正确请求并加载所需的原厂配套镜像；仅更换 header version 不够。

独立 Recovery 请求构建位于 `0x3ae54–0x3ae9c`，这里只加入 `recovery`。正常启动路径在 `0x3ab44` 加入 `vendor_boot`，在 `0x3abfc` 加入 `pvmfw`。此外，Recovery header 补充扫描中的 `0x3b414` 原始字节 `a80000b5` 是 `CBNZ x8`：此时 x8 是分区名指针，非空有效名字被跳过。没有预装主镜像时，不能依赖这个扫描自动补全加载请求。

`OK_NOT_SIGNED` 在本次解锁设备上没有立即中止加载。现有证据不能支持“只要禁用 AVB 就一定修好”，也不应因此修改 ABL、XBL 或破坏 pvmfw。配套镜像存在于磁盘上，与它们出现在 ABL 的已加载镜像列表里，是两件事。

正常 boot 入口也不能立即作为双系统安装方案：修改 boot 会改变默认开机路径。目前 `PianoRawLinuxBoot.c` 的执行后端接受带显式 DTB 的 v2 Linux 包，不直接接受原厂 v4 boot/vendor_boot/init_boot 组合。必须先解决应用载荷在这条路径上的传递，并验证从 UEFI 返回原厂 Android，才应考虑部署。当前没有修改 boot、vbmeta、ABL、XBL 或 pvmfw。

正常 boot 的请求列表在 AVB 调用前构建：`0x3aae4` 加入 boot，header ≥3 时 `0x3ab38–0x3ab50` 加入 vendor_boot，header ≥4 且有 init_boot 时 `0x3abcc–0x3abec` 加入 init_boot；有 pvmfw 时 `0x3abf0–0x3ac04` 加入 pvmfw，后者没有 header 版本门槛。因而正常 v2/v3 boot 路径值得继续评估，但本次没有进行该入口的刷写验证。

v4 还有另一项明确区别：ABL 会在 `0x756c–0x7588` 检查 init_boot 存在标志和 boot header 版本，然后在 `0x75a0` 查询 init_boot，用它的 header/ramdisk 替代 boot 镜像的 ramdisk。给 v4 boot 本身写入 APPv1 载荷不能关闭这个标志。v3 不进入该替换分支；选择版本必须依据入口的完整流程，而不能统一假定“越新越兼容”。

## 读取失败日志

设备回到已 root 的 Android 后执行：

```sh
python3 tools/collect_piano_bootfail.py \
  --serial <ADB序列号> \
  --output private/analysis/recovery-failure-新编号
```

工具检查黑匣子头、日志环和偏移边界，只读取 ABL 的失败日志记录，不写设备，也不采集 Android minidump。输出包含原始日志、文本和每条记录的 boot index；用 boot index 区分新故障和历史记录。

当前工具已在该设备的 15 槽日志环上读回真实记录。完整私有记录在 `private/analysis/recovery-abl-failures-20261007/`；公开文档只保留错误与定位信息。

原厂 pvmfw 的用途与 ABL 加载约定参见 [AOSP Protected Virtual Machine Firmware](https://android.googlesource.com/platform/packages/modules/Virtualization/+/refs/heads/android16-release/guest/pvmfw/README.md)。本项目目前没有验证从 UEFI 进入 Linux 后使用这套虚拟化能力。
