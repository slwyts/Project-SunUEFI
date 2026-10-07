# Piano persistent USB Device service

`PianoDwc3Service` and `PianoUsbControllerService` replace the bounded foreground
USB experiment in the product. There is one USB0 owner and one fastboot command
state for the whole Boot Services lifetime. A UI application returning to the
boot policy does not call Stop. There is no 90-second service expiration.

## Execution contexts

- Controller Start runs at actual `TPL_APPLICATION`. It acquires the eight clocks
  and GDSC, installs the MMIO shutdown fence, opens the shared SMMU domain,
  verifies and retires the probe buffer, allocates nine device DMA buffers,
  arms EP0, then creates the periodic raw-event collector and EBS fence.
- The timer is at `TPL_CALLBACK` and only calls `PollBounded(16)`. This invalidates
  the event ring, copies up to 16 raw DWORDs into a 64-event CPU queue and
  acknowledges those bytes to DWC3. It does not parse commands, allocate,
  invoke Boot Services, read UFS, capture GOP, or load an application.
- The shared product Pump protocol calls `PumpApp` at actual `TPL_APPLICATION`.
  Its UI integration is responsible for pumping during SimpleInit, Setup,
  Shell and boot selection. Pump protects only queue bookkeeping at CALLBACK;
  it restores APP before processing events. A supplied monotonic CPU clock
  limits work between events. An individual synchronous controller command
  still has its own existing bounded timeout.
- An unexpected ExitBootServices notification only halts DWC3 and retains all
  owner memory. It does not attempt allocation, FreePages, event closing or
  HAL teardown from the notification. The product OS manager must explicitly
  complete Stop before intentionally starting an OS loader.

## UFS and ownership

The optional `Config.Storage` callback structure is copied on Start. Its backing
Context must remain alive until Stop. The service does not retain the caller's
structure pointer. When present, the same tested proxy used by the readonly
fetch experiment verifies UFS and USB contexts/routing before and after Ready,
Info and each physical read. Mapping and RUN checks also verify all peer
routing. A mismatch prevents further response DMA and quarantines the owner.

Reboot, continue and boot commands report an APP action only after the actual
IN completion emptied the response queue. The timer does not execute actions.
The product all-owner manager closes USB, consumes its one-shot fresh retired
SMMU proof, closes UFS and only then performs reboot or OS/image handoff.
Device Stop can move the downloaded image token after a verified boot ACK,
Halt and exactly nine DMA frees. That token is not execution evidence and must
remain in the manager's ownership ledger if later owner closure fails.

## Product UI navigation

The three exact stock OEM commands latch runtime actions 2 (Setup), 3 (Shell)
and 1 (SimpleInit) from the real APP worker. Each request locates the runtime
protocol afresh, checks its revision and five method identities against the
first live instance, then checks BootServicesAlive before and after the CPU-only
RequestAction. No nested application execution, device Stop/reset or synthetic
keyboard event occurs. The parent dispatches after normal child cleanup; USB
remains listening. Commands fail when no actual live backend is available, and
SERVICE-disabled legacy diagnostics reject them. An EBS fence prevents even a
failure reply from submitting new DMA.

## Full Stop

APP Stop cancels and closes the raw-event timer, halts DWC3, retires event/TRB
and payload DMA, restores the session state with readback, then revokes the
storage proxy/check. It closes the USB SMMU owner, checks UFS stability again,
uninstalls the shutdown fence, closes the EBS event, releases all eight clocks
and the GDSC. Warning/unknown results retain state and block retry rather than
being treated as successful release.

Only a fully clean latest Stop can export retirement evidence. Proof export
captures a fresh SMMU snapshot and invokes the actual owned-domain close ledger
validator; a successful export is one-shot and invalidated on the next owner
start. An invalid USB SMR alone never creates a proof.

## Validation and remaining device work

`python3 -m unittest discover -s tests/unit -p test_usb_service.py -v` compiles the
actual Device and Controller source together under ASan/UBSan. Twenty lifecycle and eleven navigation isolated
cases cover APP/TPL/reentry checks, timer-only queueing, overflow/malformed
rings, standard command and guarded fetch, more than 90 seconds of simulated
multi-UI lifetime, reconnect enumeration, actual ACK-before-action, RAM boot
handoff/cancellation/failed Take, EBS retention, and clock/event/DMA warnings.
The exporter boundary checks fresh capture and replay prevention; the actual
SMMU ledger/proof matching is covered separately by the owned-peer tests.
The navigation cases exercise exact `oem setup`, `oem shell` and
`oem simpleinit` requests, fresh runtime lookup, method/identity replacement,
missing/dead/wrong-revision runtime, warnings and EBS without response DMA.
Gate-zero APIs deny navigation, and gate-zero/gate-one AArch64 syntax also pass. The existing
USB fastboot and RAM-controller suites remain passing.

This is host-source validation. Persistent product USB still needs one real
image acceptance across SimpleInit, Setup, Shell and boot selection, reconnect,
concurrent UFS reads, clean shutdown and Android partition verification. It does
not prove physical UFS/USB retirement or establish the 1 GiB download pool.
