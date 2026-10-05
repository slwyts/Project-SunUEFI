# Isolated readonly device profiles

2026-10-05新增两条可完整构建的生产诊断入口，当前只完成host/build，尚无实机结果。第89次原镜像已独立封存并核对SHA，新构建不替换该测试证据；平板尚未恢复前不启动新测试。

## Pogo SE6 snapshot

`--pogo-register-probe`调用实际`PianoProbePogo(Fdt)`，需要native foundation中的Clock协议，但不调用I2C Open或加载固件。已有native QUP Device mapping为800000..B00000，包含A98000/4000与AC0000/2000；没有追加重复MMIO mapping。GCD/每页AT、Clock只读查询及独占exception guard通过后，读取wrapper4项和SE6 30项，全部CRC/mirror。

```sh
python3 tools/prepare_gui_profile.py --pogo-register-probe --return-seconds 75
bash tools/build_stage0.sh gui
python3 tools/package_stage0.py --profile gui --header-version 3
```

不先安装generic FaultRecovery，避免占用相同handler。精确same-EL LDR read abort可恢复并报NOT_READY；SError/foreign fault/注销失败保留并fail-stop。软件10ms检查不能中断永不响应的总线，`hard_bus_timeout_verified=0`。clock off/unknown时不读SE；快照成功也不证明MCU runtime、GPIO/rail、PIO事务或键盘输入已可用。

封存`artifacts/diagnostics/pogo-register-probe-v1`，build_id `ba8e2731-7be0-4336-994a-3da6ba781aed`，image SHA `bbfe4a04e474bd52ca1f48958a38308f594806f712b9e390fef4ec8986100912`。实际C host测试24 opt-in+defaultoff通过，生产ARM64 object和完整EDK2构建通过。

## High address AT/EFI/GCD

`--high-ram-readonly`不加载native foundation、USB/UFS或SimpleInit。调用actual HighRam helper首点，然后额外记录A20000000、A3FFFF000、A3FFFFFFF三点的AT/EFI/GCD。只读入口保持regime/table ownership attestation FALSE、MemoryAttribute/ReadTableWord/AtWrite NULL，pattern0；标准MemoryAttribute.Get也不调用，因为当前Mu内部会裸读PTE。

```sh
python3 tools/prepare_gui_profile.py --high-ram-readonly --return-seconds 75
bash tools/build_stage0.sh gui
python3 tools/package_stage0.py --profile gui --header-version 3
```

保留ordinary fault logging及原timer，但没有映射更新、target read/write或DMA。RowBudget1与四点诊断不是全1GiB验证；metadata不完整/NOT_READY为预期，所有phase ownership仍unknown。它不会把候选交给allocator，也不会扩大fastboot公告。

封存`artifacts/diagnostics/high-ram-readonly-v1`，build_id `70da647d-1321-4059-b85d-27e3d9c97359`，image SHA `1c71ca677f8610733f5ec914c115eba1a592bd547eef7fb8e54e03bae2e6a842`。helper实际C/ARM64检查和完整EDK2构建通过，尚未取得实际AT或高RAM内容证据。

两种profile都强制排除其他硬件消费者及OS load；`tests/test_new_diagnostic_profiles.py`在临时repo运行真实prepare，验证隔离和生成内容，不修改当前staging。后续device boot、保留日志、完整启动分区SHA核对使用单独新test-id。
