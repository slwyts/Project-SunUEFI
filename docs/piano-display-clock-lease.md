# Narrow product-owned GCC display AHB reference

`PianoDisplayClockLease.c/.h` acquires exactly one native reference for
`gcc_disp_ahb_clk`. Root binds it to its actual product owner lifecycle; this
module does not edit Core/Owners, start another USB/UFS instance, change rates,
reset a controller or enable MDP/GDSC. It has not been run on the tablet.

Test101's30 protected observations show AHB CBCR bit0 clearing during native
Clock Start while HF-AXI stays unchanged. The pinned native CESTA descriptor's
four-clock list includes GCC display AHB first. C420/C46C/C480 gets/enables the
four refs; after its tables are initialized, C6D0 calls C4B4, whose C4E8/C500/
C514 releases them in reverse order. GCC is last. The type1 Disable only calls
HAL disable when its total reference is1, then decrements native counters.
This path is compatible with the observed AHB change. Live native counter
values were not captured in101, so the last-ref explanation remains a source-
and-register inference rather than an already-proven private counter event.

Acquire verifies the complete captured Clock PE SHA f9e85aa7...6fdb769e, actual
LoadedImage base/size44000 and BootServicesCode/Data types, then compares the
live27000-byte text against an owned relocation-normalized FV copy. It checks
the exact protocol location28148, Version1000b, method RVAs and power-domain
aliases, with fresh identity before every mutation. Source bytes, live text,
wrong types, replaced interfaces, partial outputs and warnings cannot grant a
lease. The FV copy is freed only after exact success; uncertain copies/events
are retained in the driver-lifetime context.

Root supplies two hardware-boundary callbacks: ReadCpu copies at most256 bytes
only from fresh verified native typed objects; ReadGcc reuses real
PianoGuardedRead Begin/four GCC127004/127008 reads/End, returning exact cleanup
and mapping evidence. The reader is a separate adapter; no guessed pointer or
WB bit grants general low-heap access, allocation, DMA or high-DDR permission.
Every BS boundary checks the CPU EBS fence; App checks again after RaiseTPL
before touching RestoreTPL. The same persistent context owns the EBS event.

GetID starts with MAX_UINTN and must produce a32-bit typed kind1 ID. The module
resolves the real module/clock arrays and requires exact GCC node33a68/name14e02.
It rejects native skip/disable-suppress flags, alternate vote-class selection,
malformed/cyclic graphs, pointer arithmetic wrap and counter saturation. The
successful native getter can create its24-byte client reference through5148;
the baseline is therefore sampled after GetID. Native Disable decrements refs
without unlinking/freeing this entry, so a legitimate first entry is supported.

`Total[2]` and `PerClient[2]` are two native counter classes: ordinary[0] at
node50/client10 and the bit9-selected alternate[1] at node52/client12. They are
not duplicated reads. Each report contains `MatchingSnapshots=2` only after
two complete independent decoded snapshots compare equal. Acquire requires
ordinary total/client each+1 while alternate is unchanged, then owns1 ref.
Release samples the current counters, requires ordinary total/client each-1
and alternate unchanged during that release, then moves its own ledger1→0.
Other legitimate refs may have changed since Acquire; Release does not compare
their absolute counts to startup. Object identity, selected class and control
flags remain checked.

After Enable, both native BOOLEAN outputs start with the0xA5 sentinel.
`IsClockEnabled` must return exact success and actually write TRUE.
`IsClockOn` must return exact success and write canonical0/1; its value is
diagnostic and FALSE is permitted. In the pinned PE, type1 IsOn7444→F98C loads
the GCC AHB HAL callback FF0C. That callback reads the CBCR, shifts right28 and
returns TRUE only for the top nibble0 or2. The captured idle CBCR88000003 thus
returns FALSE even with bit0 set. No HWCG/HALT_SKIP exemption is present in
FF0C. IsEnabled7338→F96C instead reaches FED4, which tests the enable mask;
this AHB descriptor has no separate vote register, so the mask is CBCR bit0.
The Linux3525 branch wait also skips on-bit waiting in hardware gated mode;
gcc-sm8750.c keeps this AHB enabled directly at offset27004.

