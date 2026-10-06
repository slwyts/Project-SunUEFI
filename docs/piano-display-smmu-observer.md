# MDSS SMMU observation in short protected DXE sessions

`PianoDisplaySmmuObserve(Phase, Alive)` records at most 32 named CPU snapshots in
the resident ProductCore. Each call opens and closes its own `PianoGuardedRead`
session. It never borrows the SMEM token and must not span native `StartImage`.
The parent may call it immediately before and after a driver starts, keeping all
normal product functions enabled while observing their actual effects.

The trusted whitelist is fixed inside the already mapped native SMMU window:

| Range | Permitted observations |
| --- | --- |
| `0x15000000 .. 0x15000DFF` | Global control/ID/fault and 127 SMR/S2CR rows |
| `0x15001000 .. 0x15001FFF` | GR1; only CB2 CBAR/CBA2R are actually loaded |
| `0x15082000 .. 0x1508206F` | Fixed CB2 SCTLR/TTBR/TCR/MAIR/FSR/FAR/FSYNR |

All ranges require GCD MemoryMappedIo with exactly UC, a full mapped page,
identity AT translation with device PAR attribute `00`, and the existing guard's
fresh EL1/SCTLR/TCR/TTBR/MAIR/protocol identity checks. This follows the pinned
`NS_DEVICE` platform mapping and `MAIR_ATTR_DEVICE_MEMORY=0`; a live mismatch is
reported as NotReady without remapping or weakening the attribute check. The
guard validates three pages before an actual register load. No DPU/DISPCC,
framebuffer, page-table target, MMIO write, cache change or MMU update is used.

Captured SM8750 IDs must exactly match `4C017E7F / 60000053 / 00005111`, establishing
127 routes, 83 banks, 4 KiB pages and CB offset `0x80000`. Extended-ID/global bypass
states are refused. The collector scans all routes for any valid alias of SID
`0x800` or `0x802`. Exactly one route must be `SMR=0x80020800`, TRANS, CB2. Missing,
duplicate, wider/different masks, bypass/fault or another CB produce Unknown/
NotReady without reading a different bank. Hardware register values cannot
expand the whitelist. The entire global/route/CB2 capture is repeated; a changed
second route is refused before a second CB load. Stable FSR faults or SCTLR.M=0
are retained as raw evidence, never cleared or interpreted as display/DMA ready.

Successful complete captures use 548 guarded LDR32 operations; the absolute
session budgets are 1,024 attempted loads and 100,000 microseconds, including
validation. Successful `Begin` always leads to an `End` attempt, even on a read
fault or semantic refusal. Only exact successful cleanup allows another session.
Foreign synchronous/SError handlers are never overwritten or unregistered.
Warnings, uncertain cleanup, guard retention or EBS are reported to the parent
as retained; the parent must halt instead of starting another driver. An exact
recoverable read abort records the guard fault and leaves an unsuccessful data
capture with clean cleanup. SError or an unknown fault halts inside the actual
guard with its own fatal register report; it does not return through a fictitious
successful observer cleanup.

`PianoDisplaySmmuGetReport()` and `PianoDisplaySmmuRetained()` are CPU-only getters.
`PianoDisplaySmmuReemit(Alive)` emits the stored fields, never reopens a session or
dereferences old protocols. Each record uses short lines below 180 bytes, checked
with the actual BasePrintLib `AsciiVSPrint(256)` in the host harness. Root can
combine replay with its BeforeRamlog callback to keep initial evidence despite
the normal log wrap. The parent must remain resident if the guard retained a
callback; the observer preserves that resident Alive callback for this case.

Root owns the single-product build and NativeProbe/Core wiring. This module does
not change those sources or introduce a diagnostic feature profile. A successful
register observation grants no ownership, memory allocation, translation lease,
DMA completion or physical display readiness.

Run `python3 -m unittest discover -s tests -p test_display_smmu_observe.py -v`.
The actual observer and actual guarded-read source run jointly in 32 fork cases
with ASAN/UBSAN, strict AARCH64 compilation and real capped ASCII formatting.
Cases cover the three exact ranges, CPU/GCD/AT refusals, unique route shape,
no bank loads for unsupported routes, both snapshot drift paths, clean short
sessions, foreign handlers, read-abort recovery, SError/unknown-FAR halt, EBS with
inaccessible service tables, cleanup warning retention, deadlines, 32-stage
capacity and CPU-only replay. No physical device or register is accessed by tests.

## Current native callback integration review

The generated single-product native table currently has 13 entries. The actual
`NativeProbe.c` marks each entry Attempted before FV/load work, so repeated DEPEX
passes cannot start one entry twice in a foundation invocation. An entry that
successfully loads receives `pre`, `StartImage`, `post`, then optional `UnloadImage`
after a start failure. FV/load failures and unresolved DEPEX/runtime dependencies
receive no observer callback. Core installs its resident callback only around
foundation, then clears it. Each short observer call must finish exact guard
cleanup before the corresponding native start proceeds, or Root halts on retained
state. Callback notification itself does not dispatch an application.

Core's four outer display checkpoints plus at most 26 native callbacks produce
30 MDSS snapshots, fitting the fixed 32-stage report. The earlier 17-driver
diagnostic table would require 38, so future table expansion must increase this
bounded capacity or explicitly handle capacity exhaustion; observations must not
be silently dropped. The current host integration test verifies the actual
generated table against the current Core checkpoint count.

`tests/test_native_observer.py` compiles a byte-identical temporary copy of the
actual NativeProbe source with generated FFS/BS fixture tables. Seven scenarios
exercise real callback order, post-before-unload, FV/load failure, DEPEX second
pass and misses, special runtime dependencies, default/cleared callbacks and the
native warning behavior. A second fixture uses all 13 current product names and
verifies 26 callbacks plus four outer phases fit 32. ASAN/UBSAN and strict AARCH64
compilation run against that source; no Root NativeProbe/Core source is edited.

Replayed snapshots retain their ordered phase names and raw MDSS/global/route/CB2
fields, observation coherence and Begin/End status. Comparing consecutive
`pre:<driver>` / `post:<driver>` records can locate the first measured register or
read-qualification change. A missing/ambiguous route or map refusal remains an
explicit unsuccessful snapshot. If all MDSS fields remain unchanged, that alone
does not prove clocks, scanout, framebuffer consumption or the physical display
are correct; this observer does not read those other masters/registers.
