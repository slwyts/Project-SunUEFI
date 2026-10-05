# Bounded product GOP framebuffer publishing

Test93's wrapped RAM log proves that SimpleInit reached its real `uefigop`
3200×2136 driver and received actual menu key events. The cold/early logo/Core
records were overwritten by later activity and are not reconstructed as success.
The configured `Display_Reserved` mapping is FC800000/2B00000,
`WRITE_THROUGH_XN`. No live PAR/MAIR result proves a different cache attribute.
Consequently a dirty-WB-cache explanation of the white display is a hypothesis,
not an accepted diagnosis or a reason to change the MMU.

Actual source nevertheless lacks an explicit publication operation:
`PianoGopDxe/SimpleFb.c` used only `FrameBufferBltLib`; that selected library has
no data-cache clean. SimpleInit's actual `uefi_gop.c:uefigop_flush` calls the same
GOP BufferToVideo method, then `lv_disp_flush_ready`, ignoring the returned
status. It performs no independent framebuffer cache maintenance. That common
adapter therefore is the smallest place to make successful CPU writes visible
without changing every client or the configured WT mapping.

The product adapter now captures the actual initialized base, active byte size,
pixel bytes, stride and dimensions. It validates destination rectangles by
subtraction and source rectangles for video reads/copies, rejects changed mode
address/size/stride, and validates initial multiplication/end-address bounds.
Zero width/height is rejected before division. No writes happen through an
overflowed rectangle. The adapter does not accept an arbitrary caller's mode
as a new framebuffer address.

Successful Fill/BufferToVideo operations publish only their destination rows
using `WriteBackDataCacheRange`. Whole-stride consecutive rows may form one
range. VideoToBltBuffer is a CPU read and does not clean. Invalid/failed writes
do not authorize maintenance; in a multirow copy, only rows actually completed
are published. The selected ARM library rounds to cache lines and performs its
final DSB. The real FC800000 allocation and native stride12800 are aligned;
tests also check aligned simulated line expansion stays inside their declared
allocation. No invalidate, full-cache flush, reserved-tail flush, DMA map,
ownership permission, MMU or memory-attribute modification is added.

The source audit found a separate deterministic library defect: the selected
VideoToVideo downward path advances source/destination by `Height*stride`, so
its first copy addresses a row beyond the requested rectangle. The adapter now
uses one initialized, width-bounded BLT-pixel scratch row. It calls the actual
VideoToBltBuffer and BufferToVideo primitives bottom-up for downward copy and
top-down otherwise. Reading a complete row before writing also handles
horizontal overlap. The ordinary V2V functionality is retained without calling
that defective branch or changing global upstream/profile code.

Only the first eight errors emit `SUNUEFI_GOP_BLT_ERROR` with operation,
source/destination, rectangle and exact status. There is no per-pixel or
success-event log flood and no claim that SimpleInit has displayed a successful
frame merely because it signaled `lv_disp_flush_ready`.

```
python3 -m unittest discover -s tests -p test_product_gop_blt.py -v
```

The actual product driver is linked with the actual selected
FrameBufferBltLib under ASan/UBSan. A valid downward operation through the
unmodified library demonstrably changes the allocated outside-framebuffer
canary; the adapter's downward/upward/horizontal overlap operations produce the
correct snapshot pixels and preserve both canaries. Tests inspect exact clean
addresses/lengths, read/error/no-clean behavior, log cap, zero dimensions,
tight-region and overflowing/end-address bounds, and changed mode address.
The real production C also passes strict ARM64 syntax. This is source behavior
validation, not confirmation that test93's white display has been fixed.

Changes are limited to the canonical product GOP C/INF and these independent
tests. CacheMaintenanceLib resolves to the existing ArmCacheMaintenanceLib in
the real platform DSC. Core, prepare, UI assets and global memory map are kept
outside this change.
