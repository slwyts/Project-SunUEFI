# Android 存储扩容

删除独立存储分区并把空间归还 Android，需要同时完成两件事：扩大 GPT 中 `userdata` 的范围，以及扩大其中的 F2FS 文件系统。仅删除 `sunuefi_esp`、`sunuefi_root`，或只改变分区结束位置，Android 可用容量都不会因此自动增加。

## 加密与容量调整

本机 `/data` 使用 F2FS，挂载在名为 `userdata` 的 device-mapper 设备上。原厂 fstab 同时启用了文件加密（FBE）和元数据加密，vold 在启动期间加载原有密钥并建立解密后的块设备，然后再挂载 `/data`。

因此，分区调整不需要关闭 FBE、重建密钥或格式化 Android。文件系统工具必须使用 vold 建立的解密块设备，不能直接把物理 `userdata` 当作明文 F2FS。模块的只读检查确认该映射只有一个 `userdata` 后端、从零偏移开始；设备映射参数中的密钥不输出、不保存。

F2FS 内核的在线容量调整接口可用于缩小已挂载的 `/data`。扩大容量则由 `resize.f2fs` 在文件系统尚未挂载时完成。这也是删除后的空间归还需要正常重启 Android 的原因。

## 原厂启动期间的扩容

本机 HyperOS `OS3.0.309.0.WPYCNXM` 的 `libfs_mgr.so` 包含小米增加的 F2FS 扩容步骤：

1. vold 根据当前物理 `userdata` 容量重新建立 device-mapper 映射。
2. fs_mgr 在挂载 F2FS 前检查文件系统，随后调用 `/system/bin/resize.f2fs`。
3. `resize.f2fs` 默认使用块设备容量；原厂可根据 UFS 保留区域附加目标容量参数。
4. 扩容完成后，Android 挂载原来的 `/data`，原有文件和加密配置继续使用。

这一步属于小米对 fs_mgr 的修改。[AOSP Android 16 fs_mgr](https://android.googlesource.com/platform/system/core/+/refs/heads/android16-release/fs_mgr/fs_mgr.cpp) 没有相同的 F2FS 自动扩容调用，不能假设所有 Android ROM 都具备它。元数据加密的设备建立和挂载顺序见 [AOSP MetadataCrypt.cpp](https://android.googlesource.com/platform/system/vold/+/refs/heads/android16-release/MetadataCrypt.cpp)；`resize.f2fs` 的离线操作和参数见 [AOSP f2fs-tools](https://android.googlesource.com/platform/external/f2fs-tools/+/refs/heads/android16-release/fsck/main.c)。

当前原厂二进制的分析依据：

| 文件 | SHA-256 |
| --- | --- |
| `/system/lib64/libfs_mgr.so` | `8df27abaeff6846c131efb99df04acea14a2373587a5a68de0276c53e8d56518` |
| `/system/bin/fsck.f2fs`（`resize.f2fs` 的目标） | `5df8fc45c962d7fb0f8ef7f99bea32dd4716e6a92cab32644ea3999a9078b977` |

ARM64 反汇编中，`libfs_mgr.so` 的 F2FS 检查流程在 `0x3b630` 检查 `resize.f2fs` 是否可执行，在 `0x3c6f4` 通过 `logwrap_fork_execvp` 调用它。这个步骤不依赖 fstab 中的 `resize` 标志。已有原厂 Android 启动日志也记录了挂载 `/data` 前执行 `resize.f2fs`；该日志中的一次非正常关机检查导致扩容工具退出，因此“调用过工具”本身仍不能证明扩容成功。

## 删除并归还空间的顺序

只有独立存储区域紧邻 `userdata` 尾部，且归还范围内没有其他分区时，才能保持起点不变、向后扩大 `userdata`。分区编号不决定物理顺序，必须读取实际 GPT 的起止地址。

删除过程先要求 Android 启动路线和独立分区未挂载，再删除 ESP/root 条目并扩大 `userdata` 的结束位置。原厂系统分区、`metadata`、`userdata` 起点和现有 GUID 均保持不变。

分区表写入后的结果是“等待重启完成 Android 扩容”。模块不会在已挂载的 `/data` 上运行 `resize.f2fs`，也不会提前把这一步显示为容量已经归还。正常启动后，读取物理分区、device-mapper 和两份 F2FS 超级块，确认三者一致后才完成操作。

## 重启后的只读检查

源码位于 [`tools/android/piano_resize_f2fs.c`](../../tools/android/piano_resize_f2fs.c)。

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

2026-10-10 的实机只读结果确认现有 `userdata` 的物理容量和映射容量均为 `426567008256` 字节，F2FS 为 `426566987776` 字节，两份超级块一致。新容量与现有容量不同的检查会返回未完成。删除后向 Android 归还完整区域的实机扩容尚未执行。
