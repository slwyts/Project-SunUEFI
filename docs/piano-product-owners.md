# Unified product owner retirement

`PianoProductOwners.c` is the shared APP manager between the product supervisor
and the existing real USB/UFS APIs. It does not start a parallel transport,
reset the tablet, load an image or start an image. Root starts the actual UFS,
readonly partition bridge, USB service, input and boot policy first, then passes
those registered states to this manager.

Use a zero-initialized driver-lifetime manager object. Its EBS event, input
context, runtime callback and any boot token must not outlive the containing
image/state. A retained result is terminal and may require fail-stop; the caller
must not unload the image or return stack storage while these references remain.

## Registration

The current known owner inventory includes USB Device, UFS, partition bridge,
boot policy, input, display clock, USB Host, GPI and pogo transport. Config must register every
bit exactly once as started or explicitly absent. The five core owners must be
started. An omitted/overlapping bit is not proof that a device is absent.
Started USB Host/GPI/pogo owners are currently unsupported by this retirement
contract; Initialize refuses them. This makes future hardware startup an
explicit extension of the manager rather than a hidden assumption of absence.

`PIANO_OWNER_DISPLAY=BIT8` extends the supported mask to CORE|DISPLAY; CORE stays
the original five mandatory owners and does not force a display reference to
exist. ALL includes display, so registration still covers every known owner.
Actual Held display startup requires a typed revision1 report, the matching
driver-lifetime context/ClockId/NativeBase, one owned reference, exact successful
startup, verified acquisition counters, and a bound StopDisplay callback. Unknown,
retained, lost, missing or overlapping states are refused before registration.
Explicit display absence is conservative: revision1 KnownNoSideEffects, no native
Enable attempt, no held/owned reference, no retention/loss, and NotStarted or a
known error status. Root must derive this classification from actual startup;
an absent bit alone never proves it. The implementation does not manufacture an
absence after an uncertain acquisition.

Initialize checks actual policy readiness, actual USB listening state, the
actual partition bridge and its Ready callback. The input participant uses a
typed Stop adapter that Root must bind to real input shutdown and fresh protocol
verification. An unbound adapter cannot permit handoff.

## Cooperative UI return and shutdown

The product boot policy already pumps the persistent USB service and latches
RETURN_CORE for a real USB action. `ObserveUsbAction` independently checks that
actual service state, obtains a fresh matching runtime protocol and requests the
same cooperative return. No owner is stopped from a UI timer, notify callback
or command completion. `Retire` refuses to begin while policy dispatch, pump or
an active child application remains.

Ordinary SimpleInit/Setup UI reboot uses `RequestUiReboot` after the actual
child returns. It accepts only the real BootPolicy `RequestedCoreAction` reboot
latch, an idle policy, a fresh matching runtime interface and the real USB
service still listening with no host action. The report records UI origin;
Retire rechecks that latch and state before cleanup. Host requests retain their
separate actual ACK/action checks. A caller bool, absent reason, USB status
fabrication or competing host request cannot substitute for the UI latch.

Retire performs the following real API sequence:

1. Stop policy key/event/runtime callbacks, checking fresh policy state.
2. Stop USB and validate every typed retirement field, nine DMA buffers and
   all eight released clocks. For boot, consume the actual DWC action even if
   Stop failed; partial/error tokens remain owned by the manager ledger.
3. Export the fresh one-shot retired USB SMMU proof.
4. Revoke bridge partition tokens and verify its getter now returns NULL.
5. Bind the actual USB retirement contract into UFS.
6. Hold an outer CALLBACK lease, Prepare and Shutdown UFS, then validate all
   typed fields, three DMA buffers, disconnect/protocol counts, clocks and zero
   doorbell/run/interrupt evidence.
7. Restore the manager's original APP lease only after exact UFS retirement,
   then invoke and verify the input Stop adapter.
