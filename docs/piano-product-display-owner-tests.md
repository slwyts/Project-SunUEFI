# Display owner production binding review and joint host test

The reviewed production coordinator is `PianoProductDisplayOwner.c/h`. It binds
the actual lease environment to the bounded native CPU reader and to a genuine
short GCC Guard callback. Every GCC callback ends its session before a native
Clock call. Startup uses real lease Baseline/Acquired ordinary+alternate counters
and their MatchingSnapshots, identity, owned reference and lifetime state.
Stop translates ReleaseBefore/Retired, CounterStatus, ReleaseReadbackStatus, exact
GCC EndStatus/4 loads/1 page, the closed lease event and the reader cleanup result
to the manager's typed report. It removes only the owned reference, permitting
other legitimate native references to remain.

KnownAbsent is derived only when no Enable was attempted, no Held/owned reference
or event/pinned copy remains, no retention/loss is reported, and early cleanup is
known. A partial Enable, opaque copy, failed close or lifetime loss does not become
absence. Core registers Display Started only for actual Held startup, otherwise
requires that explicit KnownNoSideEffects classification. The additional
after-display-lease checkpoint remains one phase, so the native/outer total is31.
Retained/lost display state suppresses further boot-log painting in Core.

Review found two output/lifetime boundary defects; Root fixed them in its owned
source before these tests froze. Startup/Stop now reject any output overlap with
the whole coordinator state and overflow before ZeroMem. Stop returns Aborted
and does not log after service loss. Tests execute both fixes rather than merely
searching source text.

Run `python3 -m unittest discover -s tests/unit -p test_product_display_owner.py -v`.
The host test compiles the **actual coordinator, actual Owners manager and actual
PianoGuardedRead implementation**, with explicit controlled Lease/Reader service
boundaries. Those boundaries do not validate native PE identity or model it as
hardware readiness; the separate actual Lease/Reader source suites do that.
GCC calls execute the actual guard's CPU/GCD/AT/handler/read/End paths, with the
fixture permitting only127004/127008 loads. The good joint case acquires, registers,
retires through Policy→USB→Proof→Bridge→UFS→Input→Display→Event, and checks raw
reference values10→9, exact owned1→0 and actual12 guarded loads in three separate
clean sessions. No fake Clean workflow bypasses the manager's validations.

Eighteen fork cases cover successful typed translation/handoff, true early
absence, opaque pinned/partial Enable refusal, release failure, reader-close
failure, release/close EBS with inaccessible service tables, duplicate release,
wrong context, whole-state output aliases, pointer overflow, before-start getter,
recoverable GCC read abort and uncertain guard event close. Real BasePrintLib
AsciiVSPrint256 checks complete short log lines, including the actual EFI-map
diagnostic fields. The additional reader evidence boundary verifies that the
production coordinator wires the exact `PianoDisplayClockReadFailureEvidence`
callback. Its controlled implementation returns `EFI_UNSUPPORTED`, leaves the
output sentinel untouched, and never grants `CleanSourceRefusal` or performs an
additional guarded read. Only the separate actual Reader/Lease pipeline can
prove a fresh first-read refusal and release both pinned FV copies.
ASAN/UBSAN and strict AARCH64
coordinator compilation pass. No firmware build, device or native hardware Clock
operation is executed by this test.
