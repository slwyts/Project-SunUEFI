# Bounded256KiB immutable USB console snapshot

`USB_DIAG_LOG_BYTES` in the actual PianoDwc3Device.c is now262,144 bytes. The
console's existing2MiB ring and capture algorithm are unchanged: save at most
the newest256KiB, require the observed header to remain stable, prefer the last
retained newline-delimited SUNUEFI_RAMLOG_BEGIN marker, and freeze size/CRC32/
generation. Flags bit0 still means the original log exceeded the bound; bit1
still means a retained session marker was found. A marker outside the retained
tail cannot be claimed as present. Later console writes do not change the saved
CPU snapshot or independently copied fastboot stage.

This is one bounded CPU allocation, not another DMA buffer. Existing standard
`get_staged`/upload uses the same4KiB mapped DMA bounce and DATA/raw/OKAY framing.
UFS fetch remains exactly65,536 bytes per command. The firmware version and all
vendor wire headers remain unchanged. Vendor pages are512 bytes, page headers24
bytes and Page is UINT16:256KiB needs512 pages, numbered0..511, so the field fits.
Length is still the exact24+payload length and the final page can be short.

`check_product_fastboot.py`, `check_fastboot_debug.py` and
`read_usb_diagnostic.py` now accept only the new262,144-byte log upper bound;
their partition fetch bound is unchanged. Logs can still omit older boot phases
because of ring wrapping or because the newest session marker follows them.
This update increases diagnostic retention without claiming a complete boot log,
hardware readiness or any full-DDR permission.

Actual source tests cover a131,113-byte unwrapped log, a full wrapped ring,
truncation above256KiB, absent/out-of-tail and multiple retained session markers,
CRC/generation metadata, immutable pages after later writes, page511 and rejected
page512/65535. The actual DWC3/fastboot fixture stages and uploads a full256KiB
snapshot through66 IN transfers (DATA,64 data chunks,OKAY), checks exact bytes and
asserts the DMA buffer is still4096 and fetch maximum still65536. The vendor
reader reconstructs all512 pages. Exact stock-fastboot checker source executes
offline against real files at65,537/262,144 bytes and rejects262,145 before
upload; no fastboot/device command is issued by these tests.

The complete existing fastboot actual-source suite and four related host tests
passed; Device/Controller AArch64 gate0/1 checks also passed. This is host/source
evidence. Root must build the next unique product before testing a larger log
export on the tablet.
