# Product display clock lifetime

Test101 measured GCC display AHB enable clearing inside native ClockDxe startup.
The product now binds one native gcc_disp_ahb reference after foundation and
before input/UFS/USB, retaining it across SimpleInit, Setup, Shell and selection.
The integration is pending physical validation; source or host success does not
mean the panel has recovered.

PianoProductDisplayOwner joins the real pinned Clock lease, the bounded typed
CPU reader and short protected GCC readback. The reader authenticates the actual
FV/live native code and exposes only its verified image/typed object graph.
Each Guard ends before a native API call. The lease proves ordinary total/client
reference increments and exact owned release using two independent decoded
snapshots; alternate counters are a distinct native class, not duplicate reads.

Hardware auto-gating is preserved. IsClockEnabled must actually return TRUE,
while successful canonical IsClockOn FALSE is recorded as a valid idle state.
Both outputs use sentinels. GCC bit0 and unchanged unrelated control bits are
read back independently. There is no direct register enable, frequency change,
reset or unconditional MDP/GDSC operation in this binding.

Core records after-display-lease once, bringing the observer budget to31/32.
The original four outer phases and every native pre/post checkpoint remain.
The lease's own GCC readbacks use independent short sessions and do not consume
extra observer entries. BeforeRamlog replays CPU-held status/counters alongside
the existing diagnostic snapshots.

Owner registration uses the actual returned lease report. A failed attempt
may remain absent only before any native Enable attempt, with no held refs,
retained event/copy/reader or lost services. Unknown/partial acquisition stops
execution. Core does not paint after retained observer/owner state.

On cooperative exit, the existing manager retires Policy, USB, proof/bridge,
UFS and input first, restoring APP before display release. The product callback
then invokes actual lease release, checks reference decrement and GCC readback,
closes the reader and translates those actual fields into the typed display
report. Only then may the manager close its lifetime event and permit the
requested deferred action. No global-clock-off claim is made: other legitimate
references may remain.

The callback rejects output aliases with its entire live state before clearing
output. Any warning, lost lifetime, cleanup uncertainty or unexpected counter
result remains retained. EBS paths perform no cleanup or display calls through
invalid services. This display owner grants no DPU domain lease, DDR allocation
permission or generalized OS admission.

Verification is split across actual Lease, Reader+Guard, Coordinator+Owners+
GCC Guard, and their full pipeline. The full pipeline is required before the
first device run because independent boundary fixtures can miss integration
failures. Standard product build identity includes every compiled implementation.
