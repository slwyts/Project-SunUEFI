# 原生 BOOT 重打包、还原与持久请求

`android/native/piano-boot-repack.c` 已实现普通文件的无损重打包／还原，并接通
Android 模块要求的在线 `status`、`probe`、`repack`、`restore`、`request`。
在线写入源码已完成，尚未在设备执行；实际安装、读回、还原、包装启动和
标准 Recovery 仍需分别验证，模块 ZIP 的设备证明门禁保持关闭。

当前仅支持 Piano 原厂 BOOT v4、raw AArch64 GKI、独立 init_boot、无
boot_signature 和 96 MiB active BOOT。工具不改变 Recovery、init_boot、
vendor_boot、vbmeta、GPT 或非活动槽，也不自动重启／处理 OTA。

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
工具验证 descriptor 的数据 hash，没有验证 OEM RSA 签名。

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
`sha2.c`；NEXTv1 CRC 和选择规则复用现有 `BootRequest.c`。policy/state JSON
使用固定 jsmn v1.1.0 的 MIT tokenizer，来源与 commit 位于
`android/native/jsmn-source.json`。manifest.json 记录编译命令、
所有源码 SHA、固定上游提交、二进制 SHA 和明确为 false 的设备证明。

```sh
piano-boot-repack status
piano-boot-repack probe --input stock-boot.img
piano-boot-repack repack --input stock-boot.img --output wrapped-boot.img \
  --selector selector.bin --selector-memory-bytes 21712 \
  --selector-metadata-offset 12688 --shim BootShim.bin \
  --fd PianoUEFI-product.fd --app app-payload.bin
piano-boot-repack restore --input wrapped-boot.img --output restored-boot.img
piano-boot-repack request --input wrapped-boot.img --output selected-boot.img --target linux
piano-boot-repack request --input selected-boot.img --target linux --preview
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
probe/repack/restore，逐字节比较原文件与临时恢复文件，再用真实 wrapper
保存 Linux 请求、只读查看、改选 Android，并验证请求变化后仍精确还原。结果写入
roundtrip.json。临时完整恢复文件比较后删除，不作为备用分区镜像保留。
ARM64 工具可在已有的本机 binfmt/QEMU 下执行；也可显式提供现有用户态
`--runner`。这一过程没有设备行为。

本次真实 boot_a 已完成该比较，原文件/恢复 SHA256 均为
`7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2`。
恢复目录为 18,368 字节，仅四个非零尾块，包内 GKI 只有一份。具体的产品、
selector、shim、APP、工具 SHA 和输出 SHA 位于本地 `build/boot-repack-online-native-recovery-20261008/`
记录中；最终二进制的源码/工具 hash、同 stock 文件重打包、持久改选和
精确还原另记录在 `final-file-validation.json`。它们是文件证据，不能当作 Android 旁路、Mi Recovery 或
设备刷写已经验证。

固定 AOSP avbtool 的既有检查已验证新 footer、NONE vbmeta 与 boot SHA256。
当前原生真实文件检查返回 Linux target=2/sequence=1，preview 保持该记录，
改选 Android 后 target=0/sequence=2；共享 reader 与 CRC 一致。请求变化会使
新包装 AVB 数据 hash 失配，RSTR 还原仍得到上述相同原始 SHA。所有临时完整
恢复文件和请求对照文件在比较后删除；这些是文件证据，不能代表在线写入已验证。

## 在线接口与模块接线

在线调用需要 `--policy`。工具校验接口／ABI、运行中自身二进制的 hash，以及
实际 repack 所用的四个 payload hash。模块 builder 现在携带同一产品的
`shim.bin`、`fd.bin`、`app.bin`、`selector.bin`，并从同一次 selector manifest
传入 `selector.memory_bytes` 与 `selector.metadata_offset`，不猜测 shim 或偏移。
有真实文件检查输入时，native builder 还输出 `selector-module.json` 与
`policy-prototype.json`，可供设备只读 probe/status；该 prototype policy 的
`zip_ready` 与设备证明均为 false。

以下是原生与模块脚本采用的接口。只读阶段可先 probe/status；当前已经是
RSTR wrapper 时 probe 不加 `--reject-wrapped`。该选项只用于后面的新安装检查。
`slot` 必须来自当前
Android `getprop ro.boot.slot_suffix`，node 只能是与它对应的
`/dev/block/by-name/boot_a` 或 `boot_b`：

```sh
piano-boot-repack status --interface-version 1 --policy policy.json
piano-boot-repack probe --boot-device "$bootdev" --active-slot "$slot" --policy policy.json
piano-boot-repack probe --boot-device "$bootdev" --active-slot "$slot" \
  --policy policy.json --output current.json --reject-wrapped
