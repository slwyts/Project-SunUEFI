# Product GOP metadata observer

`PianoProductDisplayObserve(Phase, Alive)` captures up to eight named stages in
one resident ProductCore lifetime. Each stage enumerates at most sixteen GOP
handles. It records handle/interface addresses, Blt function address, mode/info
addresses, current/max mode, width/height, pixel format, scan-line stride and
framebuffer base/size. It marks the interface returned by `LocateProtocol` and
the interface on `gST->ConsoleOutHandle`; a ConSplitter `PixelBltOnly` interface
with framebuffer base zero is legitimate observation data.

This is metadata only. The observer invokes no GOP method, reads no framebuffer,
and performs no MMIO, AT, GCD, mapping, cache, mode or ownership operation. A
successful snapshot does not prove that the physical panel scans the recorded
framebuffer. The stored status fields describe exact protocol/cleanup outcomes,
not display readiness.

The caller supplies its resident CPU-only EBS/lifetime fence for every call.
Observation verifies the actual calling TPL with `RaiseTPL`/`RestoreTPL`; only
application TPL may enumerate. Every Boot Services call is preceded and followed
by the fence. If EBS occurs, there is no subsequent provider dereference, cleanup
or diagnostic emission. A warning lookup or ambiguous returned handle buffer is
retained and blocks further observation. An exact successful enumeration of more
than sixteen handles is rejected without iterating the array, then its known
buffer is freed. `FreePool` error/warning or EBS makes the buffer address opaque;
it is never retried, inspected or freed again. The parent must consult
`PianoProductDisplayRetained()` and preserve/fail-stop its own retained lifetime.

`PianoProductDisplayGetReport()` returns the fixed CPU report. Neither this
getter nor `PianoProductDisplayRetained()` calls services or dereferences a GOP.
`PianoProductDisplayReemit(Alive)` emits the stored snapshots to the existing RAM
debug sink and makes no Boot Services calls. It never accesses old handles,
interfaces, info pointers or an uncertain freed buffer. Reemission may therefore
be used immediately before the existing `oem ramlog` freeze to preserve the
initial observations despite intervening log wrap. The observer stores no caller
callback pointer and creates no event requiring an extra owner.

Root integration is intentionally outside this module: compile the C/header in
the single ProductCore INF, call observation before/after foundation/UFS/USB with
the actual Root EBS fence, and combine reemission with the existing BeforeRamlog
callback. SimpleInit's own selected GOP pointer should be logged where its real
`uefigop_init` selects it. No independent diagnostic image or feature profile is
required. This module alone does not provide that build or runtime wiring.

Run `python3 -m unittest discover -s tests -p test_product_display_observe.py -v`.
The actual C implementation passes 37 fork scenarios with ASAN/UBSAN and strict
AARCH64 compilation. They cover physical/ConSplitter metadata, preferred and
ConOut identities, count bounds, malformed modes, missing services, actual TPL,
warnings, uncertain output/free ownership, reentry, eight-stage capacity, EBS at
each service boundary and replay after provider/service pointers become
inaccessible. GOP fixture methods abort if invoked; no hardware is accessed.

The next display diagnostic should use these identities to distinguish a
framebuffer/source mismatch from scanout/IOMMU state. The separately observed
ROM MDSS SID `0x800`/mask `0x2` and historical CB2 route are not read or treated as
current state by this module.
