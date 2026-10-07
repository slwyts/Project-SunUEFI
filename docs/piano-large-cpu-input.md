# Unified large CPU input and the remaining platform binding

The public ABI is `uefi/core/PianoCpuInput.h`; the OS directory
keeps a relative forwarding header, following its existing shared LaunchBlob
layout. `PianoCpuInput.c` implements one admission/owner validation and one
64 KiB slice policy for file snapshots, borrowed CPU images and download blobs.
It reuses `PIANO_LINUX_MEMORY_PROOF`, rather than creating a second ready flag.

The default remains 64 MiB. An explicit source ceiling can be at most 1 GiB.
Every larger file/bundle/session/borrow requires a current Root `CheckMemory`,
`ValidateMemory`, `ValidateBuffer`, CPU lifetime fence and service slice. The
report must describe full DDR, same nonzero boot epoch, enough normal memory,
fixed/dynamic reservations, runtime regions, cache and current ownership. Even
an all-TRUE report is rejected when the actual Root validator refuses it.
Returned reports are checked for mutation across validation callbacks.

`ValidateBuffer` receives the actual producer, owner token and CPU span. Root
must check them against its real allocation/source registry and live WB map;
the callback cannot simply approve a size or boolean. File allocation is
validated before its first write, again before Ready, and before later copied
reads or zero-release. A failed ownership check retains the uncertain pool
without reading, clearing or freeing it. Borrowed sources preserve Take and
loan lifetime; the helper adds no second image allocation or ownership Take.

File reads and incremental SHA updates use 64 KiB chunks with exact-success
background slices. CPU view hashes, copied reads, Linux LoadFile2 initrd copies
and both file/download zero-release use the same chunk size. Slice warnings
become failure. Service loss prevents further work. Reentrant ownership or
destructive operations during sliced reads/hashes are blocked. Clearing failure
quarantines the partially cleared download and prohibits Restore/retry/free.

Linux session total-source budget defaults to 64 MiB; Root may request up to
1 GiB only through the actual CPU policy. The source proof is checked before
Take; each returned source view is checked against its owner and memory map.
The later DT-based handoff memory proof must have the same boot epoch/DRAM
identity. Source aliases, overflow and LoadFile2 writes into the session or any
immutable source are refused. Root full-memory and all-owner handoff validators
remain mandatory before StartImage.

`PianoFastbootBootParse` itself has no 256 MiB image cap. It uses bounded64-bit
source intervals and32-bit PE/Android header fields. The 256 MiB limit in
`PianoProductPayload` protects the specifically pinned SimpleInit boot-RAM
carrier and is intentionally unchanged. The transport's
`PIANO_FASTBOOT_MAX_DOWNLOAD`, command admission and advertisement remain64MiB.
`PianoFastbootDownloadBlobBindWithCpu` can accept a future genuinely owned large
CPU download source, but cannot create one or advertise capacity. Old Bind and
CpuLoanBegin refuse sources above64MiB.

Product integration requires adding `PianoCpuInput.c` to `OS_BOOT_SOURCES`.
Normal shared-header staging captures the actual ABI, while `OsBoot` captures
the forwarding header and implementation. Existing diagnostic RAM-boot staging
also copies this one dependency; no new profile, capacity or feature flag is
introduced. The actual Root platform allocator/source policy remains unbound,
and the product status must remain `COMPILED_PLATFORM_NOT_READY` with64MiB.

## Verification

Actual source tests cover21 unified admission/slice/hash/copy/zero cases,
35 file-reader cases,42 Linux EFI lifecycle cases, the existing CPU-loan and
download-to-launch integrations, and a67,174,401-byte download/borrow/copy/
revalidation/zero-release case with4,110 service slices. Source authority and
hardware calls are injected host boundaries, not hardware evidence.

The actual882,173,440-byte GNOME initramfs was additionally passed through the
real BootFileSource code using a read-only host SFS adapter:13,461 chunks,
26,925 slices, independent complete SHA256 comparison, final-byte readback,
owner/loan lifecycle and full zero-before-FreePool validation passed. Log:
`build/logs/piano-bootfile-full-root.log`. This validates the large CPU path's
real work, not ARM firmware allocation, UFS bandwidth, USB1GiB downloads or
Linux kernel execution.

## Fastboot lifetime at the OS transition is still unresolved

The existing session retires hardware through `PrepareHandoff` before
`StartImage`. Actual Linux `efi_stub_common()` subsequently calls
`efi_load_initrd()` and enters `efi_boot_kernel()`. Therefore the initrd
LoadFile2 copy currently happens after USB retirement. Its slices can only
perform CPU lifetime checks in this old transition model. They do not keep
fastboot running and do not fulfill the requested whole-UEFI-lifetime service.
The helper never reinitializes retired USB. This remains a product blocker.

Two concrete later APP-phase integration points follow from actual source:

1. **Native firmware ExitBootServices entry**, preferred for Linux/Windows/
   arbitrary EFI loaders. Add an explicitly registered product handoff adapter
   to Mu `CoreExitBootServices` *before* the first
   `CoreNotifySignalList(BeforeExitBootServices)` and timer/map termination.
   Validate the actual selected image handle, APP TPL, current map key and
   registered session before touching owners. Run the genuine existing owner
   retirement once, validate clean/no-DMA plus the final memory contract, then
   enter the original Core path. Retirement may change the map key: return the
   standard INVALID_PARAMETER result and let the caller obtain the new map.
   On retry, inspect the real same-epoch clean ledger and never retire twice.
   Partial/unknown retirement must fail-stop/recover, rather than return control
   to an EFI loader with uncertain hardware. A source-level adapter is required;
   overwriting the public Boot Services function table or using notify TPL for
   the APP-only retirement is not an implementation of this design.
2. **Cooperative kernel EFI-stub call before final map acquisition**. The actual
   `efi_exit_boot_services()` first obtains a map, processes it and calls EBS;
   its retry performs only GetMemoryMap/EBS with the existing buffer. A tiny
   canonical Piano topic commit can invoke a real product transition protocol
   at APP *before the first* `efi_get_memory_map()`. Root authenticates the real
   LoadedImage handle/session and executes the same retirement; the subsequent
   map already includes its changes. This avoids a deliberate stale first key
   but requires both stable/next kernels to cooperate and does not solve the
   Windows/general-EFI goal. It is a possible intermediate implementation only.

Neither adapter is implemented or marked ready in this CPU-input unit. Actual
local source boundaries are Mu `DxeMain.c:778`/`Mem/Page.c:2431`, and Linux
`efi-stub.c:172`, `fdt.c:292`, `efi-stub-helper.c:429`. The public
[Linux EFI stub source](https://github.com/torvalds/linux/blob/master/drivers/firmware/efi/libstub/efi-stub-helper.c)
also documents the GetMemoryMap/EBS-only retry restriction. The live Root
registration, real device retirement, stale-key retry and returned-loader
recovery still need source tests and device verification before changing policy.
