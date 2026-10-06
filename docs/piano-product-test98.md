# Product test98: unchanged display SMMU through real driver startup

Unique product build `b4db1fe3-1a8a-404b-b59b-6ada974dd578`, image SHA256
`8859e856c4ac649db1cf5bbcb2d1f9f73bc5a04428405f6b0c031eef08a6ab53`, was
sent through fastboot boot. The operator confirmed the physical screen was
still blank white/grey. No flash, persistent media write or routine partition
hash pass occurred. The earlier test97 replay of sealed test24 independently
confirmed a visible, operable SimpleInit toolbox on this same tablet.

The test98 build isolates display observation changes from pending DDR producer
changes. An initial build contained a concurrently prepared inventory source;
its freshness check refused packaging. The four pending producer sources were
preserved, the original inventory restored, and prepare/build/package rerun.
The successful image's archived bytes, manifest and build record identify the
actual isolated candidate. The 374-method host suite passed; two additional
actual NativeProbe callback-order methods passed before device boot.

Standard fastboot version, oem ramlog, size/CRC queries and get_staged all
succeeded. The complete 65536-byte log has CRC32 `8C16F310`. The replay contains
all 30 named MDSS snapshots: before foundation, pre/post each of 13 actually
started native drivers, after foundation, after UFS and after USB. Every
snapshot reports exact Begin/End success, two coherent captures, 548 guarded
reads, three validated pages, zero recovered faults and no retained handlers.
The total is 16440 actual protected register loads, not host fixtures.

All emitted global, route and fixed CB2 fields are identical in every phase:

| Field | Observed value |
| --- | --- |
| SMR / S2CR | `80020800 / 00000002` |
| SCTLR / CBAR / CBA2R | `000000E0 / 0001F000 / 00000001` |
| TTBR0 / TTBR1 | `0 / 0` |
| TCR / TCR2 / MAIR | `0 / 00000060 / 0:0` |
| FSR / FAR / FSYNR | `00000400 / 466B62DEB7F2 / 007D0829` |

These are raw observations. Disabled context translation and zero TTBRs are not
by themselves proof of an error; an unchanged FSR/FAR is not evidence that a
new display fault occurred. The probe did not clear fault state, change a route,
load a page table, remap memory or alter clocks. It observes this fixed context,
not the DPU, panel, display clock or complete framebuffer CPU mapping.

The now-complete GOP lines also preserve both physical and virtual ConOut
interfaces, physical preferred selection, framebuffer `FC800000`, byte span
`1A13000`, mode `3200x2136`, stride3200 and format1 across all four outer phases.
This rules out a change in these measured fields during the observed sequence;
it does not prove physical scanout or isolate the current white-screen cause.

Standard product fastboot reboot completed successfully. The retained UFS
shutdown record reports clean1, stopped queues/IRQ, three DMA buffers freed,
domain closed, seven protocols removed and clocks released. It explicitly
does not claim other-owner quiescence. Live Android sys.boot_completed=1 was
checked after recovery.

Exact data and the independent field comparison are in
`private/analysis/usb-live-test98/ramlog.bin`, `ramlog.txt`,
`display-comparison.json`, `reboot-command.json` and the retained
`private/analysis/ramlog-test-98`. The next observation needs to distinguish
CPU framebuffer translation/cache metadata from display clock/scanout state;
the unchanged SMMU snapshot is not permission to repair arbitrary registers.
