# Shared UFS/USB retirement: physical tests90/91

Test90 preserved 21 valid short diagnostic records, zero CRC-invalid copies and zero unsealed prefixes. The known USB stream slot is1, not113. UFS baseline row1 was SMR00000540/S2CR00020001 (inactive legacy residue); after USB closes, snapshot and two direct MMIO samples all read0/0. Both Primary/Copy CRCs passed. Slot113 baseline remainedSMR0/S2CR0300006E. Test90 strict close rejected row1 and retained tables; user recovered Android, all26 boot hashes matched.

The fixed SerialPort DebugLib has a256-byte format buffer. Earlier long rejection/peer lines were cut beforeCRC, including both mirrors; they were not complete hardware evidence. Short reports now seal each before/after/live sample separately, under the real prefix+CRC budget. Decoder isolates every prefix so a truncated line cannot swallow a valid concatenated mirror.

The common lifecycle fix preserves each original Before snapshot. USB producer records actual attached SID40/slot/CB/root and exact detach/capture/destroy/table-free ledger, plus Halt/9DMA free/8clocks/GDSC evidence. It builds a one-use retired proof from a fresh snapshot. UFS checks the peer row is identical in both original baselines, its own live context is stable, all other raw rows and known contexts are unchanged, then stores an explicit contract for exactly that peer row. After UFS detach, only that contract-authorized0/0 row is accepted; all other changes still reject. No global invalid-row exemption or baseline rewrite.

Test91 physically fetched exactly`xbl_config_a`524288bytes via stockfastboot, matching the original backup SHA`e95c1e673bee3a400a7f9992fe23f43ecbc5cc5ff6f0312003ec948b08e2b8fc`. No UFS writes occurred. Its actual logs show:

```text
SUNUEFI_UFS_ACCEPT_RETIRED_USB slot=1 ... exact_contract=1
SUNUEFI_FETCH_RETIRED_USB_CONTRACT status=Success valid=1 slot=1
SUNUEFI_SMMU_PEER_RETIRE_VERIFIED ... baseline_unchanged=1
SUNUEFI_UFS_RESET_SHUTDOWN clean=1 ... dma_freed=3 domain_closed=1 protocols_removed=7 clocks_released=1
SUNUEFI_FETCH_ALL_OWNERS_STOPPED usb=Success ufs_prepare=Success ufs_shutdown=Success clean=1 reboot=1
SUNUEFI_FETCH_REBOOT_CLEAN all_owners_retired=1
```

Firmware issued the clean reboot and Android returned; system boot-complete1, root and slotA remained normal, all26 boot hashes matched again. Build_id`cee94c0f-62ec-4146-b06f-342a5a51e395`, imageSHA`103cfa3b3d3c5d9eb39144e18b61b77dd4c84ef8ea503b914483d1f0c2aa5d50`. Sealed metadata`artifacts/usb-debug/fetch-validation-test-91.json`; raw capture`private/analysis/ramlog-test-91/console.txt`.

This proves one readonly partition fetch and combined retirement. It does not prove sustained resident service across SimpleInit/Setup/Shell, every partition, original-volume writes,1GiB downloads or OS handoff. The same retirement implementation is being reused by the single product core; experimental capture names are development evidence, not alternate shipping feature sets.
