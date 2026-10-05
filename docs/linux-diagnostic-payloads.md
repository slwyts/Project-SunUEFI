# 固定 topic 的 Linux RAM 诊断包

`tools/package_kernel_diagnostic.py` 是独立诊断入口。现有 `make_kernel_initramfs.py`、`package_kernel_payload.py` 的 stable/next CLI 和 pin 行为保持原样；诊断工具不读取或修改 `kernel-profiles.json`，不修改prepare、staging或设备。

当前允许两个固定topic。`piano-efi-entry-debug`仍是clean commit `094d0b053f61ca20f584e10faa624cd0bc745db0`，直接parent为官方 `7704c4c5bb127673b4f0ead839919db573559e38`。新增`piano-efi-memory-debug`为commit `c4bbf928f335174f8518831797a94597a530c575`，明确parent_commit=094d，上游base_commit仍为7704；验证HEAD^==parent且base为ancestor，旧policy仍默认HEAD^==base。新topic状态与产物见[EFI memory诊断说明](linux-efi-memory-debug.md)。允许的topic/build-manifest/Image/config/同机captured live.dtb/BusyBox/provenance/PID1各自SHA固定在工具的显式topic policy中。输入或重新构建的Image发生改变时必须经过新诊断review更新该policy，工具不会自动跟随branch、重标旧manifest或移动stable/next pins。

生成及严格验证出口：

```sh
python3 tools/package_kernel_diagnostic.py --topic piano-efi-entry-debug
python3 tools/package_kernel_diagnostic.py --topic piano-efi-entry-debug --verify-output
```

默认输入build为`artifacts/kernel-topics/piano-efi-entry-debug/manifest.json`及同目录Image/config；DTB为`private/captures/2026-10-03-piano/live.dtb`，SHA `a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7`。CLI可明确选择另一路径，但其内容仍须匹配同一policy。Git使用no-lazy-fetch，只检查本地HEAD、branch、直接parent和tracked/nonignored工作树状态，不获取网络对象或修改refs。生成前及发布前后都会重核source/input快照，输出不能覆盖原始输入或写入topic工作树。

输出为`artifacts/kernel-topics/piano-efi-entry-debug/ram/`：新派生的RAM kernel manifest、Image/config/live.dtb副本、新`initramfs.cpio.gz`与`initramfs-manifest.json`，以及`linux-payload.bin`/`payload-manifest.json`。原topic build manifest保留，派生manifest明确记录它的路径/SHA。CPIO使用既有静态ARM64 BusyBox和RAM PID1源码，内嵌`etc/piano/diagnostic-topic.json`，绑定topic commit/base、Image、config、DTB和原build-manifest。

CPIO不含旧6.6或其他kernel modules、rootfs、存储mount脚本。验证器检查唯一允许的entry集合、PID1/BusyBox/config/build内容、symlink、设备节点major/minor和permissions；gzip解压有16MiB上限。恢复仍是PID1启动后的180秒自动reboot；进入PID1前的故障继续由root loader的panic/recovery安排处理。打包成功不证明marker或kernel实机可用。

V2保持loader实际布局：144字节header，kernel/initrd/DTB sizes在24/32/104，三段SHA256在40/72/112；后面顺序为Image、新gzip CPIO、同一显式完整DTB。验证拒绝header/version、零/过大size、截断、额外尾部、分段hash和完整FDT结构错误。发布后复核所有派生manifest的source/profile/mode/hardware gate、嵌套manifest SHA与实际文件一致；`hardware_verified`及`dtb_board_topology_verified`仍为false。

当前产物：

| 文件 | 大小 / SHA256 |
| --- | --- |
| Image | 39721472bytes；`7091941820b74407d5dd0575495afdea29e8f7ecde699705807cc817f0135ce9` |
| config | `a7fae7617530e464c9c624bf4d5a16c760060c45b0db8b82a44d9b26c02fc14c` |
| initramfs.cpio.gz | 661983bytes；`4875dc539f70b15ac583c4451779b9b8a16ad7b03a947b92c8251d870a64630f` |
| live.dtb | 1110810bytes；`a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7` |
| linux-payload.bin | 41494409bytes；`b4fc84d7d29cf39d8f26803ded2e25df86c417c5f62cd378b64f5f24e2b9fc74` |

工具不会自动替换共享`artifacts/linux-ram`。root在独立目录严格验证成功后，如现有build需要共享路径，应显式复制`linux-payload.bin`和`payload-manifest.json`，再核对共享副本hash；不要运行普通stable/next packager来给诊断包重标profile。

`tests/test_package_kernel_diagnostic.py`使用真实临时Git histories、实际newc/gzip和独立V2边界修改，覆盖source HEAD/branch/dirty、self-consistent config漂移、输入与生成中漂移、manifest gate、EFI header、CPIO模块/模式/绑定、payload长度/hash/尾部和bounded解压负例。以上仅为host验证，没有执行设备、kernel build、meta commit或staging操作。