8. When display was actually Held, recheck APP TPL after input shutdown, invoke
   the typed StopDisplay callback and validate owned reference retirement,
   native identity, reference deltas, GCC readback and the lease's closed event.
   This callback binds Root's real lease release; the manager itself makes no
   native Clock call. An absent display has NotStarted status and no callback.
9. Close the manager's own EBS fence and verify the complete registered started
   mask was retired.

Only this clean result exposes `AllowedAction`. A fault may produce a clean
retirement record for diagnostics but never receives an OS/reset permit. Any
warning, unknown result, proof mismatch, failed input cleanup or EBS signal
clears AllowedAction and makes retention sticky. No retry reuses a consumed
proof or silently discards a token. If UFS retirement fails while CALLBACK is
held, the outer lease remains held and the caller must fail-stop.

The display report must show Started/Returned/Clean, ReleaseAttempted, Released,
ExitClosed, no HeldAfter/retention/loss, and own ledger1→0. All Status/Release/
CounterStatus/GCC readback/end/cleanup results must be exact success. The4 loads/
1 page are specifically the GCC guard readback, not the total native-data read
count. Acquisition counters must match the registered startup identity. Native
`Total[2]` and `PerClient[2]` indices are ordinary[0] and alternate[1], not two
copies of one counter. `MatchingSnapshots==2` is represented by each typed
AcquireBefore/After and ReleaseBefore/After snapshots field and proves the
decoder's repeated snapshots agreed.

For each acquisition, ordinary total and client increment by one and alternate
counts remain unchanged. For this release, ordinary total and client decrement
by one from fresh release-before values; alternate counts remain unchanged across
the release itself. Other legitimate references, including alternate references,
may have changed since startup. The manager therefore does not require final
global/client counts to equal the startup baseline or zero. Baseline is diagnostic;
the exact owned ledger is what must retire. Root maps these fields from the real
lease report and its matching identities, without raw native addresses in the
manager. None of this authorizes a DPU power/clock bus load.

## Validation

`python3 -m unittest discover -s tests -p test_product_owners.py -v` compiles the
actual manager under ASan/UBSan and checks173 cases, preserving all100 original
workflows: registration, missing and
unsupported owners, cooperative UI admission, all status/report fields,
ordering, warning retention, TPL leases, stale runtime replacement, EBS events
partial boot tokens, and trusted UI reboot without fabricated USB ACK. Display
cases include actual Policy→USB→Proof→Bridge→UFS→Input→Display→Event order, absent/
held/missing/unknown registration, ordinary/alternate reference semantics,
legitimate unrelated-reference changes, reentry/duplicate/stale refusal, every
typed proof field, earlier-stage failure without display invocation, APP loss
and EBS with an inaccessible BS table, including the APP recheck after the
display callback and before closing the manager event. Strict AArch64 compilation also passes.

These fixtures validate coordinator behavior. Root must bind the production
input adapter, construct and test the single product image and prove this full
sequence on hardware before claiming product handoff or complete input support.

Core's planned one `after-display-lease` observation makes the outer checkpoint
count five. The native observer test now requires that actual checkpoint and
verifies26 native callbacks plus five outer phases fit32. It remains pending
until Root adds the Core call; the owner manager never introduces native observer
phases or dispatches an application itself.


## UI Continue and acknowledged host priority

The shared manager accepts only the actual Policy Continue/Reboot reason after
normal child cleanup, at APP with the same live runtime and USB listening. The
compatibility UI reboot entry remains. A changed UI reason, host transition or
warning prevents action permission. Continue currently uses the same clean cold
reset policy to return Android; it is not an OS-loader success result.

ResolveReturnedAction checks the real current USB ledger first. An acknowledged
host Continue/Reboot/Boot that arrives while a UI return is pending takes the
host path and its token/ACK proof, rather than using an older UI reason. Tests
execute the actual manager for these priority and retention cases. No caller
status, permission boolean or fabricated host ACK selects the path.
