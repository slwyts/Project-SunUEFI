# Typed UFS retirement for a combined cold-reset finalizer

New cold-reset-only C interfaces in `PianoUfsShutdown.h` preserve the existing revision1 Halt protocol, default normal Stop behavior, and FAT session paths:

```c
EFI_STATUS PianoUfsBlockIoPrepareForReset(VOID);
EFI_STATUS PianoUfsBlockIoShutdownForReset(VOID);
EFI_STATUS PianoUfsStopClocksForReset(VOID);
CONST PIANO_UFS_RESET_REPORT *PianoUfsResetShutdownReport(VOID);
```

The all-owner finalizer sequence is **Prepare UFS → typed USB retirement → Shutdown UFS → ResetSystem only when every owner reported exact EFI_SUCCESS**. No interface here invokes ResetSystem, filesystem writes, or gap restoration. Dirty/NeedsRecovery/quarantined/running experimental window or fixed transaction states reject before consumer callbacks. Bounded mode additionally requires an opened window already closed with whole-gap RestoreVerified; the active test86 session is never modified by this new API.

Prepare verifies our HCI transfer base is still installed, disconnects consumers while media remains live, then halts all32-bit TR/TMR pending masks, both run regs and IRQ with full readback. It leaves mappings, domain and clocks owned across USB retirement. It intentionally holds TPL_CALLBACK across that gap; this interface is for immediate cold reset/fail-stop, never ordinary return to IO or OS boot. A caller that already holds CALLBACK remains at CALLBACK. Failure latches its exact error and retains the TPL/remaining resources; subsequent typed calls return that error without repeating native release attempts. Default ServiceRead rejects prepared/failed reset state. Legacy Stop cannot be mixed after entering reset retirement, and another experiment cannot erase the state.

Shutdown repeats the halt proof, restores saved TR bases with IRQ0, and **does not restore saved TR/TMR run**. All buffers are first checked for Active/quarantined/ExitRetained before any free. Each successful Free must also clear Signature/Mapped/Active; OwnedSmmuClose must leave Attached=false, Domain=NULL, TableMemory signature0 and no Used mapping records. Protocol/event teardown returns exactSuccess. Final HCI readback is cached before clock/GDSC release; no potentially unclocked MMIO is read afterward.

Clock retirement releases IDs in reverse acquired order and removes an ID from the ledger only after exact EFI_SUCCESS. Positive EFI warnings become EFI_DEVICE_ERROR. The GDSC gets the same exactSuccess treatment; a failed clock/domain retains its pointer, ID/count and remaining acquired refs, and legacy VOID clock stop cannot silently drop that failure. These statuses prove **this firmware owner's release callbacks**, not that globally shared clock hardware is physically off.

`PIANO_UFS_RESET_REPORT` retains phases/result/clean/failed/TPL, disconnect/free/protocol counts and final queue/run/IRQ readbacks. Clean is only set after all stages pass. A normal Stop that already restarted inherited runs is not accepted as a typed clean reset. An owner that never installed hardware returns EFI_NOT_STARTED without claiming clean; the finalizer must omit undeclared owners from its roster. EBS-retained ownership is refused. Current native OwnedClose verifies stream detachment and unchanged other SMR/S2CRs; Root's separate coexist audit must still compare other context-bank TTBR/TCR/MAIR and the finalizer must verify USB. `other_owner_quiescence_verified=0` remains explicit in the UFS clean report.

`tests/native/test_ufs_reset_shutdown.sh` compiles actual source with memory-only callbacks and ASan/UBSan. Cases cover live disconnect, high-slot TR/TMR clear, all run/IRQ readback, base restoration without inherited restart, idempotent phases, exact warning failure, ownership drift, stuck queue/IRQ, free/domain/protocol/event/clock failures, pre-free Active fence, post-free predicates, partial clock release/GDSC failure and retained legacy clock guard. Production AArch64 syntax uses **-Wall -Wextra -Werror -Wno-unused-parameter**, as requested after Root found unused legacy-only objects in the bounded full build.

Existing DMA/owned/layout/read-only transport/BlockIO, FAT/Shell/lifetime and bounded core/harness host checks still pass. No prepare, full firmware build, device operation or commit was performed. Root will integrate the combined read-only fetch profile separately; neither live UFS+USB fetch nor a complete combined reset is hardware-verified by these host tests.