The protected GCC pair must be stable, AHB bit0 set, HF-AXI unchanged and other
AHB bits stable apart from enable/on-status. This combines native refs with
actual readback;
Enable success or an inherited ON bit alone cannot establish ownership. After
release, independent guarded readback and exact event close are required. The
module reports release of its own native ref, never global hardware-off.
Errors, warnings, lost services, inconsistent counter copies, failed readback
or partial release retain state and prohibit a second Disable attempt.

Root manager bindings use actual report fields Baseline/Acquired/ReleaseBefore/
Retired with ordinary/alternate counters and MatchingSnapshots, OwnedReferences,
CounterStatus, ReleaseReadbackStatus, ClockId/NativeBase, guard reports and exact
callback cleanup. The default unbound module grants no display readiness. A
reader not ready or a native identity mismatch before Get/Enable has no clock
reference side effect; Root may distinguish that exact unheld result from any
retained or attempted native mutation.

The first live-text read has one narrow clean-refusal path. The optional
`GetReadFailureEvidence` callback must return the actual Reader's fresh first
call sequence, exact address/length/status and map failure, with zero sessions,
reads, active/owned exception handlers, fatal/retained/lost state and no busy
reader. Only `EFI_NOT_READY` at image+1000 for the first256 bytes qualifies,
before any Lease event, GetID, Enable or owned reference. The Lease then frees
its successfully acquired FV copy and returns the original refusal. Root must
also successfully close the actual Reader and free its separate FV copy before
reporting zero side effects. Missing/stale/malformed evidence remains retained;
the callback does not grant memory, clock or display permission. FV retrieval
warnings/partial outputs, any uncertain guard, later text failures, native
mutations, EBS and failed/warning frees keep their previous retention rules.

`BorrowHeld`, `ValidateHeld` and `ReturnHeld` provide one revocable read-only
borrow of the existing clock reference. Every successful call performs fresh
APP/alive/image checks, two matching typed-counter snapshots, an actual
`IsClockEnabled` output and an independent exact GCC guarded readback, followed
by another fresh image check. Ordinary total and client references must each
remain at least1; the owner ledger must remain exactly1 and object identity,
flags and counter class must match the acquired owner. Other legitimate
references may change since startup or between borrow and return. No call
adds a reference, disables a clock or invokes IsOn.

The token is monotonic across all Lease instances for the loaded driver
lifetime, reserved before callbacks and never reused or wrapped. Only one
borrow per Lease may be active. Release rejects an active borrow. Return
validates again itself before revoking its token; a prior Validate cannot
substitute for that final check. EBS and retained failures revoke the token
and prohibit release retries. Invalid/stale tokens and aliased outputs are
rejected without granting a proof. Native queries and the API's own short GCC
guard must finish before the observer's guarded session begins; Return is
called only after the observer's exact Guard End.

The typed held proof covers one `gcc_disp_ahb` reference and its enable state.
It does not prove MMCX, MM/MX rails, a complete DISPCC bus-power lease, any DPU
domain, or permission to map/read arbitrary registers. A controller observer
must separately satisfy its actual power and fixed-resource gates; no missing
gate may be replaced by the presence of a held-clock token.

The actual pinned PE fixture now uses its real static BSP283a0/module-array28308,
GCC module28678/149 clocks/array32418/index51, and static parent37528. Only native
ARM calls and protected-read boundaries are substituted.88 ASAN/UBSAN fork
cases cover identity/hash/relocations, first client entry creation, two-snapshot
coherence, legal other-reference changes, skip/no-op behavior, failed native
calls, wrong/missing outputs, saturation/cycles/wrap, guard cleanup, EBS during
RaiseTPL/Enable/IsEnabled, uncertain registration/release, actual sentinel
outputs, hardware gated idle88000003 with IsOn FALSE, inherited enabled refs
and exact-once release. The first-read evidence cases cover a clean map refusal,
absent/stale/mismatched callbacks, every guard-owner uncertainty, warning/failed
cleanup, service loss and the prohibition on cleaning later read failures.
Borrow cases additionally cover active-release rejection, before/after fresh
validation, legitimate other-reference changes, cross-instance and returned
token rejection, output aliasing, reentry, EBS revocation, counter drift, missing
or false enabled outputs, changed identities/flags and uncertain GCC cleanup.
Strict AArch64 syntax passes. Host callbacks are fixtures, not hardware proof;
Root must bind the real reader/manager and run the next unique image to verify
AHB retention and subsequent display observations.
