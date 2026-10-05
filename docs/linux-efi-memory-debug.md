# EFI memory debug：待 root 实机验收

独立worktree为`/home/slwyts/linux-piano-efi-memory-debug`，branch为`topic/piano-efi-memory-debug`，canonical commit `c4bbf928f335174f8518831797a94597a530c575`。直接parent仍是已测`094d0b053f61ca20f584e10faa624cd0bc745db0`，上游base仍是官方`7704c4c5bb127673b4f0ead839919db573559e38`。旧094d、stable/next、meta pins、fullDRAM map、shared/staging/prepare均未改。

test81六marker已证实执行到efi_init返回；其真正后续失败阶段和panic原因仍未知。此新topic只增加默认关闭的`CONFIG_PIANO_EFI_MEMORY_DEBUG`，要求entry gate、PRINTK和built-in PSTORE_RAM。相对已验094d config，仅此gate=y和LOCALVERSION=`-piano-efi-memory-debug`发生变化；Image/config及包的hardware_verified保持false，不代表日用或完整OS启动。

新记录覆盖FDT、arch/main两次early parameters、memblock/kernel reserve/DT reserved-memory、paging、unflatten、bootmem、resources、setup_arch返回、log buffer/MM core/scheduler/console，以及ramoops probe/register。每次checkpoint与panic dumper通过标准`kmsg_dump_get_buffer`读取最新可容纳8KiB printk文本，切成最多64片128字节写入已检查的2MiB console。不会直接printk、注册boot console、分配内存、读取设备寄存器、挂载磁盘或修改原分配policy。

重入guard在实际ARM64汇编中只有一次LDAXR/STXR尝试；失败直接退出，没有重试回跳。helper显式关闭ftrace、各sanitizer和stack protector，最终对象无allocator/stack_chk/printk/BS调用。panic callback的buffer、状态和函数保留到late-init，随后disable/unregister，避免访问已释放init fixmap代码。

ramoops建立WC/NC映射前，WB fixmap producer暂停。console存在后用同一cprz映射和已验证无ECC容量，`raw_spin_trylock_irqsave`成功才进行有界header/data append；锁busy直接skip，不调用可能自死锁的`persistent_ram_write`，不fallback到WB alias。失败probe先detach provider、free原映射后才resume WB producer。过渡期间故意没有sink，因此仍可能缺少该小区间内的panic文本。后续normal ramoops archive/zap行为保持原样，正常driver可能归档或替换先前ring。

完整AArch64 Image、最终增量和modpost均通过，最终build无warning。初次编译的atomic include问题已修复；之后进一步以单次exclusive-store guard替代可能重试的普通atomic。源码host ASan/UBSan测试运行真实mapped producer和snapshot/lifecycle代码，覆盖128字节片段/8KiB边界、64片上限、panic重入、quiesce/provider失败无fallback、unmap后resume与late-stop。provider trylock的kernel实现由实际ARM64对象审查；host lifecycle测试mock backend，不声称验证实际硬件锁或cache。

默认关闭对照也通过：head `.idmap.text`1692bytes、setup `.init.text`2208、mm/init `.init.text`2116、init/main `.init.text`5532、ramoops `.text`5620、efi-init `.init.text`1340，与已验094d对象逐字节相同。证据见`artifacts/kernel-topics/piano-efi-memory-debug/default-off-proof.json`；这只证明这些关键代码section，不代表全部Image等价。

| 独立产物 | 大小 / SHA256 |
| --- | --- |
| Image | 39721472bytes；`1dcb79e2f3cf4c79f0fe7ba3523091369202c5c4f67a84361c4d1cf4a02e288a` |
| config | 314135bytes；`ec7b9f916b8ec801c218bb7182fa6639d3c84749d516455db9b73cb098004172` |
| topic manifest.json | `eb01f9e7588df2f7fac3fce8d0027338932b31dae4aadf0d76d7fffb641ebbb0` |
| 新initramfs.cpio.gz | 662033bytes；`7bd873998fbed3a483d75b498768f5e97d1e31195050acde06fc38cb85d13c5e` |
| 新linux-payload.bin | 41494459bytes；`e0c90f35fa0cedcd9a83194ec5a43cf0ce420df40a7722f1afb54e7542a38378` |

Image/config/asm在`artifacts/kernel-topics/piano-efi-memory-debug/`；新RAM bundle在其`ram/`子目录。SizeOfImage已增至40566784（0x26B0000），primary_entry RVA为1D400F0、PE entry RVA为1DE5854；不能沿用test81的旧primary RVA。DTB仍为同一captured live.dtb SHA a4b55d...，新CPIO仍使用相同静态BusyBox/RAM PID1，内嵌新commit/Image/config/DTB绑定，无modules或rootfs变化。

生成和严格验证：

```sh
python3 tools/package_kernel_diagnostic.py --topic piano-efi-memory-debug
python3 tools/package_kernel_diagnostic.py --topic piano-efi-memory-debug --verify-output
```

新policy明确parent_commit=094d、base_commit=7704；验证HEAD^==parent及base ancestor，旧entry policy仍默认HEAD^==base。source/input/嵌套manifest/CPIO/V2全部严格复核，当前kernel相关50个host tests通过。root应在独立包验证成功后显式copy payload和payload-manifest到shared build路径，再核对副本hash；工具不会自动更新shared或操作设备。

下一次运行看最后一个stage与其printk tail；若有`PIANO_PRINTK_PANIC_BEGIN`，按其中真实panic文本定位。没有panic段仍可能是异常、hang/watchdog、sink拒绝或过渡gap，不能据容量数字或缺日志认定原因。本轮没有新设备操作，硬件结果待root。
