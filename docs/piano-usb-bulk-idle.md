# Product fastboot bulk transfers during UEFI waits

Test95 exposes a software scheduling cap in Setup: the actual native
CoreWaitForEvent pumps APP work, checks user events and then signals Idle.
ArmCpuDxe's Idle notification calls CpuSleep; the compiled timer period is10ms.
DWC3's existing shared DMA bounce buffer sends one4KiB block at a time. Setup's
17,825,792 bytes in45 seconds corresponds to10.34ms per4KiB, close to the
4096/10ms limit. SimpleInit's existing APP loop uploads the20,505,654-byte frame
in7.481 seconds. This explains a throughput regression; it does not establish
physical Setup display correctness or complete screenshot acceptance.

The same product service now records `BulkActive` after its bounded APP worker:
listening+bulk-live and queued reply frames, actual EP3 DMA pending, or download
receiving. Empty event queue does not mean idle. A permanently armed command EP2
is excluded. Stop/EBS clear the snapshot. The notification timer still only
copies raw events into its bounded queue; it performs no command, allocation,
Boot Services, storage or image launch.

The existing runtime five-method ABI is unchanged. Its provider installs an
independent `PianoProductIdle` read-only protocol on the same owned handle. The
protocol binds its revision and exact runtime instance; a CPU-only Read returns
the most recent completed APP sample sequence and active state. It has no MMIO
or ownership authority. No sample is valid before a successful service/status
observation, while pumping, after Stop/EBS, or in retained state. The sample
cannot wrap. Partial protocol registration retains the provider's events/code;
exact successful uninstall is required during Stop.

The real PumpLib reads a fresh protocol immediately after an APP pump, verifies
runtime/idle identity, revision and method stability, requires a strictly newer
sample and consumes the decision once. Missing/stale/unknown status does not
become an idle permission. Legacy Null returns the original idle behavior.
CoreWaitForEvent still pumps with its1000us budget, checks every requested event,
TPL, cooperative return and EBS. Only a fresh inactive decision signals Idle
at APP; active or unknown skips WFI and returns to the next bounded iteration.
Above APP the existing Mu wait behavior remains. No global timer period, DMA
size, USB owner instance, GOP or GUI timing helper was changed.

`prepare_product_pump.py` owns the exact source hook installation and verifies
all canonical sources. The new condition is idempotent and altered/duplicate
hooks are rejected. Actual product DxeCore/UiApp/SetupBrowser link the real
library; the metadata's default Null applies to other legacy profiles.

Host validation compiles actual sources under ASAN/UBSAN:64 client/wait/GUI
scenarios for both GUI flags,88 provider scenarios,40 persistent Device/
Controller lifecycle/navigation/log/active cases, and one joint pipeline. The
pipeline links actual Device+Controller, exact extracted native Wait and
provider Pump/ReadIdle bodies, and real PumpLib. It observes active EP3 with an
empty event queue, processes its completion at APP, restores one Idle signal
only after transfer completion and performs exact clean Stop. Hardware/event
wakeups are fixtures. Strict AArch64 device gate0/1 and provider/client syntax
remain checked. A new unique product build and real complete Setup upload are
required to measure the improvement; these source tests are not a device result.
