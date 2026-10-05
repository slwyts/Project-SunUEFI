# Guarded Read32 DXE adapter

`bootprofiles/guarded-read/PianoGuardedRead.c/h` implements an actual AArch64
guarded LDR32 using the exception, AT and mapping approach already present in
`PianoPogoProbe.c`. This is a singleton DXE adapter with static producer storage,
not a SEC memory provider. It does not install memory, map pages, walk PTE tables,
write the target, flush caches, initialize native drivers, or authorize DMA.
`MemoryOwnershipGranted` is always false.

## Caller and ranges

The caller supplies real Boot Services, DXE Services, a genuine CPU-only
`BootServicesAlive` fence, and at most four trusted physical ranges. Each range
fixes the exact permitted GCD type, cache class and PAR memory attribute. Supported
observations are device UC (`00/04/08/0c`), normal noncached UC (`44`), or normal WB
(`ff`). Addresses and lengths are multiples of four, nonempty and bounded by the
current VA/PA widths. Ranges may not overlap. Range permission cannot be expanded
from a cookie or data read from the target.

`Begin` and `End` require APP TPL. `Begin` checks the real lifetime fence before
dereferencing service tables, takes a private configuration snapshot, and creates
an independent ExitBootServices event. It registers the synchronous and SError
handlers using the standard CPU Arch protocol. `EFI_ALREADY_STARTED` leaves a
foreign handler intact; only an exact successful registration is marked owned.
There is no unconditional unregister before registration and no takeover of the
existing product fault handler.

The standard CPU Arch protocol has no method to inspect a currently registered
handler. The caller must therefore reserve exclusive exception-registration use
for the session. The pinned Mu AArch64 `RegisterCpuInterruptHandler` implementation
returns `EFI_ALREADY_STARTED` for an occupied slot before writing it. A concurrent
actor that unregisters and replaces an owned slot would break this caller contract;
the adapter does not claim to detect such a replacement through a nonexistent API.
The intended product window is after native Foundation and before USB/UFS/fault
owners start, with normal product fault recovery registered only after `End`.

For every covered page, `Begin` uses real `GetMemorySpaceDescriptor` and
`AT S1E1R`, requires a complete GCD page with the configured type/cache and no read
protection, and requires identity translation with the exact configured PAR
attribute. The live page is checked again before each LDR. Fresh EL1, SCTLR, TCR,
TTBR0, TTBR1 and MAIR must match the original observation before and after a read.
The CPU protocol pointer and registration method must also remain unchanged.
This observes the mapping; it does not create or repair one.
Every mapping attempt records `LastMappingPage`, `LastGcdType`,
`LastGcdAttributes`, `LastPar` and the returned `MappingStatus`, including refusal
and lifetime loss. `LastPar=MAX_UINT64` means no AT result was obtained on that
attempt. This distinguishes a GCD refusal from an actual device/normal cache
attribute mismatch without another hardware operation or a relaxed check.

## Reads and exception recovery

`Begin` returns a globally monotonic opaque token. `PianoGuardedTryRead` matches
that token and checks active, nonreentrant state before using the service fence.
It copies at most 256 aligned bytes using actual 32-bit LDRs into internal scratch.
Only a fully successful copy is committed to the caller's valid CPU destination;
failed reads leave it unchanged. Output ranges cannot alias producer state,
configured target ranges, CPU/BS/DS protocol tables or the configuration. Tokens
from an ended session cannot authorize a later session.

The inline assembly records the exact LDR and resume addresses, masks IRQ/FIQ,
allows SError, and restores DAIF after the guarded instruction. AT preserves PAR
and DAIF. A synchronous exception is recovered only when CurrentEL is EL1, its
PC and FAR exactly match this armed LDR, ESR describes a same-EL data read abort
with a valid FAR and 32-bit instruction, and SPSR returns to EL1t/EL1h. Recovery
changes only saved ELR to the recorded resume label and reports the failed read.
Write aborts, different PC/FAR/EL, recursion, SError or other exceptions retain the
producer and halt the CPU. Exception and EBS callbacks do not perform protocol
lookup, Boot Services cleanup, native teardown or reset.

The timer and read budgets are absolute to the complete session, including page
validation. Hard limits are 1,024 validated pages, 65,536 attempted LDRs and
100,000 microseconds; the caller selects a smaller nonzero count/time limit as
needed. Counter direction, bounds and monotonicity are checked. These software
budgets cannot interrupt a bus transaction that never completes or a DSB that
stalls, and exception recovery is not a verified hardware bus timeout.

## Cleanup and SMEM use

`End` unregisters only handlers it acquired with exact success, in reverse order,
then closes its EBS event. Any warning, unexpected protocol identity, CPU-state
change or unsuccessful unregister/event close retains the producer and refuses
reentry or a cleanup retry. EBS flags the producer lost and blocks further BS
calls. A caller observing `Retained` must fail-stop rather than continue product
initialization; an unknown handler result can leave registration TPL held. A clean
end permits a new token/session. No DMA or memory ownership is granted by either
success or cleanup.

The TryRead signature matches `PIANO_SMEM_TRY_READ`, so Root can bind it to
`PianoSmemRamCollect` after a successful `Begin` and must call `End` even when the
collector rejects its snapshot. SMEM's current product row is reserved UC memory;
its expected normal-NC versus device attribute must be verified by actual AT.
The named native TCSR region covers the `01fd4000` cookie page. No mapping is
added by this adapter; failed actual GCD/AT checks refuse the range. Missing
mapping evidence must not be repaired or replaced with a fabricated cookie value.
Product Core now calls the Root SMEM wrapper through this adapter after
Foundation and before UFS/USB. Its trusted ranges come from the exact SMEM and
TCSR native rows. A mapping rejection remains diagnostic; unknown handler or
lifetime ownership stops further initialization. Product physical validation
is still pending. Pogo was not changed and no tablet reads were performed by
the host verification.

## Verification

```sh
python3 -m unittest discover -s tests -p test_guarded_read.py -v
```

The test runs the actual adapter C in 41 isolated ASan/UBSan cases with EFI and
architectural boundary fixtures. It covers foreign handlers, warning acquisition,
exact unregister and retained failures, GCD/AT/cache/CPU refusals, read and timer
budgets, exact abort recovery, asynchronous and unknown fault fail-stop, EBS during
mapping/read, unchanged failed output, opaque-token reuse refusal, aliases to target
and producer storage, Begin aliases to service tables, and inaccessible service
tables behind a false initial lifetime fence. The actual unmodified ARM code also
compiles into an AArch64 object with strict warnings, validating the LDR labels,
fixup stores and DAIF/PAR instructions. Host faults are injected contexts, not
physical aborts; no device operation or complete firmware build was performed.
