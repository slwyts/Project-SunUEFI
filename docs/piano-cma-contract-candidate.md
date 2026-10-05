# 两段CMA EFI合同候选：默认关闭，待硬件验收

此候选仅处理同机enabled fixed reusable pools：QDSS `[82800000,84800000)`32MiB和TrustUI `[F3800000,F8800000)`80MiB。kernel/DTB/CMA策略保持原样；没有fullDRAM map、1GiB下载cap或permanent Reserved+nomap转换。原native42rows、FD/heap/window/cache不覆盖，新candidate为44rows。

离线生成入口：

```sh
python3 tools/piano_cma_contract.py --output-dir artifacts/dram/cma-contract-candidate
```

产物为`MemoryMapLib.c`和`cma-contract-candidate.json`，status HOST_CMA_CANDIDATE_NOT_HARDWARE_VERIFIED。table SHA `70aaab84738198b18a78b0f11f715f15fa4c11295f30befe8ab82a98e693b7cc`。工具禁止离线publish到platforms/upstream source；本轮没有运行实际prepare或fullbuild。

Root显式准备独立Linux target时使用：

```sh
python3 tools/prepare_linux_profile.py --cma-contract-candidate
```

该flag默认关闭，与raw互斥。所有固定输入、DT属性/范围和Mu源码语义预检发生在创建target之前；只向派生pianoLinuxPkg的独立MemoryMapLib追加两rows，并写入strict snapshot，再复制到派生upstream target。原pianoProbe/pianoPkg真源不动。不开flag时恢复原native table、只清理由此工具产生的typed candidate snapshot，未知文件不删除；default85路径保持原有行为。

## 实际Mu语义与XN细节

固定Mu source中Silicium SEC `InitializeMemory`调用MemoryPeim。真实`MemoryInitPei.c:AddHob`对AddMem构建resource HOB；只要ResourceType==SYSTEM_MEMORY还构建allocation HOB并保留指定MemoryType。候选使用AddMem、SYS_MEM、SYS_MEM_CAP和显式EfiLoaderData枚举，实际值2由真实UEFI headers/host断言确认，不按其他表的magic number猜测。

DXE GCD源码随后遍历allocation HOB，以AllocateAddress占用整段GCD memory，再将HOB MemoryType传给CoreAddMemoryDescriptor；普通page allocator寻找空闲时只选择EfiConventionalMemory。于是候选的设计合同是EBS前占用、不作Conventional分配；EBS后Linux把LoaderData+WB视为System RAM，而同一DT reserved-memory/CMA继续保留管理它。真正DXE消费成功及allocator行为仍需实际memory map验证。

重要：本Mu `MemoryMapLib.h`里的`WRITE_BACK_XN`只是普通WRITE_BACK别名，不能把名字当实际XN。新rows使用真正`ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP`枚举；ArmMmuLib把它变成WB+XN（single regime）或WB+UXN/PXN（dual）。原rows旧alias和cache属性完全保留。host测试验证MMU表传参为XP，尚未证明实机page-table permissions或physical ownership。

## 固定输入与拒绝条件

候选固定live.dtb SHA a4b55d...，current native C SHA b93f55...、native JSON b7f5e8...和test85 console6c6c5d...；同时固定MemoryMapLib.h、MemoryInitPei、PrePiHobLib、GCD、Page allocator、ArmLib和ArmMmuLib源码SHA。任一drift立即拒绝，不自动沿用新板/新表。snapshot生成结束前再次读取确认输入未改变。

DT每个pool必须exactly one固定tuple，完整位于捕获DRAM，enabled/reusable/shared-dma-pool/无no-map、base/size严格吻合且4KiB aligned。任何原native cache/归档EFI allocation/其他fixed reservation/另pool相交都拒绝。候选source解析后逐项比较原42rows与新增两rows，原字段不改，新增只能type EfiLoaderData和实际XP枚举；descriptor总数须低于真实Mu128项限制。原table最后新增delimiter逗号不改变原row字段。

## Host验证与尚未验证

```sh
python3 -m unittest discover -s tests -p test_piano_cma_contract.py -v
python3 -m unittest discover -s tests -p test_piano_cma_hob.py -v
```

前者6tests包含真实固定生成、pool属性/范围/tuple漂移、native/EFI/其他reserve碰撞、source/mid-generation漂移，以及仅临时fixture内的default/opt-in/回default派生prepare与raw互斥。stdout中的prepared文案来自TemporaryDirectory，真实staging未运行。

后者直接include真实MemoryInitPei/AddHob/MemoryPeim及整份真实PrePiHobLib/Hob.c，HobConstructor/CreateHob/resource/allocation builder真实执行；只stub硬件ArmConfigureMmu、宿主PCD/排序、HOB-list pointer和无关PE loader。ASan/UBSan验证两pool的SystemMemory resource HOB+整段Type2 allocation HOB，无Conventional overlap；MMU identity descriptors为XP，其他42native属性/address/length不变。还覆盖default无candidate、HobOnly不映射、AllocOnly、reserved nonSYS不allocation、MMU失败、overlap死循环拦截和非aligned allocation ASSERT。

本轮完成后实际pianoProbe C、platforms/pianoLinux C与upstream派生pianoLinux C仍均为b93f559dc4570b998a519fa0987d02b4b9390ee678772c641a615deb252a8634，证明没有提前应用候选。Root后续prepare/build才会改变派生target。

以上不证明真实DXE GCD占用、EFI type2最终出现、ARM映射/ownership正确、CMA activation成功或完整Linux `/init`。Root88左右须对照新完整EFI map恰两新增occupied ranges、原rows/cache不变、kernel sparse/CMA/panic/PID1与Android26SHA恢复。当前hardware/ownership/MMU verified全部false，memory-debug c4bb Image和原DTB保留。
