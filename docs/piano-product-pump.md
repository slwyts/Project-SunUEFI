# PianoUEFI product service pump

The product has one runtime protocol instance, owned by the resident product
supervisor. SimpleInit, Setup, Shell and the root boot-selection loop use the
same USB service. Entering an application does not construct another DWC3
controller or substitute another feature set. Diagnostic image profiles are
development inputs, not separate product distributions.

`bootprofiles/uefi-app/Protocol/PianoProductRuntime.h` is the canonical ABI:
revision 1, one GUID, five methods (`Pump`, `BootServicesAlive`, `RequestAction`,
`GetPendingAction`, `AckAction`). Consumers never dispatch an EFI image from a
timer, key notification, or `WaitForEvent` callback. Notifications only latch
work. The parent supervisor alone acknowledges action sequences and dispatches
applications after the active application returns.

## Actual firmware and GUI hooks

The Mu `CoreWaitForEvent` loop calls `PianoProductPumpApplication` only when the
actual `gEfiCurrentTpl` is `TPL_APPLICATION`. Mu's existing support for waits at
other TPLs remains, but those waits do not run hardware work. This common hook
services ordinary Shell and Setup event waits. A latched `RETURN_CORE` action
returns `EFI_ABORTED` from that wait so product Shell/Setup can leave their own
input loops and run normal cleanup. The wait never acknowledges the action or
dispatches a nested application. It is cooperative scheduling:
an arbitrary third-party application that never waits or yields cannot be
preempted into a blocking driver call by this hook. `BudgetUs` is a bounded
admission/work slice, not a mechanism that interrupts a blocking vendor HAL.

SimpleInit's real `gui_main` loop calls the GUI wrapper before taking
`gui_lock`. With `PIANO_PRODUCT_GUI_PUMP=1`, the wrapper pumps the same protocol,
then reads the pending action. A pending action invokes the existing
`gui_run_and_exit(NULL)` mechanism. It does not acknowledge the action, execute
`StartImage`, alter `ReadKeyStroke` function pointers, or manufacture keyboard
events. The next parent supervisor iteration consumes the action and its
sequence. F12 routing remains in the supervisor's real key-notify enrollment.

The protocol client creates its ExitBootServices fence lazily during its first
application-TPL call. A library constructor cannot create that event because
the DXE Core constructs libraries before Event Services are initialized. It
refuses reentry, checks the actual calling TPL and refuses warning statuses as
success. An EBS notification clears the cached provider immediately; subsequent
calls never dereference it or invoke Boot Services. The parent must keep the
provider installed and resident for every live consumer. An application unload
must close the consumer's callback event successfully; an uncertain close or a
lost BS table with a live callback stops execution instead of leaving a callback
into unloaded application code.

## Reproducible source installation and builds

The ignored `upstream/` checkout is not the canonical implementation. New client
and wrapper files live under `bootprofiles/product-pump/`; the protocol lives
under `bootprofiles/uefi-app/Protocol/`. Run:

```sh
python3 tools/prepare_product_pump.py apply
python3 tools/prepare_product_pump.py verify --manifest build/product-pump-hooks.json
bash tools/build_simpleinit.sh --product-gui-pump
```

The installer checks all three pinned Git commits, performs exact single-anchor
transformations, rejects modified or duplicated hooks, copies the canonical
sources and preserves the original newline convention. It never contacts a
device. `verify` hashes the actual Core Event source, package/INF/DSC entries,
protocol header, library sources and GUI sources used by the build. A product
firmware builder must invoke installation before build freshness capture,
fingerprint the returned manifest, and bind the real library explicitly for
`DXE_CORE`, firmware applications and drivers. The default MdeLibs/Silicium
binding is the Null library, which has no Boot Services or provider dependency.

`build_simpleinit.sh --product-gui-pump` uses
`build/simpleinit-product-edk2/`, the real library and GUI macro, and exports
`artifacts/simpleinit/product/SimpleInit.efi` with source and hook manifests.
`--output-dir /absolute/directory` chooses another export directory. The default
diagnostic build still exports `artifacts/simpleinit/SimpleInit.efi`; product
builds do not overwrite that validated diagnostic application. Hook manifests
are verified before and after compilation and must match byte for byte.
The exporter clears `build-ok.json` before work, then seals it only after the
actual report, GUI makefile and application linker list prove the intended
library/macro selection. `tools/simpleinit_build_identity.py verify` rechecks
the application hash, current source hooks, DSC and actual build metadata. A
failed build cannot retain an old success marker.

Product functionality is enabled by the product DSC and runtime supervisor,
not by calling the read-only diagnostic test entry points. In particular, these
hooks alone do not prove USB bulk service, F12, reboot, image boot, or storage
access works while each physical UI is active. Those combinations still need
device acceptance tests against the single product image.

## Verification scope

`python3 -m unittest discover -s tests -p test_product_pump.py -v` compiles the
actual client and extracts the actual `CoreWaitForEvent`, `gui_main`, and
`gui_run_and_exit` source bodies into a host harness. With GUI mode both disabled
and enabled, 30 fork cases cover application TPL, nonreentry, EBS/cache refusal,
warning statuses, GUI locking/cooperative exit and event destruction. Additional
tests apply the canonical installer to clean pinned source blobs, verify
idempotence and refuse modified/duplicated hooks before any mutation. The Null
library test confirms no unresolved BS/provider dependency.

A complete product SimpleInit AARCH64 EDK2 build also verifies that the actual
build report and GUI makefile select `PianoProductPumpLib.inf` and
`-DPIANO_PRODUCT_GUI_PUMP=1`. This proves build wiring; no physical UI or USB
acceptance is claimed by the host tests.
