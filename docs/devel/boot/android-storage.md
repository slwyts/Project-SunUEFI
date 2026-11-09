# Android 存储扩容

删除独立存储分区并把空间归还 Android，需要同时完成两件事：扩大 GPT 中 `userdata` 的范围，以及扩大其中的 F2FS 文件系统。仅删除 `sunuefi_esp`、`sunuefi_root`，或只改变分区结束位置，Android 可用容量都不会因此自动增加。

## 加密与容量调整

本机 `/data` 使用 F2FS，挂载在名为 `userdata` 的 device-mapper 设备上。原厂 fstab 同时启用了文件加密（FBE）和元数据加密，vold 在启动期间加载原有密钥并建立解密后的块设备，然后再挂载 `/data`。

因此，分区调整不需要关闭 FBE、重建密钥或格式化 Android。文件系统工具必须使用 vold 建立的解密块设备，不能直接把物理 `userdata` 当作明文 F2FS。模块的只读检查确认该映射只有一个 `userdata` 后端、从零偏移开始；设备映射参数中的密钥不输出、不保存。

F2FS 内核的在线容量调整接口可用于缩小已挂载的 `/data`，不支持在线扩大。扩大容量必须在正常卸载文件系统后运行 `resize.f2fs`。重启可以重建完整映射，但不能保证原厂系统会运行扩容工具。

## 原厂启动期间的扩容

HyperOS 的 `libfs_mgr.so` 包含小米增加的 F2FS 扩容代码，但存在代码、工具或字符串，不表示当前 `/data` 路径会执行它。HyperOS `OS4.0.5.0.XPYCNXM` 对 `/dev/block/dm` 开头的路径跳过 `resize.f2fs`，恰好包括 vold 为元数据加密建立的映射。启动日志只有文件系统检查和挂载，没有扩容调用。

同机二进制的分析依据：

| 文件 | SHA-256 |
| --- | --- |
| `/system/lib64/libfs_mgr.so` | `aa0a9ee08340310992c77df3fad679c51c81761ba4ccddda3311333f9dfd9094` |
| `/system/bin/fsck.f2fs`（`resize.f2fs` 的目标） | `fce9b89bc35ee1d8160007406caa402a00bbbae94d662a1ccddd8d129171dc24` |

ARM64 反汇编在 `0x3d82c` 附近检查设备路径，匹配 `/dev/block/dm` 时跳至 `0x3dc78`，绕过扩容。旧版 HyperOS 的扩容调用记录不能代替当前版本、当前设备路径的验证。模块因此不对 device-mapper 上的 `/data` 提供删除并归还空间，避免先删除系统、随后只能停在待完成状态。

元数据加密的设备建立和挂载顺序见 [AOSP MetadataCrypt.cpp](https://android.googlesource.com/platform/system/vold/+/refs/heads/android16-release/MetadataCrypt.cpp)；`resize.f2fs` 的离线操作和参数见 [AOSP f2fs-tools](https://android.googlesource.com/platform/external/f2fs-tools/+/refs/heads/android16-release/fsck/main.c)。模块不会修改系统库、fstab 或加密配置来强行进入扩容分支。

## 删除并归还空间的顺序

只有独立存储区域紧邻 `userdata` 尾部，且归还范围内没有其他分区时，才能保持起点不变、向后扩大 `userdata`。分区编号不决定物理顺序，必须读取 GPT 的起止地址。

删除过程先要求可用的扩容路径、Android 启动路线和独立分区未挂载，再删除 ESP/root 条目并扩大 `userdata` 的结束位置。预览和执行均检查扩容条件；不满足时不删除分区。原厂系统分区、`metadata`、`userdata` 起点和现有 GUID 均保持不变。

分区表写入后的结果是“等待重启完成 Android 扩容”。模块不会在已挂载的 `/data` 上运行 `resize.f2fs`，也不会提前把这一步显示为容量已经归还。正常启动后，读取物理分区、device-mapper 和两份 F2FS 超级块，确认三者一致后才完成操作。

## 重启后的只读检查

源码位于 [`tools/android/piano_resize_f2fs.c`](../../../tools/android/piano_resize_f2fs.c)。

```sh
piano-resize-f2fs status --expect-bytes 新userdata容量字节数
```

输出中包含：

- `userdata_bytes`：内核当前识别的物理分区容量。
- `mapped_bytes`：Android 解密块设备的容量。
- `f2fs_bytes`：F2FS 超级块中声明的容量。
- `mapping_requires_reboot`：物理分区已扩大，但 Android 仍在使用旧容量映射。
- `grow_pending`：文件系统尚未覆盖新的分区容量，按 F2FS section 对齐保留余量。
- `expansion_verified`：目标容量、物理分区、映射和两份超级块均满足扩容后的要求。

指定目标容量后，完整确认返回 `0`；尚未达到目标返回 `3`，同时保留完整状态输出。此命令仅读取，不修改文件系统、分区表或加密配置。

实机删除 ESP/root 并扩大 GPT 后，物理 `userdata` 和解密映射均为 `495286484992` 字节，F2FS 仍为 `219871686656` 字节，双超级块一致。状态检查正确返回 `3` 并保留待完成记录，没有将分区表扩大误报成 Android 容量已经归还。

## 离线维护的约束

维护必须先停止数据使用者并正常卸载所有相关挂载，再独占打开明文块设备。不能使用 lazy／force 卸载，也不能因普通文件描述符已关闭就假定文件系统离线。SukiSU 的内核包管理监测还会持有 `/data/system` 的挂载引用；其隐藏模块 sysfs 在本机正常卸载模块时会触发内核错误。因此不能靠卸载 Root 内核模块来释放 `/data`。完成启动早期的离线维护接入前，模块保留未完成状态，不自动重试或循环重启。
