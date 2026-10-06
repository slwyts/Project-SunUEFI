# Framebuffer CPU mapping observation without target loads

`PianoFrameBufferMappingObserve(Phase, Alive)` stores at most 32 resident CPU
reports. It associates the current `LocateProtocol` GOP with the framebuffer
actually observed on piano: `FC800000/1A13000`, 3200x2136, stride3200, pixel format1.
A zero/mismatched base, size or mode is rejected before any AT/GCD sample.

The sample pages are fixed: first `FC800000`, middle `FD509000`, last `FE212000`.
They are observed twice, not expanded from a translation result or a control
field. The 27,340,800-byte framebuffer has 6,675 pages; these three samples do
not prove the other pages, any CPU/device ownership, DMA or physical scanout.
The log therefore always says `whole_range_ready=0`.

Each round saves raw EL/SCTLR/TCR/TTBR0/TTBR1/MAIR/DAIF before and after sampling.
Each page records exact GCD status, type, base/length, capabilities/attributes,
the raw RP bit, and raw PAR, derived physical-page/attribute/fault/identity fields.
Derived physical/attribute fields are meaningful only with exact successful AT
and no PAR fault; Identity additionally requires the fixed requested page.
Unattempted operations remain NotStarted. Warnings become an unsuccessful
snapshot, preserving raw status/data. WT/`BB` is legitimate raw observation data,
not an allocator or readiness permission. This module does not modify the
guarded-read library's UC/WB restrictions or retag framebuffer memory.

Observation requires actual application TPL and the caller's genuine CPU-only
lifetime fence before/after every BS or DS call. EBS stops before another CPU
sample, provider/service dereference or emission. No pool, event or handler is
created, so there is no ambiguous free or callback cleanup. Retained reports
denote service/lifetime loss; the parent must halt its resident lifecycle.
`GetReport()` is CPU-only. `Reemit(Alive)` never touches old tables/GOP pointers or
performs new AT/GCD work, allowing saved initial evidence to survive RAM log wrap.
The caller callback is never stored for later use.

The actual AArch64 AT sequence preserves DAIF and PAR. Its CPU table walk is
architectural translation, not a software framebuffer/PTE LDR. HA or HD could
allow hardware PTE updates, so both are refused. MMU-off, DS/LPA2, non-EL1,
unsupported address geometry, big-endian table interpretation, non-4K TG0,
EPD/TBI flags, invalid TTBR0 alignment/width or insufficient VA/PA span are also
refused while preserving raw CPU evidence. CPU state is checked again after
masking DAIF inside the AT primitive; this closes the GCD/interrupt boundary
before translation. Every path restores the caller's DAIF. No SCTLR/TCR/TTBR/
MAIR write, cache maintenance, MMU mapping, fault handler, MMIO access or target
data load occurs. This module observes six translations, not a protection or
ownership authority.

Root owns NativeProbe/Core/prepare wiring. The intended narrow comparison uses
the same before/after native phases as the MDSS observer. Test98's 30 complete
MDSS snapshots had identical route/CB2/global data, so changing CPU framebuffer
translation/cache attributes remain a separate hypothesis. Stable samples do
not demonstrate working clocks, DPU plane state or the physical panel. Native
HWIO mapping operations must not be called merely to obtain a diagnostic range;
DPU/DISPCC remain outside this module.

Run `python3 -m unittest discover -s tests -p test_framebuffer_mapping_observe.py -v`.
The real source passes 38 fork scenarios with ASAN/UBSAN and actual BasePrintLib
`AsciiVSPrint(256)`; full-width output stays below180 bytes with complete CRLF
(observed maximum154). Cases cover the exact six pages, source metadata rejection,
raw WT/device attributes, faults/nonidentity, repeated-state drift, warning status,
all ten BS/DS EBS boundaries with inaccessible table/provider pointers, non-APP
TPL, 32-phase capacity and CPU-only replay. HA/HD/DS/MMU-off/TG0/alignment and an
HA change after GCD each produce zero AT instructions in the boundary fixture.
A strict AARCH64 object is built and disassembled to verify actual system-register
and AT/PAR/DAIF instructions. No device or physical framebuffer/PTE is accessed.

The detailed replay contains both rounds and may exceed64 KiB across30 stages;
Root's256 KiB RAM-log snapshot must be used to preserve every earlier phase.
