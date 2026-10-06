# Staged non-GDSC AHB clock owner

`bootprofiles/display-rail/PianoDisplayNonGdscClock.c/.h` is a separate minimal
owner for `disp_cc_mdss_non_gdsc_ahb_clk`. It is not linked to a native tablet
callsite. The existing GCC Lease and Reader contracts are unchanged.

The owner directly consumes the actual GCC Lease, Clock Reader and Rail
Observer. A short GCC Borrow/Validate/Return transaction proves the registered
Clock image/protocol and completes all protected reads before each native
method. It reuses that identity instead of duplicating the PE pin logic. The
native getter must actually write `02010006`; the post-get selector snapshot
becomes the baseline, so the getter's native-owned24-byte client entry is
supported. The owner has a driver-lifetime EBS event and one attempt per state.

Acquire executes Enable once and requires ordinary node/client counts each+1,
alternate counts unchanged and two consistent selector snapshots. When the
node's previous ordinary count was zero, its parent-domain ordinary count must
increase by1; an already-used node leaves that domain count unchanged. Parent
rail mask8 and ordinary/non-suppressed flags are required. Actual IsEnabled
must write TRUE from an A5 sentinel; IsOn must write canonical0/1 and FALSE is
permitted. No rate, reset, GDSC, MDP or independent MX request is performed.

The real Rail Observer is called before/after acquire and before/after release.
Its latest typed records are copied into the clock report. MM object/status
coherence is required; missing MX is not a separate gate. The current/configured
corner, clock cached vote and NPA/VCS requests/applied states remain separate
actual observations. This owner does not add a physical voltage or whole-screen
readiness claim. Further bus admission can use the validated software driver
transaction and dependency evidence; it must distinguish that from panel output
acceptance and must not substitute a stale cached clock corner for a request.

Release uses a new short GCC transaction and fresh selected counters. It
executes Disable once, requires node/client ordinary each-1 and alternate
unchanged, and decrements the domain count only when the node's resulting
ordinary count is zero. Other legitimate references may change during the
held lifetime. Fresh rail observations, exact event close and GCC Return are
required before publishing Released. Warnings, missing outputs, inconsistent
counts, unknown observers, EBS or partial cleanup retain the child and any
active transaction; no Disable retry is permitted.

Successful acquire returns the single GCC borrow token, so existing periodic
observers remain usable while the child is held. Future product integration
must place GCC and this child under the same parent: stop the child first and
require its actual clean report before releasing GCC. The standalone GCC API
has not been changed to discover this staged child automatically.

25 UBSAN actual-source cases compile the new owner with the real GCC Lease,
Reader, Guard and captured Clock PE; AArch64 syntax also passes. Native ARM
methods and the separate Rail Observer's typed graph boundary are fixtures.
They cover zero/first and existing references, legal lifetime changes, node/
client/domain deltas, HWCG, missing/wrong outputs, unsupported masks/flags,
MM-graph failure, EBS, partial native failures and exact-once cleanup. The Rail
Observer has its own actual pin/Guard tests; an all-modules joint fixture and
product parent binding remain future work. No native device invocation, media
write, full product build or new product readiness claim is made by this unit.
