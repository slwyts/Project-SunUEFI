# 原生 BOOT 文件重打包与还原

`android/native/piano-boot-repack.c` 已实现普通文件模式的 `status`、`probe`、
`repack`、`restore`。当前支持 Piano 原厂 BOOT v4、raw AArch64 GKI、独立
init_boot 和无 boot_signature 的布局；不读写设备，不接受 `--boot-device` 或
`--execute`，不能直接交给现有 Android 模块的在线安装脚本使用。

## 已核对的真实布局

2026-10-03 RO 采集的原厂 boot_a 文件为 100,663,296 字节（96 MiB），BOOT4
header_size=1584，GKI 文件长 36,866,560 字节，AArch64 Image span=37,486,592。
AVB 原 image_size=36,888,576，vbmeta 长 2368 字节，footer 在文件最后 64 字节。
GKI 后约 61 MiB 的区域仅有 5393 个非零字节，不能因为大多数是零而丢弃其余
header、签名、footer 或填充。

BOOT4 / AVB 字段依据 AOSP 的
[boot image header](https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/main/include/bootimg/bootimg.h)、
[AVB footer](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/libavb/avb_footer.h)
与 [hash descriptor](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/libavb/avb_hash_descriptor.h)。
输入必须有一个正确的 SHA256 boot hash descriptor 和 Piano boot fingerprint；
当前文件模式验证 descriptor 的数据 hash，没有验证 OEM RSA 签名。

## 保持 SPLITv1

重打包的内核仍是当前早期 selector 使用的 `SUNUEFI-SPLITv1`。布局为：

```text
BOOT4 header（仅 kernel_size 更新）
原 GKI 一份（仅 branch/image_size 更新）
原 Image span 后的两个 4 KiB NEXTv1 请求页
selector 文件与其私有 stack 空间
本次明确提供的 BootShim + 当前产品 FD
当前 APPv1
RSTRv1 恢复目录
对齐填充、unsigned boot AVB metadata、分区末尾 footer
```

GKI 未再复制一份。原 BOOT header 的完整 4096 字节、原 GKI header 的 64 字节
以及内核之后的非零 4 KiB 块和原偏移保存到 APP 后的恢复目录；没有存储零区
副本，也没有另存整个原厂 BOOT。目录最多 2 MiB，无法完整表示的原始填充会
直接拒绝重打包，不会静默丢失数据。

`RSTRv1` 固定 header 为 320 字节，记录原文件长度、GKI 长度/span、selector
文件/内存大小/metadata 偏移、source/GKI/selector/shim/FD/APP SHA256 与目录
body SHA256。body 是原 BOOT page、原 GKI header，然后是原始 offset/length
及该非零块。还原首先从唯一的 GKI 副本恢复原 header，再按目录重建尾部及零区，
验证完整原厂 BOOT SHA256，并核对恢复后的 AVB descriptor。新镜像使用 unsigned
AVB NONE/hash 元数据，保留原 properties 与 rollback；没有 OEM 签名，也没有
改变设备 vbmeta 的功能。

现有 `tools/package_product_trampoline.py` 生成的旧 SPLIT 镜像没有这个恢复
目录。它的正常入口 ABI 可共用，但原生工具会拒绝声称能从旧载荷无损恢复整个
BOOT；不能凭旧 ROM 下载或历史 BOOT 备份补齐缺失的数据。

## 编译与文件接口

```sh
python tools/build_boot_repack.py --arch aarch64 \
  --sysroot build/distros/release-7.2.9/rootfs --output build/boot-repack-native
```

编译入口使用现有 clang/LLD 和 ARM64 libc/GCC sysroot，输出无 PT_INTERP 的
静态 ELF，不安装 host 包。SHA256 复用固定 simple-init 内的 BSD libmd
`sha2.c`；NEXTv1 CRC 复用现有 `BootRequest.c`。manifest.json 记录编译命令、
所有源码 SHA、固定上游提交、二进制 SHA 和明确为 false 的设备证明。

```sh
piano-boot-repack status
piano-boot-repack probe --input stock-boot.img
piano-boot-repack repack --input stock-boot.img --output wrapped-boot.img \
  --selector selector.bin --selector-memory-bytes 21712 \
  --selector-metadata-offset 12688 --shim BootShim.bin \
  --fd PianoUEFI-product.fd --app app-payload.bin
piano-boot-repack restore --input wrapped-boot.img --output restored-boot.img
```

示例中的两个 selector 数值仅对应本次已编译的 selector；每次应取同一
`selector.json` 的 `selector_memory_bytes` 与 `metadata_offset`，不能固定复用
旧值。`--bootshim` 是 `--shim` 的同义入口。FD 必须为 3 MiB，APP 必须通过 APPv1
大小和 SHA 校验；selector 的 metadata placeholder 必须是原始空值，拒绝嵌套
包装。输出必须是新文件，不覆盖输入。

一次真实原厂文件的完整比较入口是：

```sh
python tools/build_boot_repack.py --arch aarch64 \
  --sysroot build/distros/release-7.2.9/rootfs --output build/boot-repack-native-check \
  --roundtrip-stock path/to/current-stock-boot.img \
  --selector-dir path/to/current-selector-bundle
```

builder 会核对 selector 与当前 product FD/BootShim 的 manifest，运行原生
probe/repack/restore，逐字节比较原文件与临时恢复文件，并将结果写入
roundtrip.json。临时完整恢复文件比较后删除，不作为备用分区镜像保留。
ARM64 工具可在已有的本机 binfmt/QEMU 下执行；也可显式提供现有用户态
`--runner`。这一过程没有设备行为。

本次真实 boot_a 已完成该比较，原文件/恢复 SHA256 均为
`7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2`。
恢复目录为 18,368 字节，仅四个非零尾块，包内 GKI 只有一份。具体的产品、
selector、shim、APP、工具 SHA 和输出 SHA 位于本地 `build/boot-repack-native/`
记录中；它是文件 roundtrip 证据，不能当作 Android 旁路、Mi Recovery 或
设备刷写已经验证。

固定 AOSP avbtool 已独立验证新 footer、NONE vbmeta 与 boot SHA256；现有
`piano-boot-request status` 也已只读识别新文件的 NEXTv1 页，返回 target=0、
sequence=0。这些检查确认文件结构与现有请求 reader 的布局兼容，没有执行请求
写入或设备重启。

## Android 模块的下一步

当前模块 payload 模板只携带 FD、APP、selector，实际 SPLIT 还需要同一产品的
BootShim.bin。后续模块必须增加它及其 manifest SHA，不能让原生工具虚构 shim
或回退到其他构建。在线适配还缺活动槽、ROM/boot fingerprint 和 current BOOT
身份的重新读取、写前比对、受限写入和读回；OTA 后仍需正常启动当前 Android，
不能预写非活动槽。

文件工具没有实现 module policy/readiness、`request`、一次性请求消费、OTA
自动化或分区操作。`status` 明确给出 `device_execution_ready=false`，现有模块
的 `--require-ready` 参数也会被拒绝。原厂 Recovery 的路径没有被修改，但设备上
的 NORMAL、Recovery 和独立 UEFI 请求仍需同一包装的真实证明，模块 ZIP 门禁
不能据主机文件比较而解除。