piano-boot-repack repack --boot-device "$bootdev" --active-slot "$slot" \
  --source-metadata current.json --payload-dir payload --policy policy.json \
  --state-output install-state.json --execute
piano-boot-repack status --boot-device "$bootdev" --active-slot "$slot" \
  --policy policy.json --installed-state install-state.json --json --read-only
piano-boot-repack request --target linux --boot-device "$bootdev" --active-slot "$slot" \
  --policy policy.json --installed-state install-state.json --preview
piano-boot-repack request --target android --boot-device "$bootdev" --active-slot "$slot" \
  --policy policy.json --installed-state install-state.json --execute
piano-boot-repack restore --boot-device "$bootdev" --active-slot "$slot" \
  --policy policy.json --installed-state install-state.json --execute
```

原生端调用固定 `/system/bin/getprop`，重新确认 piano、active slot、正常启动
完成和解锁状态。probe 只读 BOOT，保存 current source SHA、节点/rdev、容量
以及三项独立身份：当前 ROM 指纹、`ro.bootimage.build.fingerprint`、实际 BOOT
footer 指纹；不要求后两者相等。当前 wrapper 的 probe 还报告从 RSTR
验证的 `original_source_sha256`、`app_sha256` 和规范化 `wrapper_sha256`，用于
对照既有载荷；没有安装记录时 `owned=false`，仍不能据 probe 直接写入。
实际设备曾出现 bootimage 属性 303、BOOT
footer 309，属于要分别记录与写前比对的情况。

repack 会先在内存中完成完整包装及逐字节反向重建，保存小型
`PREPARED_NOT_WRITTEN` 安装记录并 fsync；写前重新读取身份、source hash 和节点，
只写当前 active BOOT。成功必须 fsync 并读取整个 96 MiB 的 SHA，随后才将
状态更新为 `WRITTEN_READBACK_VERIFIED`。如需临时解除该 BOOT 的内核 readonly
flag，只针对已验证的当前节点，并恢复原 flag；工具没有其他分区写入口。
中途报错必须先检查现分区，不能把 PREPARED 记录当成已安装或直接重启。

restore/request 必须核对同 ROM、同槽、同 bootimage 属性、同 footer 和安装
记录中的 source/app/wrapper hash。wrapper hash 只规范化两个自有 NEXT 页，
其他 header、GKI、payload、catalog、padding 与 AVB bytes 均需相同；NEXT 页的
CRC、generation 和零 padding 另行校验。还原先重建并验证完整原 BOOT SHA，
再执行相同的写前检查、fsync 和完整读回。OTA、改槽、外国包装、旧 SPLIT 无
RSTR 目录或缺安装记录都会拒绝；不能拿历史 ROM 镜像替代缺失的原始数据。

request 使用共享 CRC 与双页序号，写入较旧／无效的一个 4 KiB 请求页，完整
读回 BOOT 后才报告保存成功。目标可以选 Android(0)、UEFI(1)、Linux(2)、
setup(3)，持续生效直到重新选择；status/preview 不消费请求，不写 misc/PMIC，
不负责重启。模块 Action 的 `request TARGET --confirm` 保留用户明确确认。
所谓 one-shot/consumed_once 不再作为产品门禁；对应检查是实际持久选择、
改选与 CRC 往返。诊断 cmdline 的 `sunuefi.boot=uefi` override 与 NEXT 持久
选择是两个接口，不能据名字把前者捆进普通模块。

`status --require-ready` 仍要求在线 source/slot/restore/readback 等实际检查、
同包装的 Android 旁路、持久 request handling 和标准 Recovery 的证据。
实现在线代码、host roundtrip 或把字段设为 true 都不能替代这些记录。
WebUI manager bridge、设备在线写入与还原、最新 selector 包装启动和持久 Linux
偏好下的 Recovery 仍待验证；当前 prototype 文件检查使用最新 Recovery 优先 selector
`2d053c9e...`（`build/bootselect-recovery-20261008`），其源码记录保存在
roundtrip.json。已有 selector 的设备证明与在线 repacker 写入／还原的证明仍需分别记录。
