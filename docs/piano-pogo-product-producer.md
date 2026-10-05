# Pogo product input producer: lifecycle implemented, hardware NOT_READY

`bootprofiles/pogo-product/PianoPogoDxe.c/.h` provide one product-lifetime
producer for the official cover keyboard and touchpad. They are separate from
the frozen Root Keys driver and current firmware inputs. This work does not
enable the diagnostic PIO/transport gates or install a fake input device.

## Actual platform evidence and minimum missing hardware step

Anchored inspection of test89 and test91 console logs found no
`SUNUEFI_POGO_PROBE`, GENI or SE6 physical snapshot markers. Those runs exercised
joint UFS/USB and high-address AT metadata, not pogo. Existing source can capture
30 SE registers but is not a captured hardware result.

Android's actual HID descriptors show Bus6 Xiaomi Keyboard/Mouse/Touchpad/
Consumer devices15D9:00A3/00A2/00A1/00A4. The retained nanodev log reports
Connected1/Power1 in Android and also shows real screen-off/screen-on66 writes.
That proves the cover works under the Android driver; it does not prove an MCU
runtime session, clock or regulator survives ABL into UEFI. No UEFI wire frame
is contained in those HID descriptor files.

DT/native/reference source agree on SE6 A98000/4000, wrapper AC0000/2000,
slave4C, SDA/SCL56/57, ready97, reset188, status95, sleep3 and declared1MHz.
They do not establish current electrical polarity, rail/mux state or exclusive
owner. Native Open can load SE firmware and change clocks/GPIO, so it is not a
read-only qualification call.

Before a write1(register4C)/repeated-start/read68 transaction can be attempted
without firmware/power changes, Root still needs current protected Device-MMIO
and clock evidence, FW protocol3/full revision, expected byte packing/width/
FIFO depth, inactive M/S commands and IRQs, empty FIFOs, both IOS lines high,
DMA/GSI/IRQ routing disabled, real GPIO97 ready polarity and level, MCU runtime
session, exclusive SE ownership and actually bounded read/write/time callbacks.
The present evidence does not satisfy those conditions: **backend NOT_READY**.
The next physical step is the existing read-only PogoProbe/Capture, not native
Open or unverified power/firmware writes. Touchscreen NT36532 remains a separate
unready physical backend; this producer's AbsolutePointer is the cover touchpad.

## Producer binding and protocol behavior

The producer accepts immutable callbacks from a genuine Root platform backend:
current Ready, DataReady, monotonic NowUs, fixed bounded Read68 and checked Stop.
Root must not bind a fabricated frame source or declare readiness from parser
tests. There is no public Feed/injection API. Start calls real Ready first; any
non-success publishes zero input protocols and creates zero events.

After readiness, one handle publishes DevicePath and four actual standard
protocols: SimpleTextIn, SimpleTextInEx, SimplePointer, AbsolutePointer. It reuses
the existing real Piano report parser and adapter. The device path and handle
are independent from Root's physical buttons. Duplicate starts are refused.

The periodic timer only sets WorkPending; wait-event notify only signals
already queued data. Neither callback reads hardware. The shared product APP
pump calls `PianoPogoDxePump`, which rechecks readiness and data-ready, reads one
real68-byte report with a10ms absolute deadline, checks monotonic completion,
feeds the real parser, dispatches matching key notifications at TPL_CALLBACK,
then signals wait events. New keys are captured before wait-event consumers can
drain the queue. The pump refuses reentry and callbacks cannot stop it while
active. Unknown/late/short/error/warning reports are not delivered and are
retained. No DMA logic exists here.

Key notify registration is semantic and duplicate-aware. Tokens have monotonic
generations at the resident module level, so a stale unregister cannot remove a
later registration in the same slot or a new driver instance. Actual F12 travels through SimpleTextInEx key notification rather
than fake key injection or Root input-function pointer replacement.

Stop is APP-only: cancel/close polling, obtain checked quiet+clean backend stop,
disconnect consumers, exact uninstall of the same protocol tuple, close four
wait events and the EBS fence. All mutation warnings/errors retain state;
partial/unknown installation/event ownership cannot be silently cleaned up.
Failed Stop is not retried; successful Stop is idempotent. EBS notify is CPU-only
and marks services lost/retained, with no backend call, BS cleanup or release.
The producer object/backend and any uncertain callbacks must remain resident.

The existing transport scaffold has a64-read diagnostic budget and default-off
gate. It is not a permanent product backend; this lifecycle layer does not
reinitialize it, lift its budget or fabricate its readiness. A future genuine
backend must support sustained bounded operation with the same ownership and
cleanup rules. Typematic, physical LED/output support and complete gesture
behavior are not claimed by this lifecycle implementation.

## Verification and integration boundary

`python3 -m unittest discover -s tests -p test_pogo_dxe.py -v` runs the actual
producer plus actual parser/adapter under ASan/UBSan:23 fork cases cover
NOT_READY/no-publication, exact five-interface tuple, duplicate start/notify,
timer-only work, F12 callback TPL, wait-event signaling, stale tokens, relative
mouse states, short/error/late reports, event/install/signal/close warnings,
checked backend stop, uninstall failure, sticky retention and EBS/no further
BS/backend calls. ARM64 Wall/Wextra/Werror syntax also passes. Test backend
frames are synthetic host fixtures, never evidence of actual cover input.

Root future product integration needs this source/header, existing
PianoPogoInput/PianoPogoReport sources, UefiBootServicesTableLib/BaseMemoryLib,
the four input+DevicePath GUIDs and ExitBootServices group GUID. It must register
the actual started owner with the unified product manager and pump at APP.
Hardware unready must remain NOT_READY with no protocol publication. This work
does not modify Root Core/Keys/current transport or add a new product profile.
