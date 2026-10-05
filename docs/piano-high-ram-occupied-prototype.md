# Piano 1GiB高RAM occupied契约：离线prototype

当前状态为**offline prototype，safe_to_apply=false，ready_for_hardware=false**。工具和host合同测试已准备好；没有改实际88 target、默认map、prepare、内核/config/pins、MMU/allocator/DMA或fastboot高RAM容量公告，也没有设备操作。下一步按Root计划先做实际AT/页表/映射读回和阶段所有权审查，再决定具体受控硬件候选。

新增`tools/piano_high_ram_contract.py`锁定captured live DT、原native C/cache、Mu PEI/HOB/DXE/MMU源码、test85与test88 EFI snapshot、Android86 runtime evidence，并严格重生成原两CMA候选确认44-row base完全一致。默认只输出review JSON；只有显式`--emit-prototype`才写新的独立C，工具没有prepare/apply接口，拒绝platform/upstream/build/private等目录和覆盖88 base artifact。

```sh
# 默认：只写review JSON，不输出MemoryMapLib.c
python3 tools/piano_high_ram_contract.py --output-dir /tmp/piano-high-review

# 单独offline C prototype；仍不prepare/apply
python3 tools/piano_high_ram_contract.py \
  --output-dir artifacts/dram/high-occupied-prototype --emit-prototype

python3 -m unittest discover -s tests -p test_piano_high_ram_contract.py -v
```

已生成`artifacts/dram/high-occupied-prototype/MemoryMapLib.c`，SHA `45d73ca96e8e2769ce08abc8a46fcb7e2ccdf6000f6233f1e4e9a752d4dda311`。review为同目录`high-occupied-prototype.json`。原88候选表SHA仍`70aaab84738198b18a78b0f11f715f15fa4c11295f30befe8ab82a98e693b7cc`；原probe表仍b93f559dc4570b998a519fa0987d02b4b9390ee678772c641a615deb252a8634。

## 精确区域与阶段契约

新增唯一row：

```c
{"Piano_HighRam_Occupied_1GiB", 0xA00000000, 0x40000000,
 AddMem, SYS_MEM, SYS_MEM_CAP, EfiLoaderData,
 ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP},
```

这是half-open `[0xA00000000,0xA40000000)`，大小1073741824bytes，4KiB/2MiB/1GiB对齐，物理地址超过32bit。fixed DT本身的/memory tuple9是`A00000000..C00000000`，完整包含候选；此前planner合并的`8C0000000..C00000000`是更大的连续区域视图，不能将它误报成一个原DTtuple。

已核对候选没有与所有fixed reserved-memory、FDT memreserve、原native/cache以及两CMA共44rows、test85/test88实际EFI descriptors相交。Android86 debug_kinfo的`BFFFFF000..C00000000`、dump_mem的`880000000..885A00000`也不相交。但它们只属于Android运行阶段；动态debug_kinfo/dump_mem的DT alloc-ranges覆盖候选，UEFI阶段实际placement依旧unknown。alloc-ranges是允许范围，不能当已占用整段，也不能因Android当前base不同就把候选标safe。

AddMem+SYS_MEM产生resource HOB；SYSTEM_MEMORY还产生完整EfiLoaderData(Type2) allocation HOB。未来真实DXE如果正确消费该HOB，preboot EFI map保持occupied type2，不开放给普通Conventional allocator；EBS后Linux的Type2/WB usable规则将其重建为System RAM，随后仍由DT处理实际reserved-memory。该区域没有固定reusable CMA定义，符合提供普通RAM的预期；不是承诺全1GiB最终均可自由分配，动态DT保留与kernel自身管理开销仍须处理。

它不改原low heap、FD/BootHandoff、显示WT、native UC/NS_DEVICE、runtime、两CMA或任何原cache属性。保持前44rows完整、新增第45row，无覆盖/重分区。candidate不是永久Reserved/nomap，也不是preboot free download pool。只有occupied HOB和正确DXE消费可防止preboot再分配，JSON中的布尔字段不能完成这件事。

## 本轮实际host验证

7项测试通过，覆盖范围精确/页对齐/DT完整bank、所有fixed/native/FDT/EFI/Android实际owner的冲突拒绝、源SHA与生成中snapshot漂移拒绝、默认CLI不输出C且真实88表未改变、52条test88 map和free/CMA/OOM accounting，以及以下真实C执行：

- `PianoHighRamHobTest.c`复用冻结的实际Mu `MemoryInitPei.c`和完整`PrePiHobLib/Hob.c` harness，加载真正生成的45-row C。实际运行MemoryPeim/AddHob/builders，验证高RAM完整64bit resource+Type2 allocation HOB、无Conventional allocation交集、identity WB-XP输入、前44rows全部不变；原42native和两CMA验证也继续通过。硬件ArmConfigureMmu仍是host shim，只验证传给它的descriptor。
- `PianoArmMmuAttributesTest.c`从固定SHA的真实`ArmMmuLibCore.c`提取并原样编译`TranslationRegimeIsDual`、`ArmMemoryAttributeToPageAttribute`、`SetOutputAddress`、`GetOutputAddress`。真正执行EL1、EL2、EL2E2H的WB/inner-shareable/XP转换，以及候选开始/中间/最后4KiB的64bit输出地址保存；没有以重新实现的枚举转换替代原函数。CPU regime是fixture，不执行实际CPU指令。

两类C均ASan/UBSan通过。实际Mu中`WRITE_BACK_XN`宏仅等于plain WB，因此本row使用真实`ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP` enum；转换EL1&0得到UXN|PXN，EL2单regime得到XN。host证明该函数计算正确，不证明live PTE、MAIR、TCR、translation或物理DDR可访问。

## 硬件候选前必须解决的具体unknown

88 TCR snapshot为480803514、EL1、TTBR0 D7FFF000，当前地址宽度能表示候选；不能由此推导已有translation或物理owner。Root下一步需在实际阶段审查：

1. dynamic reserved-memory和firmware/ABL/UEFI/DMA阶段所有权，确认候选没有隐藏owner或临时活动buffer。
2. 实际TCR/TTBR/MAIR和页表descriptor，bounded AT S1E1R/S1E1W结果；核对候选边界/中间页identity PA、cache、读写权限、XN，保留fault/recovery。仅AT结果也不能替代物理owner证明。
3. 若未来建立该映射，核对page-table页来自已验证low内存，原low heap/native缓存映射未改；没有另造GPI/SMMU mapping。
4. 真实DXE/GCD完整消费1GiB Type2 allocation HOB，EBS snapshot完整覆盖且没有Conventional穿入；继而观察Linux普通Zone/managed/free非CMA增长及是否进入PID1。

本轮只交付这些可复用离线输入、源合同测试和审查出口。工具明确保留ownership/MMU/AT/DXE false和dynamic placement unknown；不把“无已知冲突”写成“可安全启用”。
