# Piano Linux 构建与引导门槛

当前状态（2026-10-05）：stable 的独立 7.2.6 内核已在电脑编译，38 MiB Image 带 ARM64 EFI stub。test71 已实机完成 raw ARM64 RAM smoke：PID 1/BusyBox 运行，CPU_ONLINE=0-7，MemTotal=15520788 kB，DIAGNOSTICS_READY，180 秒后自动重启 Android；实际 /proc/config.gz 解压 SHA256=1cbb6541bdd94231db305fbc6ff79658ed45e8af888d32351d6fbbbbfb06c211，与构建 config 完全一致，恢复后26启动分区 SHA 一致。console 不可用时通过 kmsg 记录，不再退出 PID 1。此前 test68 的 console 退出问题和 test69 的嵌入 DTB 地址对齐问题均已修复。

这个结果只验证 raw 交接、CPU/RAM、静态 initramfs 和恢复闭环；日志 efi: UEFI not found / EFI_SYSFS absent，因此不是 Linux EFI-stub/运行时服务验收。完整发行版、原生显示、USB 交互、触摸、音频、休眠和日常使用均未因此得到验证。官方 next 7.3-rc5 的 RAM Image/initramfs也已在电脑构建，仍待对应实机验收。

## 当前源码组织

| 位置 | 实际状态 |
| --- | --- |
| `/home/slwyts/linux-piano` | 独立 sibling Git 仓库，当前 `piano-stable` 在 `7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8` |
| `kernels/linux-piano` | 主仓库登记的 Gitlink/submodule，当前 detached checkout 为同一提交；其 `.git` 指向 sibling 的 `.git/worktrees/linux-piano`，共享 sibling Git 数据 |
| `build/kernel-worktrees/stable` | 当前 detached 构建 worktree，同一固定提交；构建工具也支持带 profile/提交前缀的新目录 |
| `kernel-profiles.json` | 构建的完整 commit / base / base-config 锁定信息，branch 名称仅帮助识别来源 |

`.gitmodules` 登记 `path = kernels/linux-piano`、`url = ../linux-piano`、`branch = piano-stable`。这是当前本地 linked-worktree 安排，不是已经发布到远端的完整仓库拓扑。复制或迁移工作区时，应重新建立 sibling / worktree 关联；绝对 `.git` 指针不能作为可搬迁的依赖描述。

| 分支/来源 | 固定提交 | 当前用途 |
| --- | --- | --- |
| `piano-stable` / `blu-sharky:piano-7.2.6` | `7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8` | 设备移植基线，60 个补丁提交接在 vanilla 7.2.6 后 |
| `piano-next` / `torvalds:master` | `7704c4c5bb127673b4f0ead839919db573559e38` | 主线移植目标，当前还没有完整 piano 板级支持或已验收的 Image |
| `stable:linux-7.2.y` | `5fce161649b4d779d1b76d9fcd52dc77779774b8` | 已观察到的 kernel.org 7.2.9 更新来源；尚未成为本项目 stable pin |

内核源码由独立仓库维护。更新 pin 时，分支、Gitlink 和 `kernel-profiles.json` 应明确指向选定版本，并保留可恢复的 stable 产物和来源；不要依靠移动 remote ref 隐式改变既有构建。

## 电脑构建与产物

```bash
# 配置检查，生成状态为 CONFIGURED_NOT_BUILT。
python3 tools/build_kernel.py --profile stable --mode ram --configure-only

# 固定源码的完整 Image 构建。
python3 tools/build_kernel.py --profile stable --mode ram

# 独立静态 ARM64 BusyBox RAM initramfs，0 模块。
python3 tools/make_kernel_initramfs.py --profile stable --mode ram

# 必须显式选择完整板级 FDT；此处路径由本机核对结果提供。
python3 tools/package_kernel_payload.py --profile stable --mode ram --dtb /绝对路径/piano.dtb
```

构建输出位于 `artifacts/kernels/{profile}/{mode}/`；`manifest.json` 保存 source/base、实际 config、输入 fragment hashes、编译器、Image hash/EFI 检查和 DTB 状态。`initramfs-manifest.json` 绑定该 kernel 的 commit/Image/config，并记录 BusyBox 来源、静态 ELF 验证、PID 1 源码和 CPIO hash。封装到 `artifacts/linux-ram/` 时会再次检查 profile/mode/pin、Image/config、initramfs 的 nested hash 和 Image 来源。

`ram` 合并 `piano-ram.config`：BLOCK/storage 关闭，启用 EFI、initramfs、ramoops 和 simpledrm，命令行交给 loader。`userspace-debug` 再合并调试 fragment，提供 USB 网络/串口和模块化存储/rootfs 支持；它需要自己的 Image/config/initramfs 验收，不能继承 RAM smoke 的硬件结论。

构建工具使用 detached worktree，不切换用户的源码 branch；工作树脏、commit 不符、配置阶段或 Image 阶段 fragment 漂移均拒绝发布完成 manifest。EFI 检查覆盖 ARM64 magic、PE/COFF、PE32+ optional/data-directory 和 section/raw-data 边界。DTB 自动检查目前覆盖 FDT header/总长度，板级 wiring、reserved-memory、DMA 流和交接仍需单独核对。

## Stable / Next 引导门槛

| 检查 | 两个 profile 的要求 |
| --- | --- |
| 来源 | Image manifest 的 profile/mode/完整 source commit 匹配锁定文件，源码状态干净且完成构建 |
| 配置与 Image | 实际 config/Image hash 匹配 manifest；RAM 禁用持久存储；Image 的 ARM64 EFI 结构有效 |
| initramfs | 使用对应 profile 的独立新 `/init`；Image/commit 来源和 nested CPIO hash 匹配；不带原机 6.6 vendor modules |
| DTB | 显式完整 FDT，无隐式 MTP/手机 DTB 替代；继续核对实际 piano 节点、保留区与 DMA/显示交接 |
| payload | v2 头长 144 bytes；kernel/initrd/DTB 大小、顺序、边界和各自 SHA256 一致 |
| 运行状态 | 主机生成的 status / `hardware_verified=false` 保持原义；实际结果由对应镜像的本轮日志和设备观察建立 |

当前 stable 具有主机 Image 和新 RAM initramfs，可用于后续实验；test68 只能证明内核执行到 init 尝试。当前 next 缺对应完成构建与 piano 板级移植验收，其实验入口应在这些产物和校验齐备后启用。不能借用 stable initramfs 的 provenance、旧 GKI 成功日志或通用 ARM64 镜像把 next 标记为可用。

RAM PID 1 先写 `/dev/kmsg` 并启动 180 秒恢复计时器，再探测可打开的 console；没有 console 时保留 PID 1 并继续诊断。它只挂载 devtmpfs/proc/sysfs/tmpfs，读取 USB sysfs 状态，不配置 gadget 或加载模块。计时器只能处理 PID 1 已运行后的情况；kernel panic 恢复由本次 loader cmdline 决定。

补丁缩减使用 `tools/audit_kernel_upstream.py`：ancestor、patch-id + changed-byte fingerprint 分开记录，canonical squash components 单独报告；缺对象、浅历史或搜索不完整保留 `unknown`，不自动删除补丁。构建/封装负例测试使用临时夹具和模拟编译命令，覆盖源码 pin、漂移、EFI 边界和 payload v2 合约；这些测试不是硬件启动测试。
