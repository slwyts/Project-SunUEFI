# Native product startup log

`PianoProductBootLog.c` draws seven fixed native GOP rows over the white product
splash: DISPLAY, PAYLOAD, SMEM, INPUT, UFS, USB and MENU. Orange stage markers,
dark gray monospaced text, non-success EFI status in hexadecimal and measured
elapsed milliseconds make actual startup results visible. Empty rows show `--`;
there is no percentage, simulated progress, extra delay or automatic countdown.

The renderer accepts a GOP and the caller's CPU-only BootServices-alive callback
through `PianoProductBootLogInitialize`. `PianoProductBootLogStage` takes the
actual stage name, EFI status and elapsed milliseconds. It neither probes a
backend nor changes the caller's startup decision. `EFI_SUCCESS` displays OK,
`EFI_NOT_STARTED` WAIT, `EFI_NOT_READY` NOT READY, `EFI_NOT_FOUND` and
`EFI_UNSUPPORTED` UNAVAILABLE, and every other warning or error FAILED. The
complete original EFI status is displayed for NOT READY, UNAVAILABLE and FAILED,
even when its summary label is shared. OK and WAIT show `--` in the code column.

The caller owns the ExitBootServices event and timing. Every GOP Blt checks its
alive callback before and after drawing. A Blt error stops future drawing; a
Blt warning becomes a display `EFI_DEVICE_ERROR`; loss of BootServices becomes
`EFI_ABORTED`. Those display results must never replace a hardware result. The
renderer allocates no memory, creates no event or timer, writes no framebuffer
pointer and does not reconfigure, hide or disable the standard text console.

The actual ProductCore registers its CPU-only exit fence before acquiring the
payload. It measures elapsed ticks from that entry in the counter's real
direction. BootServices loss after a display or event call halts before the
next startup action. If the payload is rejected and the application returns,
its exit event is closed before BDS can unload the callback code. A display
warning or error while BootServices remain alive only disables painting;
existing hardware results and owner decisions remain unchanged. The actual
Core callback test exercises normal return, event warning/error cleanup,
counter direction, exit during protocol lookup/rendering/event close, and
display-only failures under ASAN/UBSAN.

At 3200x2136, the panel is left-aligned at X192 and Y1281 through Y1630 with
35-pixel glyphs and 50-pixel row spacing. Its 6% left margin matches the BIOS
hints. It leaves the product logo, wordmark and subtitle above
and the setup/key hints below. The smallest supported mode is 480x320. Text and
columns scale together. The code column follows the actual elapsed text with a
small gap, while the full 64-bit elapsed value still fits without covering the
status column. After the menu is ready, the caller stops emitting startup
rows before launching SimpleInit so later updates cannot cover its user interface.

PAYLOAD represents acquisition and FDT validation; SMEM represents the bounded
observation; INPUT represents physical-key startup; UFS represents the actual
read-only DMA/controller startup; USB represents the resident service start;
MENU represents policy, owner-manager and late-provider initialization. SMEM OK
does not grant high-DDR memory ownership. UFS OK does not mean the original media
is writable or that a reserved writable volume has been provisioned. USB OK does
not prove a cable connection or host enumeration. MENU OK does not mean an OS
image has been booted. Native foundation currently has no aggregate status return
and is not assigned a fabricated OK row.

Run the actual renderer validation with:

```sh
python3 -m unittest discover -s tests -p test_product_boot_log.py -v
python3 -m unittest discover -s tests -p test_product_boot_log_lifecycle.py -v
```

The host C test decodes the rendered pixels for status labels, original status
hex and full elapsed values, checks seven independent rows and supported bounds,
injects GOP errors, warnings and ExitBootServices loss, and runs under ASAN/UBSAN.
The same source is also checked with the ARM64 Windows/UEFI target. The combined
preview in `build/previews/product-splash/product-bootlog-3200x2136.ppm` is produced by the actual splash and
boot-log C renderers. Its illustrative host statuses are not device acceptance.
