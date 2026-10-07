# 一次性 bounded FAT/SFS session（仅主机验收）

新增 `--ufs-bounded-filesystem-test`，默认关闭，与固定write/preflight互斥，拒绝原filesystem/Shell/Setup/USB/USB screenshot/touch/QUP等消费者。Root的USB screenshot小补丁保持不变。此profile仅发布14MiB窗口，复制bounded core/layout/private transport与新harness，加入真正EnglishDxe/Fat FDF模块；不定义原 `PIANO_UFS_FILESYSTEMS` probe、不发布原父LUN、不加载SimpleInit/OS。

本轮没有执行prepare、完整固件构建、设备操作或commit。Root已创建并fsck -n检查的主机FAT12 image是输入，SHA256 `1e6d98314d880f88d6537fb466d3ec87b294422538bdc5795c574b6a1e713add`，4KiB非零logical块仅0/1/3/5。`tools/prepare_ufs_bounded_fs_test.py`每次验证固定PC whole-gap archive，然后独立检查完整image size/SHA、typed manifest、全部3584块非零extent列表和四块SHA、二次读取无漂移；只输出四块+hash的C header，不调用mkfs或操作设备。

harness在写前重构3584块的稀疏image SHA并对照固定已知值、逐块hash与logical列表，确认runtime窗口Opened、尚未Dirty/NeedsRecovery、零写attempt、4096/3583媒体几何，才开始session。

## 同session顺序

1. 整个session外层RaiseTPL(TPL_CALLBACK)。四个format块通过真实bounded BlockIO WriteBlocks，继承每块freshcap/WP/GPT、FUA/sync/独立RX验证。
2. 只Connect window handle，再在**该同一handle**获取真正SimpleFileSystem。没有global fs0替代或自制SFS。
3. OpenVolume → Root.Open普通 `SUNTEST.BIN`（CREATE|READ|WRITE，attrs0）→ 写8193-byte确定性内容 → Flush → Close。
4. 重新Open READ，GetInfo检查普通文件/size8193，poison独立8193-byte RX后Read逐字节比较，再Read1要求EOF0；关闭Reader与Root。
5. 无论格式或SFS阶段是否失败，都关闭public window：请求Disconnect consumer、记录真实DetachStatus，再CloseForRecovery禁止缓存晚写。
6. 持有专用recovery lease，fresh WP/capability gate后读取全部3584 gap块。只对实际非零块置attempt ledger再WRITE original zero4KiB/FUA → full-window sync → quiet → 独立READ全零校验；不自动重试失败。
7. 全窗final sync，再**仍持有同一lease**调用VerifyRestored。它嵌套完整3584块独立比较和fresh双GPT/WP，实际成功才清Dirty/NeedsRecovery。
8. 检查Release返回状态及provider state无Dirty/NeedsRecovery/quarantine、RestoreVerified1，打印静态最终摘要并最后释放外层TPL。到期timer此时才能Halt/冷重启。

任何queue不明、ownership变化、restore/readback/verify/release失败都会保留外层TPL/recovery lease和ledger，CpuDeadLoop，不Reset、不进入下一启动。物理断电不受此fence控制。原始gap为固定完整零dataset；省略零块恢复写不省略完整最终读校验。Format/SFS失败但whole-gap恢复成功会返回原first failure，不能假称文件测试成功。

## TPL契约

[UEFI Boot Services章节的TPL规则](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html)允许Simple File System调用至TPL_CALLBACK。固定Mu树FatPkg/Data.c的FatFsLock=TPL_CALLBACK、FatTaskLock=TPL_NOTIFY；同步Read/Write/Flush用Token=NULL，不创建非阻塞task。此profile只暴露BlockIO、没有BlockIO2/异步FileEx任务，独立新文件的task queue为空。固定DXE DriverSupport.c的Connect/Disconnect没有Application-only ASSERT；此路径调用协议/内存服务的锁不低于CALLBACK。harness不调用WaitForEvent、ReadKey、异步FileEx或UI输入，内部IO lease释放到外层CALLBACK，不让timer穿插。

