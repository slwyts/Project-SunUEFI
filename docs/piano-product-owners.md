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
boot policy, input, USB Host, GPI and pogo transport. Config must register every
bit exactly once as started or explicitly absent. The five core owners must be
started. An omitted/overlapping bit is not proof that a device is absent.
Started USB Host/GPI/pogo owners are currently unsupported by this retirement
contract; Initialize refuses them. This makes future hardware startup an
explicit extension of the manager rather than a hidden assumption of absence.

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
8. Close the manager's own EBS fence and verify the complete registered started
   mask was retired.

Only this clean result exposes `AllowedAction`. A fault may produce a clean
retirement record for diagnostics but never receives an OS/reset permit. Any
warning, unknown result, proof mismatch, failed input cleanup or EBS signal
clears AllowedAction and makes retention sticky. No retry reuses a consumed
proof or silently discards a token. If UFS retirement fails while CALLBACK is
held, the outer lease remains held and the caller must fail-stop.

## Validation

`python3 -m unittest discover -s tests -p test_product_owners.py -v` compiles the
actual manager under ASan/UBSan and checks 90 cases: registration, missing and
unsupported owners, cooperative UI admission, all status/report fields,
ordering, warning retention, TPL leases, stale runtime replacement, EBS events
partial boot tokens, and trusted UI reboot without fabricated USB ACK. Strict AArch64 compilation also passes.

These fixtures validate coordinator behavior. Root must bind the production
input adapter, construct and test the single product image and prove this full
sequence on hardware before claiming product handoff or complete input support.
