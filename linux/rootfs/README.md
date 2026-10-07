# 发行版基础系统

入口为 `python3 tools/assemble_rootfs.py --distro debian --desktop gnome --plan`。`--execute` 才使用真实 debootstrap/APT 或 Arch tarball/pacman 后端，需要 root、私有挂载 namespace、chroot 及跨架构时的 qemu/binfmt。输出必须是新项目 `build/distros/` 目录。

| 基础系统 | GNOME | KDE | DDE |
| --- | --- | --- | --- |
| Debian trixie | 构建候选 | 构建候选 | 不可用：无已审配方 |
| Ubuntu noble／24.04 LTS | 构建候选 | 构建候选 | 不可用：不添加 DDE PPA |
| Arch Linux ARM aarch64 | 构建候选 | 构建候选，Maliit按包可用性 | 不可用：无已审配方 |
| Deepin ARM64 | 未开放 | 未开放 | 需显式签名 base 与目标仓库包检查 |

“构建候选”不是 Piano 启动验收。APT/pacman 执行前会检查必需包，缺项会失败，不使用 ignore-missing 或关闭签名。默认软件源仍会更新，记录实际包清单及仓库元数据，不宣称完整位级复现。

Arch 官方 latest tarball 是可变来源，执行必须显式传 `--base-sha256`；不自动补一个未经取得的 hash。官方默认 root/alarm 密码会被锁定，SSH身份与machine-id会清理，目标仓库使用 pacman-key 初始化/填入 Arch Linux ARM 签名密钥。

Deepin 官方有 ARM64 发布，但本仓库没有已核实的可直接导入 base tarball。必须提供 `--suite --base-url --base-sha256 --base-signature-url --base-keyring --base-keyring-sha256 --keyring --sources-list`。导入前核对 digest/GPG签名，导入后核对 distro身份；源文件只接受同 suite 的 Deepin 官方 HTTPS仓库，不混 Ubuntu/Debian源。不提供这些输入时只报告 `INPUTS_REQUIRED`，不下载或假设其存在。

基础系统与 [桌面](../desktops/README.md) 独立，BSP另包提供。[storage.json](storage.json) 约定根分区名 `sunuefi_root`，文件系统标签仍为 `PIANOROOT`；assembler不更改GPT、UUID或设备数据。`--bsp DIR --layer MESA --layer RUNTIME --layer MODULES --layer FIRMWARE` 要求所有 manifest 的 `target={distro,suite,arch:aarch64}` 匹配；Mesa/runtime还要声明实际 glibc版本或静态libc，包管理器保持正常依赖检查。一个 Debian deb不能作为其他发行版的通用编译层。BSP `payload.tar/PKGBUILD` 仅是配方，未生成目标包时不能声称安装完成。

没有选择 BSP 时可真实生成基础系统+桌面，状态为 `HOST_ROOTFS_DESKTOP_BUILT_BSP_PENDING`。选择完整BSP时缺编译层会拒绝。安装过程中移除发行版 generic kernel包，dispatcher不把它们设为ESP启动项；最终kernel/DTB/label bootstrap由共同发布流水线提供。

桌面默认自动登录锁定密码的 `piano` 用户；通过安装器或可信root控制台首次设自己的密码。没有公共固定密码。每个桌面的200%默认和OSK边界见桌面页，旋转尚依赖实际传感器验收。

官方依据：[Debian trixie ARM64](https://www.debian.org/releases/trixie/arm64/)、[Ubuntu noble ARM64](https://cdimage.ubuntu.com/ubuntu/releases/noble/release/)、[Ubuntu debootstrap/keyring](https://packages.ubuntu.com/noble/debootstrap)、[Arch Linux ARM Generic AArch64](https://archlinuxarm.org/platforms/armv8/generic)、[Deepin发布页](https://www.deepin.org/en/download/)。
