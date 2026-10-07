# Piano Stable / Next EFI 文件树

这套文件由电脑端准备，不连接设备、不修改 GPT、不向 UFS 写文件。模板中的 Stable/Next、Android、Recovery、Shell 初始均为 inactive；真实产物通过检查后才生成可选择的 Linux/Shell 项。`BootManifest.json` 始终明确 `HOST_ONLY_NOT_DEPLOYED` 和 `storage_location=UNASSIGNED`，不能把目录生成视作 UFS 部署或启动成功。

## 先生成无产物的配置计划

```bash
python3 tools/prepare_boot_files.py --plan-only --output artifacts/piano-boot-plan
python3 tools/prepare_boot_files.py --check-tree artifacts/piano-boot-plan
```

计划含有效的原生 SimpleInit JSON、BootManifest 和两个空目录，没有伪造 Image/DTB/initramfs。

## 从两条独立内核产物准备候选树

```bash
python3 tools/prepare_boot_files.py \
  --stable-manifest artifacts/kernels/stable/ram/manifest.json \
  --next-manifest artifacts/kernels/next/ram/manifest.json \
  --initramfs /absolute/path/to/ram-initramfs.cpio.gz \
  --allow-unverified --output artifacts/piano-boot-candidate
python3 tools/prepare_boot_files.py --check-tree artifacts/piano-boot-candidate
```

只准备某一项时只传该项 manifest，另一项保持 inactive。`--stable-initramfs`、`--next-initramfs` 可以分别指定；`--mode userspace-debug` 使用另一套准确配置。`--shell-efi` 只接受真实 AARCH64 EFI application，将其复制为 `/EFI/Piano/Shell.efi`；没有提供时 Shell 文件菜单保持 inactive，已经在 FV 中的 Shell 不会被假定成磁盘文件。

工具拒绝：未完成构建、提交/base_commit 与当前 `linux/kernel-profiles.json` 不符、canonical patch commits 与本地固定源码 Git 范围不符、source_dirty 不是明确 false、config/fragment/Image/DTB 哈希或镜像长度不符、缺显式 DTB、不完整或拼接 FDT、缺可执行 `/init`、强制/非空内置下游命令行。每个输入文件受固定 SimpleInit 的 `<128 MiB` 加载边界约束，DTB 还受 ARM64 的 2 MiB 边界约束。检查 EFI Image 的 ARM64 magic、AARCH64 PE32+ 和 EFI application subsystem。initramfs 输入可为 raw newc 或 gzip newc，输出统一为 gzip，要求内核启用 gzip 解包。

默认拒绝硬件未验证的 kernel manifest；`--allow-unverified` 明确允许制作候选树，但不会把它标为 rescue 或把 Stable 设为默认。只有 manifest 明确 `hardware_verified=true` 的 Stable 产物才成为 rescue 首选；Next 始终是独立实验项，不会自动成为 rescue default。构建工具目前输出的两个 baseline 都未实机验证，正常结果应是 `UNVERIFIED_HOST_STAGED`。该标记不证明 board DT 拓扑、UFS/SMMU EBS 交接或其他硬件功能已经正确。

## 文件布局

```text
simpleinit.static.uefi.json          # 固定 PCD 的自动加载入口
EFI/Piano/BootManifest.json          # 唯一显式 locate marker，记录所有实际文件 SHA256
EFI/Piano/simpleinit.static.uefi.json # 同内容副本，供手动导入/检查
EFI/Piano/Stable/Image.efi
EFI/Piano/Stable/piano.dtb
EFI/Piano/Stable/initramfs.cpio.gz
EFI/Piano/Stable/config
EFI/Piano/Stable/BuildManifest.json
EFI/Piano/Next/...                   # 完全独立的同类文件
EFI/Piano/Shell.efi                  # 仅 --shell-efi 时存在
```

没有给 Stable/Next 指派固定 `fs0:`。`locates.piano.by_file` 只匹配 `\EFI\Piano\BootManifest.json`，Linux 路径采用 `locate://piano/EFI/Piano/Stable/...` 或 `Next/...`。不存在查找 userdata root、MTP DTB、Android boot partition 或任意目录的后备路径；文件缺失即无法载入。以后实际部署应选择专门拥有的 FAT 卷，只保留一个该 marker，避免多个卷含同名树导致 SimpleInit 的“首个匹配卷”算法歧义。

## 已核对的固定源码契约

- `src/confd/json_conf.c:19`：原生 JSON 对象递归对应配置 key；当前构建启用 JSONC。
- `src/confd/uefi.c:84` 与 `SimpleInit.dec:46`：每卷根读取 `\simpleinit.static.uefi.json`，即使该卷只读也可作为静态配置 include。生成文件树不会改变这一代码。
- `src/filesystem/layer/uefi.c:150`、`src/linux-boot/loader.c:198`：实际解析的是 `locate://TAG/path`。旧文档里的 `@TAG:/path` 已不对应当前通用 fs_open 路径。
- `src/linux-boot/conf.c:153`：读取 `kernel`、`dtb`、`initrd`，实际布尔键为 `use_uefi`，不是旧文档 `use_efi`。禁用 inherited kernel-FDT cmdline，显式用 `rdinit=/init`，不添加 `root=`。
- `src/linux-boot/fdt.c:149`：处理的最终 DTB 安装到 EFI FDT configuration table；`ramdisk.c:75` 安装 Linux initrd device path + LoadFile2。Linux EFI stub 通过 LoadedImage 的 UTF16 LoadOptions 获得命令行，不使用这里额外拼出的 `dtb=`/`initrd=` 文件命令行。
- `src/boot/efi.c:92`：Shell 的实际配置键为 `options_widechar`；文件路径使用 `efi_file`。目前 RAM 加载的 SimpleInit 没有可靠 FV/SFS DeviceHandle，不能假定 `efi_fv_guid` 的“当前 image 所属 FV”查找可用。
- `src/gui/interface/core/bootmenu.c:249`：`enabled=false` 的项会被隐藏；Android/Recovery 的 inactive 描述保留在配置和 BootManifest 中，但不会伪装为可点击可启动项。BOOT_NONE 没有启动处理器。

模板设置 `boot.timeout=-1` 以保留手动选择。当前 bring-up 的 `src/main/uefimain.c:43` 在 confd_init 后强制改为 150 秒，这会覆盖加载配置的 timeout；该既有源码此次没有修改。若要完全使用文件菜单的时序，应在后续 SimpleInit 集成时移除或限定该覆盖，再验证实际 UI。

BootManifest 保留原始 BuildManifest hash、准确 source/base/canonical commits、config/fragment hash、compiler、kernel release、DTB source/validation/model/compatible 和 initramfs 验证信息。`--check-tree` 会重新核对所有实际文件 hash/size、配置两份副本和当前 source pin；现有输出目录不会被覆盖。

主机验证：`python3 tests/unit/test_prepare_boot_files.py`。测试在临时目录使用明确 TEST FIXTURE ONLY 的内核/FDT，完成后删除，绝不把它们作为真实 artifacts 发布。

[Linux EFI stub 官方文档](https://docs.kernel.org/admin-guide/efi-stub.html) 解释 arm64 Image 的 PE/COFF 与 DTB/LoadFile2 initrd 契约；[ARM64 官方启动文档](https://docs.kernel.org/arch/arm64/booting.html) 给出 DTB 大小、内存和 DMA 静止要求。具体配置键以当前固定 SimpleInit C 源码为准。
