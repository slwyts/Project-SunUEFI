# Android 存储扩容

删除独立存储分区并把空间归还 Android，需要扩大 GPT 分区表中 `userdata` 的范围，再扩大其中的 F2FS 文件系统。引导助手通过 SukiSU 的早期 init 入口完成第二步，不依赖原厂自动扩容。仅删除 `sunuefi_esp`、`sunuefi_root`，或只改变分区结束位置，Android 可用容量都不会自动增加。

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

ARM64 反汇编在 `0x3d82c` 附近检查设备路径，匹配 `/dev/block/dm` 时跳至 `0x3dc78`，绕过扩容。旧版 HyperOS 的扩容调用记录不能代替当前版本、当前设备路径的验证。模块使用独立的早期维护入口，不修改系统库、fstab 或加密配置来强行进入原厂扩容分支。

元数据加密的设备建立和挂载顺序见 [AOSP MetadataCrypt.cpp](https://android.googlesource.com/platform/system/vold/+/refs/heads/android16-release/MetadataCrypt.cpp)；`resize.f2fs` 的离线操作和参数见 [AOSP f2fs-tools](https://android.googlesource.com/platform/external/f2fs-tools/+/refs/heads/android16-release/fsck/main.c)。

## SukiSU 早期维护

模块的 `initrc/storage.rc` 导入 `/metadata/sunuefi/storage-early.rc`。SukiSU 将模块 init 配置拼入第二阶段 init，导入的 `late-fs` 动作排在原厂 vendor 配置之后：此时 vold 已建立并挂载解密映射，但 `post-fs-data`、APEX 数据挂载、应用和 KSU 的数据监测尚未启动。普通 `post-fs-data.sh` 比 KSU 数据监测更晚，不能代替这个入口。接入使用 SukiSU 自带的模块接口，不修改 BOOT；不支持该接口的管理器仍可使用引导功能，但不能发起空间归还。

静态维护程序保存在 `persist/sunuefi/storage/`，因为早期尚不能读取 `/data/local` 等目录里的加密文件，系统动态链接环境也未完成初始化。`/metadata/sunuefi/` 只保存 init 配置，vold 的原有密钥和其他目录保持不变。安装时原子更新程序和配置，预览、执行与重启前核对早期程序内容和管理器生成的导入项。

确认归还后，程序从已有操作记录提取任务编号和目标容量，核对当前明文 F2FS 的 UUID，写入一个仅 root 可读写的一次性请求。下次启动由 init 同步执行：

1. 在 `late-fs` 核对请求、不同的启动编号、原文件系统 UUID、完整零偏移解密映射及目标容量。进入 `post-fs-data` 后拒绝执行。
2. 把原厂扩容工具、loader 和所需库复制到内存目录，验证工具可运行，保存 `/data` 的挂载参数。
3. 同步并正常卸载 `/data`，要求明文设备可以独占打开。不能使用 lazy／force 卸载，不停止或卸载 KSU。
4. 运行原厂正常扩容，向其标准输入传递已确认操作对应的 `y`。不使用忽略错误或强制选项；原厂 `-s` 用于不调整元数据的缩容，不能用于扩大。原厂工具的额外 rescue 日志在子进程私有挂载空间中重定向到内存目录，不写其他原厂分区文件。
5. 确认双超级块一致、UUID 未变且 F2FS 覆盖目标容量，按原参数挂回 `/data`，继续 Android 启动。模块的启动服务只读检查结果，确认后清除待完成记录。

执行前把请求改名为已尝试记录。失败不会自动重试写入或循环重启；未能正常卸载时保持 Android 挂载继续启动。扩容工具或读回失败时，只重启一次，让原厂启动路径重新检查文件系统。若重启调用失败则停住 init，避免在未挂载的 `/data` 上继续创建目录，此时可长按电源重启。关键输出保存在 `persist/sunuefi/storage/grow.log`。

## 删除并归还空间的顺序

只有独立存储区域紧邻 `userdata` 尾部，且归还范围内没有其他分区时，才能保持起点不变、向后扩大 `userdata`。分区编号不决定物理顺序，必须读取 GPT 的起止地址。

删除过程先要求已安装早期维护、Android 启动路线和独立分区未挂载，再删除 ESP/root 条目并扩大 `userdata` 的结束位置。预览和执行均检查扩容条件；不满足时不删除分区。原厂系统分区、加密配置、`userdata` 起点和现有 GUID 均保持不变。

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

实机早期扩容将 F2FS 从 `219871686656` 字节扩大到 `495286484992` 字节，用时约 10.8 秒。物理分区、解密映射和 F2FS 容量一致，双超级块一致、UUID 保留，原挂载恢复后 Android 启动完成，BOOT 哈希未变。重启后的用户加密文件仍需解锁才能读取，不能用设备加密区域可读代替用户文件内容检查。
