# 本地输入与复现边界

按需查阅的输入表。clone 不包含 [忽略目录](../../.gitignore) 中的采集、第三方 checkout、环境与产物；下面记录脚本要求，不表示本次已重建或验收。

## 按目标选择输入

| 目标 | 必需本地输入 | 期望身份 / 校验来源 | 取得或准备入口及限制 |
| --- | --- | --- | --- |
| 原机采集 / stage0 | `piano/sun` 采集、manifest、提取后的 UEFI、Mu/dtc | [capture_device.py](../../tools/capture_device.py) 记录大小/hash；[prepare_piano.py](../../tools/prepare_piano.py) 比较 xbl_config/uefi/live DTB/dtbo hash | capture：`--output`、`--serial`、`--metadata-only`；需 root、新目录。metadata-only 没有所需固件镜像。 |
| 产品固件 | Mu/Basecore/simple-init、工具链/venv、原生 PE/DEPEX 清单、板级 DTB | [pump pins](../../tools/prepare_product_pump.py)、[prepare_product.py](../../tools/prepare_product.py) 的 PE hash | [build_product.sh](../../tools/build_product.sh) 使用已准备输入；product prepare 没有输入路径参数。 |
| 救援 Stable/Next RAM 内核 | 含选定 commit 的内核仓库、config、编译器；initramfs 另需静态 ARM64 BusyBox/provenance | [kernel-profiles.json](../../linux/kernel-profiles.json)；[make_kernel_initramfs.py](../../tools/make_kernel_initramfs.py) 校验 Image/config/BusyBox | [build_kernel.py](../../tools/build_kernel.py)：`--repository`、`--profile`、`--mode`、`--configure-only`；从已有对象建 worktree，不自动 fetch。 |
| 完整 Linux 内核 | 已有干净完整集成/Next worktree、public config、工具链 | [full builder](../../tools/build_piano_full_kernel.py) 的 `COMMIT`/`SOURCE_PINS`；[next builder](../../tools/build_piano_next_full.py) 的来源验证 | full：`--worktree`、`--commit`、`--build-dir`、`--artifacts`、`--root`；与救援 pin 分开。 |
| 完整/组合 DTB | ROM base/live DTB、overlay、内核 header、dtc/fdtoverlay、manifest | [full DTB](../../tools/build_piano_full_dtb.py) 的 `BASE_SHA`/`STOCK_SHA`/`LIVE_SHA`；[compose](../../tools/compose_piano_dtb.py) 的 recipe | compose：`--base`、`--review`、`--kernel-tree`、`--dtc`、`--fdtoverlay`、`--variant`；显式传入匹配树，避免个人目录默认值。 |
| 磁盘 root/bootstrap/ESP 暂存 | ARM64 派生 rootfs、分区计划、Image/config/modules、BusyBox、DTB/manifest | [bootstrap](../../tools/build_piano_disk_bootstrap.py) 的配套校验；[ESP stage](../../tools/stage_piano_disk_esp.py) 的固定 `PINS`/`BOOT_SHA`/分区身份 | [root stage](../../tools/stage_sunuefi_disk_root.py)：`--rootfs`/`--plan`；bootstrap：`--rootfs`/`--kernel`/`--busybox`/`--root-partuuid`/`--output`。ESP stage 绑定固定产物，不创建分区。 |

## 私有采集与原生模块

- 外部 `uefi-firmware-parser` 需自行准备；[analyze_capture.py](../../tools/analyze_capture.py) 接收采集目录和 `--output`。提取结果还需检查结构、身份与缺项，不会自动变成产品输入。
- [prepare_piano.py](../../tools/prepare_piano.py) 支持 `--capture`、`--extracted`、`--workspace`、`--source`、`--fdtput`，会生成平台文件。
- [inventory_native_drivers.py](../../tools/inventory_native_drivers.py) 无路径参数，读取固定日期采集和 `private/uefi-extracted`，输出私有驱动清单；状态仅为 `INVENTORIED_NOT_ACTIVATED`。
- [ClockDxe 派生](../../tools/piano_inherited_clock.py) 固定原始/派生 hash；产品输入不包含历史 GPT；UFS 实验采集的校验仅供对应测试重放。新的原生固件输入仍需核对版本与身份。

## 如何理解 expected hash

采集 manifest 记录该次字节；源码 commit/固定 hash 指定脚本接受的来源；生成 manifest 关联实际产物。完整 DTB 的三份 ROM hash、ESP stage 的四份 manifest 和 boot hash 都保留在代码中，本页不再复制一套锁。重建字节不同，需要核对新来源与配套关系；hash 检查不等于实机验收。

## 主机环境、字体与测试

固件环境使用 `upstream/Mu-Silicium`、`build/host-tools/usr`、`.venv`，以及 [build_stage0.sh](../../tools/build_stage0.sh) 的 BaseTools/补丁。字体来源与实际 hash 见 [第三方清单](../../THIRD_PARTY.md)。

[ClockDxe 回放](../../tests/unit/test_piano_inherited_clock.py)、[early-memory](../../tests/unit/test_early_memory.py) 直接依赖本地输入；[存储提案测试](../../tests/unit/test_check_product_storage_proposal.py) 缺输入会 skip。新 clone 按实际输入报告通过/跳过，不继承维护者本机的旧测试总数。