SFS重新Open/Read验收文件API/metadata行为；FAT可能保留cache，因此不能将它单独当作物理读回证明。实际bounded backend已在每个物理WRITE后执行独立fresh DMA READ比较，恢复后又独立全窗证明；下一轮设备阶段仍必须外部Android/PC capture复核。

## 静态最终报告与测试

`SUNUEFI_UFS_FS_SESSION`记录started/returned/lease/recovery_lease、first/final、format_blocks、file_match、scanned/nonzero/restore_attempts/restored/whole_verified。26个`FS_STEP`记录每步attempted/returned/status/bytes；`FS_RESTORE_LAST`在callback前标记最后physical/attempted，返回后记录returned/transferred与write/sync/read/quiet。Report在源Halt与异常diagnostic重发，未知恢复不报whole_verified。成功应format_blocks4、file_match1、scanned3584、restored=nonzero、whole_verified1；任何mock成功不构成硬件成功。

可复现电脑端入口：

```bash
bash tests/native/test_ufs_bounded_fs.sh
python3 -m unittest discover -s tests/unit -p 'test_ufs_bounded_fs_profile.py'
bash tests/native/test_ufs_bounded_block.sh
```

actual C harness 26个memory-only场景通过ASan/UBSan，包含成功与format/Connect/SFS/OpenVolume/CREATE/short write/flush/close/GetInfo/read/EOF错误后同session恢复；恢复Acquire/WP/read/WRITE/short WRITE/unknown quiet/sync/whole verify/release不明均fence。10个纯profile/PC image tests通过，涵盖默认关闭、全部隔离（含USB screenshot）、无OS路径、pinned image/size/manifest类型/extent/symlink/PC archive漂移；测试仅临时render/header，不运行main/prepare或改变staging。production bounded+harness宏下AArch64语法通过。

源码入口为PianoUfsBoundedFileSystemTest.c/.h；profile将两宏PIANO_UFS_BOUNDED_VOLUME/PIANO_UFS_BOUNDED_FS_TEST只加到复制的UfsDma，Sources包括harness/core/layout/helper，新增gEfiFileInfoGuid及真实SFS协议依赖。这些主机检查之后，标准EnhancedFatDxe实机验收结果见下一节。
# 第86次实机验收

第86次已经完成真实限定窗口BlockIO与EnhancedFatDxe/SimpleFileSystem闭环。25个步骤全部Success：4个格式块、OpenVolume/Create `SUNTEST.BIN`、Write8193/Flush/Close、重新Open/GetInfo/Read8193逐字节匹配/EOF、Disconnect、关闭public IO、恢复与整段验证。provider累计18个请求、9次WriteBlocks、11个经过FUA/sync/独立DMA读回的写块、8次Flush、0失败。文件重新打开使用标准FAT缓存语义，物理持久数据的独立证据来自每次底层写块校验。

恢复扫描3584块，发现并恢复7个非零块；最终完整14MiB读取匹配原始全零基线。18次真实WRITE门铃（11次格式/文件块+7次恢复）、27次SYNC门铃，原LUN句柄发布数0。最终Dirty/NeedsRecovery/quarantine均0、restore_verified/whole_verified均1、消费者已断开且public IO已关闭。

Android恢复后capture4重新读取完整gap、MBR、主备GPT头/数组和相邻块，九份对象逐字节匹配原capture1及本次执行前capture3；26启动分区SHA一致，A槽位/root正常。封存证据为 `artifacts/ufs/bounded-fs-validation-test-86.json`，镜像SHA `0445da01dc9078b7b5d550c234679cbf5d5420d675f888f0ac627936ad2f8ad5`，build_id `7174ffe0-633b-41df-b2e5-968c6de83854`。大量传输使session开头被RAM环覆盖，完整raw console保留最终guard/provider/harness报告，不冒称完整启动日志。

本次是临时测试卷，未改GPT、未保留新文件系统。持续可写ESP、Shell文件复制、持久变量与自主OS加载仍需后续独立验证。
