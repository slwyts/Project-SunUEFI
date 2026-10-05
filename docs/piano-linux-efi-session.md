# 产品标准 Linux EFI session

`bootprofiles/os-boot/PianoLinuxEfiSession.c/.h` 把旧 LinuxRamBoot 中真实标准 EFI 机制提取为可复用 session。它不扫描固定 boot RAM、不写固定 kernel/DTB 地址、不走 raw MMU-off 跳转、不增加 diagnostic profile、不修改 Boot Services 函数表或 console vtable。

输入是 reader 提供的三份已验证 immutable CPU snapshot，直接使用统一 PIANO_LAUNCH_BLOB。Take/Borrow 移交并固定 kernel、DTB、initrd，直到相关 image、LoadFile2 和 config table 已退休才 Unborrow/ZeroRelease。Root配置明确 kernel/loaded/DTB/initrd budget，reader本轮总预算仍64MiB。Session不把文件扩展名当Linux身份，也不在owner退休后访问UFS；内核须通过实际AA64 PE parser，DTB用实际libfdt检查并复制到aligned LoaderData。

## 准备与启动顺序

1. 接管并固定三份源，解析AA64 PE；复制DTB，删除旧chosen initrd地址和随机种子，设置与UTF16 LoadOptions一致的ASCII bootargs。
2. Root的实际CheckMemory和ValidateMemory必须确认完整DDR、固定与动态reserved-memory、runtime/cache/ownership以及同一个boot epoch。当前平台合同未ready时返回NOT_READY，没有Start或owner退休。未绑定validator也返回NOT_READY。
3. 保存实际原FDT config table，安装owned FDT；注册真实Linux initrd vendor device path/LoadFile2。已有该vendor路径的provider会被拒，避免内核取到另一份initrd。
4. LoadImage到EFI LoaderCode，记录LoadedImage接口、base、size身份，安装独立LoadOptions副本及BeforeEBS/Exit CPU fence。此时设备尚未退休，正常USB后台服务仍可工作。
5. 最后一个APP service slice后，Root的PrepareHandoff只执行一次真实全owner退休，ValidateRetired核对实际ledger；随后重新采集并验证完整内存合同，epoch必须一致。只有这些精确成功才StartImage。这是明确的OS transition，不是在EBS notify里调用HAL。

两个Root validators必须访问实际平台/owner状态，proof字段只是报告与一致性检查，不能由UI传入TRUE位或旧规划snapshot授权启动。Session不发布dummy memory protocol，不广告Linux Ready。Root尚未将此session与完整DDR及owner registry接入产品，不能以主机mock的ready值宣称设备可启动。

## initrd、FDT及返回生命周期

LoadFile2使用标准LinuxEfiInitrdMediaGuid与真实EFI ABI，先返回required size，再向足够大的目标复制完整CPU initrd；policy=true、错误path/长度被拒。没有设备IO、分配或Start调用。

普通Start返回后只通过新HandleProtocol查询LoadedImage。Mu已自动卸载的旧对象不被解引用；仍存在的对象必须满足接口/base/size/options身份，才恢复原选项并卸载。然后撤销LoadFile2、仅在当前FDT table仍指向自有copy时恢复原表，释放ExitData/options/FDT、关闭events，最后归还sources。任何warning、未知接口、被替换table、uninstall/free/unborrow失败保留资源并拒重入。

内核Start返回EFI_SUCCESS但没有EBS不算Linux启动，session返回EFI_ABORTED，原ImageExitStatus独立保存。返回后若OwnersRetired=true，调用者不能使用旧USB/UFS/input/runtime实例继续GUI；需要冷恢复或经过完整验证的新session/epoch重启。Session不会自行reset或假重启服务。

BeforeEBS代表尝试，不等于成功。它只设置Attempted fence，EFI stub仍可在其内部按标准GetMap/EBS重试；若随后返回父session，任何普通BS cleanup/恢复UI均被禁止。Exit仅标记其通知。测试分别覆盖Before-only与Exit返回，gBS设置为不可访问，确保CPU fail-stop没有后续BS调用。没有改Linux的retry代码，也没有在notify执行设备退休。

## 当前证据与剩余

`tests/test_linux_efi_session.py` 编译实际session、PE parser和固定libfdt，35个实际EFI ABI host场景通过ASAN/UBSAN。包括完整LoadFile2 query/copy、DTB修改、返回自动卸载/显式卸载、FullDDR false/未绑定validator、all-TRUE报告但实际validator拒绝、退休后fresh epoch变化、已存在initrd provider、owner失败、Before-only/Exit fence、各种cleanup retained与pre-transition service slice abort。

ARM64严格语法检查通过。测试不执行ARM内核，不接触平板。仍需Root/平台提供真实完整DDR验收、标准EFI内存图及reservation/cache proof、实际全owner pre-handoff、kernel entry/EBS/kernel console/PID1证据。保留已有raw Linux救援结果，不能用它替代标准EFI session验收。

参考本地标准内核 `drivers/firmware/efi/libstub/efi-stub-helper.c:429`，ExitBootServices失败重试使用已分配map buffer，只再调用GetMemoryMap/EBS；这是Before-attempt后不恢复普通UI/分配清理的依据。

## source admission 与未知结果保护

Source总实际Bytes必须不超过与reader一致的64MiB；先用减法边界检查总量，超过单项/总量或UINT64溢出风险在Take前拒绝。三份Context必须独立且不能落在session对象内，Env/blob描述符同样不能与session别名，避免初始化或后续Take破坏已有状态。

view只能在Borrow后知道其地址；在第一处已知跨源重叠、与session重叠或地址长度回绕时立即retain，不继续取得后面的源，也不调用任何Unborrow/ZeroRelease。Take返回error/warning却给Owner，或Borrow返回error/warning却给view/loan，按未知所有权处理，CPU fail-stop保留整组，不能ordinary cleanup猜测资源已安全归还。

实际C失败注入覆盖这些unknown outputs、相同Context、view交叉别名、session别名、指针回绕和总量超限。test_linux_efi_session.py现在同时执行严格AArch64源语法检查；2项测试包括35个主机生命周期场景，原NotReady/live validator门禁仍保留。
