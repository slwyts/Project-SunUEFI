# RAM launch coordinator core (unwired)

`PianoFastbootLaunch.c/.h` 已提供独立execution coordinator，host tests调用实际EFI API签名的mock。没有修改或注册Device/Controller/profile/boot command，没有执行设备LoadImage/StartImage；现MAX64MiB仍不变。它只为后续root接小EFI app的真实闭环准备，不能称设备RAM boot或1GiB下载已经完成。

## 明确blob ownership与预算

`PIANO_LAUNCH_BLOB` 由Take、exact Read、BorrowView、Unborrow、Restore、ZeroRelease组成，含64-bit Bytes。Take必须把唯一blob ownership从download state转出；成功必须返回非NULLowner。callback若错误但已partial取得owner/loan，应返回可清理的token；success NULL视为unknown并保留，绝不假restore/free。

BorrowView是稳定**CPU-only**引用，不是DMA映射，不分配wholebuffer副本。Read接口复用纯boot parser，支持未来高arena/1GiB source的metadata读取；LoadImage仍需要已验证可访问的连续CPU view，scatter source不能假装支持。两个独立明确budget：MaxSourceBytes约束完整blob/selected file view，MaxImageBytes约束PE SizeOfImage及真实LoadedImage.ImageSize。它们不更新fastboot广告，也不验证arena/MMU/firmware内部额外预算。

raw AA64 PE application直接使用全source view；Android容器只在kernel是有效AA64 PE且没有ramdisk/second/recoveryDTBO/DTB时提取kernel view。未知/非PEAndroidkernel及未实现的initrd/DTB组合明确EFI_UNSUPPORTED。stock fastboot `.efi` wrapper的兼容只是这个有意提取路径；没有把容器load addresses当写权限。known v4 CLI header quirk必须Environment显式允许。签名验证仍由实际LoadImage政策决定，parser只识别结构/边界。

顺序为：Take → parse/校验/borrow → **typed ShutdownAll exact EFI_SUCCESS** → EBS fence → LoadImage → LoadedImage/小LoadOptions copy → StartImage。任何shutdown error/warning都不能调用LoadImage。caller保证Blob、Environment context、State拥有driver lifetime；shutdown不能释放已经转移的source。`PianoFastbootDownloadBlob.c/.h` 与本接口匹配：它对现有64MiB pool实现两次quiet acknowledgement、pin pointer/size、清除Source借用Upload、stale loan拒绝和空Source才Restore。未接线。

## EFI返回生命周期

固定Mu `MdeModulePkg/Core/Dxe/Image/Image.c:1799–1824` 在EFI application返回后自动卸载它，原LoadedImage指针和handle可能已失效。Coordinator因此在Start返回后**fresh HandleProtocol**，不解引用旧protocol：明确absence且Start已调用时按已测Mu contract记ImageUnloaded；fresh成功时核对原protocol address、ImageBase、ImageSize，拒绝recycled handle指向不同image。只有identity相同才恢复原LoadOptions并显式Unload。

LoadOptions最多4096bytes，由小独立pool保存，在app运行期间保持；ExitData按EFI契约回收。source/loan保持到image已卸载之后，不会先free source再调用Start。LoadImage返回error但nonNULLhandle时仍尝试安全retirement。未证明unload、lookup、event close、free、unborrow或ownership返回时，相关resource/state保留并禁止重入，不把部分清理称为成功。

failure策略明确二选一：RestoreOnFailure返回**未修改**的blob ownership给caller（不意味着已关闭USB自动重启），或ZeroRelease必须zero后consumed/free。successful returned app使用ZeroRelease；Result分别标BlobRestored、BlobZeroReleased、CleanupStatus、ResourcesRetained，不会同时宣称restore与zero。Cleanup failure成为最终返回status，app exit status单独保留。State必须zero-initialized且长期存在；retained或Busy状态Init/Run拒绝覆盖。

## EBS返回的fail-stop fence

CreateEventEx安装EXIT_BOOT_SERVICES notify，只做State CPU标记。所有BS调用前后都检查runtime-safe BootServicesAlive；Start返回若观察到EBS signal或BS失效，进入Runtime-only FailStop并以CpuDeadLoop防护callback意外返回，绝不继续Unload/FreePool/CloseEvent、restore/free blob或返回普通BS caller。

这是保守handoff fence，**EBS event不等于已经证明ExitBootServices成功**；若image在signal后回到caller，coordinator宁可保留并fail-stop，不猜测是否可恢复。标准OS正常不返回的handoff仍需实机entry/EBS-return/kernel证据。这个core不是完整OSExit所有owner registry，也不提供后台服务。

## actual-source host验证

```sh
bash tools/test_fastboot_launch.sh
```

`test_fastboot_launch.c` 编译实际parser/coordinator，用真实EFI_BOOT_SERVICES函数类型实现mock，覆盖：raw/wrapped PE、shutdown-before-Load/Start、normal auto-unload与显式unload、fresh身份/recycled handle、LoadOptions及ExitData、rawAndroid/ramdisk unsupported、budget、load/security/allocator/start错误、restore-vs-zero、unload/close/free/unborrow/ownership失败保留、successNULL违约、reentry和1GiB logical source无wholecopy。旧LoadedImage页在模拟auto-unload时PROT_NONE，BootServices表在模拟EBS返回后PROT_NONE，确保生命周期错误会实际fault。

ASan+UBSan/leak通过；PianoFastbootLaunch.c与Boot.c的AArch64 freestanding -Werror syntax通过。脚本另运行root的actualDownloadBlob adapter tests。上述是纯host证据，不是物理PE执行、全shutdown完成、有效高DRAM映射或1GiB传输验收。

下一步root可让USB boot请求在IN ACK结束后转移blob到持久adapter，绑定可信live BS检查/typed all-owner shutdown/Runtime FailStop与budget，再单独运行可返回的小AA64 EFI app。原版boot是否成功仍以真实日志/画面/返回/回归验收；不要现在把这些callback接口宣称已接线。

联合actual-source测试另通过：`python3 tests/test_fastboot_launch_integration.py -v`。四个生产翻译单元（Fastboot、DownloadBlob、Boot parser、Launch）分别编译链接，使用实际命令层完成valid AA64 PE下载；ShutdownAll调用真实FastbootReset，确认borrowed source没有提前zero/free。四个场景覆盖返回应用成功、Load失败后完整Restore、typed release error和warning后保留及禁止重入。该fixture的EFI函数仍是host mock，未执行ARM64指令或操作设备。
