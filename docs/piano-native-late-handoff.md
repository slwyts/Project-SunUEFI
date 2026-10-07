# Native APP handoff at ExitBootServices

The new `uefi/components/product-handoff` unit implements a native EBS client and
a Root authority backed by the actual `PianoProductOwners` manager. It is not a
planner or a clean/no-DMA report supplied by UI. `prepare_product_handoff.py`
generates exact changes to the three native Core sources and stages library
metadata; its default operation is review-only and currently no real Mu file
was changed by this unit.

Native sequence:

1. `CorePianoCurrentImageInfo` reads Mu's actual current `StartImage` frame,
   signature, Started state, handle and EFI application type. It returns the
   actual loaded-image object, rather than trusting the caller's handle.
2. `CorePianoCurrentMemoryMapKey` reads the real Page.c key under the native
   memory lock without allocating or terminating the map.
3. `CoreExitBootServices` calls the product library before its first
   BeforeExitBootServices notification, timer shutdown or memory termination.
   Wrong APP TPL, stale key and an unregistered/foreign image are refused
   without stopping owners or notifying Before. A terminated map is refused.
4. The real library locates Root's unique provider on this first call. Root
   validates the previously armed exact LoadedImage identity and full-memory
   epoch through its real platform callbacks. It calls the existing actual
   `PianoProductOwnersRetire` once, verifies and freezes its clean owner ledger,
   then checks final memory against the same epoch/DRAM identity. Warning,
   partial cleanup, unknown owner or changed epoch after retirement invokes
   Root's fail-stop/recovery path and cannot return to the loader.
5. The original Core sequence continues unchanged: Before notification once,
   timer0, `CoreTerminateMemoryMap`, Exit notification and BS table removal.
   Retirement can change MapKey, so the original first termination may return
   INVALID_PARAMETER. The normal EFI caller obtains a fresh map and retries.
6. Retry checks cached protocol function identity, same actual running image
   and the provider's real frozen clean ledger using CPU reads only. It does
   not LocateProtocol, HandleProtocol, allocate, collect memory or call HAL;
   retirement remains exactly once. A mutated ledger/provider fails closed.

Root provider lifecycle is explicit: Initialize installs one unarmed provider
and refuses another instance; Arm validates an actual loaded child and Root's
memory/owner state before StartImage; ordinary returned children may Disarm
without retiring controllers; Shutdown uninstalls only the unarmed, unretired
provider. Uninstall warning/error retains its code/context and fail-stops.
Completed/unknown transitions cannot be disarmed to resume old controllers.
The provider and owner backing objects must have persistent driver lifetime.

## Exact integration still required

After the current immutable build finishes, Root should:

- Apply/verify `prepare_product_handoff.prepare(root, apply=True)` before
  firmware/SimpleInit identity capture. This changes shared Core/library input
  hashes, so source manifests and the SimpleInit build must be refreshed.
- Bind `PianoProductExitLib` only for product `DXE_CORE`; default diagnostic
  binding is Null and preserves its existing EBS behavior.
- Stage the provider with `stage_provider(root, app)` after the existing shared
  `uefi-app` header mirror, add `LateHandoff/PianoLateHandoff.c` to ProductCore
  INF, and include the new MdePkg protocol/library headers and exact hashes in
  build integrity. The provider uses existing LoadedImage/BS/Base/Hob-free
  interfaces and the existing Root owner implementation.
- Initialize an actual persistent provider with the live Root owner object,
  real full-memory callbacks and runtime-safe recovery. Do not supply a fake
  success memory validator. It can remain unarmed while full DDR is unavailable.
- Arm the loaded OS image after sources/options/LoadFile2 are ready. Change the
  Linux/generic image launch path to StartImage while owners remain running;
  remove its pre-Start hardware retirement. Do not replace the old retire proof
  with a fake clean result from an Arm call. Native EBS performs actual retirement.
- On a normal pre-EBS return, Disarm before ordinary loader cleanup. If native
  retirement began, do not restart old USB/UFS or return to their old UI epoch.

The shared Linux session and generic launcher now expose the explicit
`PianoHandoffNativeLate` mode with real NativeLateArm/NativeLateDisarm callbacks.
Root product configuration must select it; default legacy mode exists only to
preserve diagnostic behavior. Native mode never calls pre-Start retirement or
invents a clean report. Ordinary pre-EBS return disarms before any image/source
cleanup; Before-only/Exit return or unknown Arm/Disarm result retains/fail-stops.
Root integration, full DDR/OS backend and device verification remain required.

## Evidence

`tests/unit/test_late_handoff.py` compiles actual transformed CoreExitBootServices,
the real Page.c CoreTerminateMemoryMap, both exact native getters, the actual
client/provider and actual PianoProductOwnersRetire. Eighteen host cases cover
success/stale-key retry, wrong image/TPL/Started identity, unarmed/missing
provider, full-memory and actual validator refusals, USB warning/proof failure,
final epoch drift, retry ledger mutation, duplicate provider and uninstall error.

The same test extracts the actual current Linux LoadFile2 initrd copy and uses
the actual unified CPU helper before the native EBS call:131,073 bytes copy in
four slices while the real owner report has USB active, policy installed and
retirement count zero. Native entry then executes the real manager cleanup
before Before notification. First key10 becomes11 through the manager's host
BS-close boundary; the original Core returns INVALID_PARAMETER; retry completes
without any new lookup, HAL, memory callback or owner retirement. Timer/IRQ/BS
termination behavior runs from the actual Core function, not a parallel model.

Separate checks compile actual AArch64 sources and test temporary preparation
idempotence/drift rejection. SHA of all three current real Mu sources remains
unchanged. Hardware, full-memory and transport boundaries are host fixtures;
no device boot/MMIO/write occurred, and no whole-lifecycle fastboot hardware
success is claimed.

Full caller tests additionally run the whole actual GenericLaunch in8 cases and
the whole actual LinuxEfiSession with pinned libfdt in8 cases, linked to the real
provider/client/native Core/owner manager. In the Linux StartImage callback the
real registered LoadFile2 copies67,108,865 bytes in1,026 active-USB service
slices before retirement. Native Core then retires once, handles the changed
MapKey and retries successfully; parent return after Before/Exit uses only its
CPU fence and never disarms/unloads/frees. Other cases prove ordinary return,
failed LoadImage/Arm, missing callbacks, real platform NOT_READY (zero Starts),
warning Arm/Disarm quarantine and proper image/source cleanup. Host injected
EFI/hardware boundaries are explicit; these are software call-chain results.
