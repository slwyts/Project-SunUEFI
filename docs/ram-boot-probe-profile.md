# First standard fastboot EFI boot profile

当前代码已接入真实command/Device/Controller/Application路径，并完成host验证与完整EDK2构建；**尚未实机启动**。默认`PIANO_USB_RAM_BOOT=0`，旧USB profile不启用boot。第一版明确只允许固定返回probe，完整产品的任意EFI/Android/Linux镜像与1024MiB下载仍待后续内存/交接验证。

## 已接入的路径

`boot`先检查完整CPU下载、实际Boot parser、PE预算和caller policy，再排队OKAY。DWC观察真实EP3 IN完成、HWO清除、residual0、4bytes OKAY，队列空后冻结dispatch；Halt及9个共享DMA buffer退休成功，再校验source并在ClearFastboot之前移交。

Controller的独立RAM入口拒绝已经存在的SID60/40；USB结束后核对domain/table/mappings全空、8个clock与GDSC退出、SID60/40缺失，以及其他SMR/S2CR和display context与初始值一致。任何retained/unknown或warning都不能触发PE执行。halt-only protocol只供原恢复timer使用，不能当成完整owner退休；正常路径先卸载该protocol再释放clocks。

App将DWC移出的CPU source放到持久adapter，再由carrier一次转交Launch，避免再次调用原Download.Take。全owner门禁通过才LoadImage/StartImage，返回后验证auto-unload、source zero-release及probe记录。Root与Launch两道EBS fence捕获异常返回；fail-stop只存CPU状态、mask DAIF及CpuDeadLoop，不在EBS后调用BS/DebugLib。所有不保留资源的普通返回都关闭Root event，retained状态禁止复用。

## 固定返回应用与记录

`PianoRamBootProbe.efi`是实际AArch64 PE application：3584 bytes、SizeOfImage20480、4sections、真实DIR64 relocation。SHA256为`e42f0ae416c1843ed1dcdaa87301f9a34b093becec9c829d46ce03388038d2b9`，编译/链接工具和源码SHA在产物manifest中。当前caller policy只允许该kernel-view SHA与精确尺寸，stock fastboot生成的Android wrapper仍可使用；其他文件会在OKAY之前FAIL。

应用读取自己的LoadedImage、打印一行后返回，写入唯一GUID下的`SunUEFI-RamBootProbe`记录。attribute只有`EFI_VARIABLE_BOOTSERVICE_ACCESS`，没有NON_VOLATILE/RUNTIME；不覆盖已有值，没有block/device/DMA/reset操作。CRC涵盖完整88-byte记录，consumer还核对EL1编码4、Parent、实际loader ImageBase/ImageSize。只收到CLI OKAY不代表执行成功，必须在最终RAM log看到`SUNUEFI_RAM_BOOT_PROBE_VERIFIED`与完整execution/retirement结果。

## 构建与测试入口

```sh
python3 tools/build_ram_boot_probe.py --output NEW_OUTPUT_DIRECTORY
python3 tools/prepare_gui_profile.py --usb-ram-boot --return-seconds 120
bash tools/build_stage0.sh gui
python3 tools/package_stage0.py --profile gui --header-version 3
python3 tools/check_fastboot_ram_boot.py --test-id NEW_ID
```

最后一行默认为dry-run，不联系设备；显式`--execute`才在匹配的固件RAM启动record后等待`SunUEFI-piano`，检查product、`SunUEFI:ram-boot=enabled`、64MiB容量并运行stock `fastboot boot`。工具不重启固件、不flash，不把command accepted写成EFI execution verified。第一次执行仍应在第89次恢复和保留日志回收之后，使用新的test-id。

主机验证包含184项Python suite、实际command/DWC事件闭环，以及Controller16+App24+默认关闭2个fork案例（共42），ASan/UBSan通过。App fixture执行的是生产C函数和模拟EFI服务；AA64 CurrentEL只作明确host bridge，不冒充ARM执行。实际probe PE两次构建SHA相同，parser接受且relocation存在。完整build曾因缺EBS event GUID失败，补入真实INF Guids后重新构建通过。

## 仍需实机证明

本profile使用已测USB基础，但新boot链尚无设备Load/Start/return记录；不据host mocks声明controller实际退休、volatile record或自动Android恢复通过。小probe验收之后才能解除实验SHA策略并扩大loader/initrd/DTB路径。1GiB必须先有真实高RAM映射/阶段所有权、全段读写与streaming/timeout证据，当前仍advertise64MiB。后台菜单服务、UFS OS启动和Windows/完整Linux EFI handoff没有由本profile完成。

当前封存版本为`artifacts/diagnostics/usb-ram-boot-v2`，build_id `dcad19d4-56d7-4f0f-9c62-4980cff3d018`，image SHA `dad7962e9e403b45c8826f7604c0f89828feed009bdfd634a41441a19c37387a`。v2增加动态capability query，旧/default profile回答disabled，注册且Ready的backend才enabled；因此host checker不会把旧USB固件当boot候选。测试每次在临时目录实际构建probe，不依赖未提交的旧EFI artifact。
