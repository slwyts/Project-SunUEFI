# 第三方来源与本地材料

按需查阅的来源索引；完整 commit/hash 以链接的锁文件和脚本为准。本页保留已知许可线索，没有重新读取上游许可正文或私有材料。

## 源码依赖

| 组件 | 来源与固定身份 | 本仓库使用方式 / 工具 | 许可核对位置与当前范围 |
| --- | --- | --- | --- |
| Mu-Silicium | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)，`66e7bd1e7bcb757d4b28629bd6409d7209d3b242` | [prepare_piano.py](tools/prepare_piano.py) 派生 OnePlus 模板；[build_stage0.sh](tools/build_stage0.sh) 编译 | 上游许可与文件声明；[DeviceBuild.py](uefi/platforms/pianoPkg/DeviceBuild.py) 保留 Microsoft 版权/BSD-2-Clause-Patent。 |
| Mu_Basecore、Mu Plus、Mu OEM Sample、Silicium-ACPI、Device-Binaries | URL/commit 见 [sources.lock.json](sources.lock.json) | Mu 构建树及 [product hooks](tools/prepare_product_pump.py) | 各来源的许可/版权；Device-Binaries 还需按二进制材料核对。 |
| dtc | [dgibson/dtc](https://github.com/dgibson/dtc)，`7a1e017926004ecff5fce62d62d42ce9f3e00082` | `upstream/dtc`、DTB 工具 | 上游固定提交的许可和文件声明。 |
| simple-init | [BigfootACA/simple-init](https://github.com/BigfootACA/simple-init)，`3d66a6e78d519dd050fbebde4db6c5ac933f9aa4` | [prepare_simpleinit.py](tools/prepare_simpleinit.py)、[build_simpleinit.sh](tools/build_simpleinit.sh) | 上游声明；[本地 runtime](uefi/components/product-pump/simple-init/src/gui/piano_product_runtime.c) / 头文件为 LGPL-3.0-or-later。 |
| Linux / linux-piano | [blu-sharky/linux-piano](https://github.com/blu-sharky/linux-piano)、[torvalds/linux](https://github.com/torvalds/linux)；救援 pin 见 [kernel-profiles.json](linux/kernel-profiles.json) | [build_kernel.py](tools/build_kernel.py)；完整基线见 [full builder](tools/build_piano_full_kernel.py) / [next builder](tools/build_piano_next_full.py) | 选定内核的 `COPYING`、`LICENSES/` 和文件声明；不改变主机脚本的许可。 |
| MiCode 板级资料 / 公开内核 | [MiCode/Xiaomi_Kernel_OpenSource](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)，`45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71`；DTS pin 见 [来源集](docs/piano-full-linux-candidate.md) | [PianoKeys.c](uefi/core/PianoKeys.c) 等注明参考来源 | 原声明保留；本地 Keys 为 GPL-2.0-only，TouchProbe 为 GPL-2.0-or-later。 |
| Debian/Piano 用户态、Mesa、固件 | 组件/精确 commit 见 [Linux 来源集](docs/piano-full-linux-candidate.md) | [stage_piano_full_userspace.py](tools/stage_piano_full_userspace.py) 等暂存到派生根系统 | 包、服务源码、Mesa 与固件按各自通知处理。 |

`sources.lock.json` 只覆盖部分依赖，其他 pin 仍在脚本中。内核 linked-worktree 获取方式见 [内核复现说明](docs/linux-reproducible-builds.md)。

## 字体、图标与显示资源

| 资源 | 身份与使用 | 通知 / 尚需核对 |
| --- | --- | --- |
| Noto CJK | [prepare_simpleinit.py](tools/prepare_simpleinit.py) 提取主机字体的 SC 字面；主机包版本未锁定 | 实际 hash 写入 `source-manifest.json`；字体许可/通知来自取得包。 |
| Font Awesome | 同一脚本下载 `FortAwesome/Font-Awesome` `5.15.4` 字体 | 记录实际 hash，下载前未锁内容 hash；核对该版本字体通知。 |
| AOSP bitmap font | [Font.h](uefi/platforms/pianoProbePkg/Library/RamLogSerialPortLib/Font.h) | 文件头已保存 AOSP 版权及源码/二进制分发条款；发布时保留适用通知。 |
| TianoCore 标志与品牌文字 | [ProductSplashAssets.h](uefi/components/product-support/Library/ProductBootManagerLib/ProductSplashAssets.h) 描述为六辐标志的向量解释 | 保留来源说明，品牌使用范围另核对。 |

`build_simpleinit.sh` 会把字体所在 rootfs 生成链接对象，因此它们可能进入二进制，通知核对不能只限于 Git 中的源码文件。

## 原机固件与二进制发布边界

原生 PE/DEPEX、boot/DTB、厂商模块及固件保存在被忽略的本地目录，取得与输入限制见 [本地输入](docs/devel/local-inputs.md)。

[inventory_native_drivers.py](tools/inventory_native_drivers.py) 清点提取模块；[prepare_product.py](tools/prepare_product.py) 校验并嵌入 FV；[piano_inherited_clock.py](tools/piano_inherited_clock.py) 产生固定 ROM ClockDxe 兼容派生。

这些材料的二进制再分发条件**未审查**；hash 只证明身份，不授予分发许可。未来 Release 按实际内容核对来源与通知，本页不作合法性或兼容性结论。
