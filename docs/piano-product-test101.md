# Product test101: CESTA fault corrected; Clock clears display AHB enable

Product build f3b8f77d-94fe-417d-840d-725b29817c85, image SHA256
de8a9b4833e5e560e66865d361b85ab1bf2aa546e72fb2deb97cb542d0fe5786,
adds only the precisely pinned native display CESTA resource to the53-row cold
table. Twelve targeted mapping/composition/staging methods passed after the
previous386-method suite. Actual build and source/output integrity passed.
The operator still sees a blank white/grey physical display.

RAM-only boot now completes native ClockDxe StartImage: post:ClockDxe and all
three later outer phases appear. The test100 CRMC translation-write fault no
longer stops startup. Product fastboot enumerates and standard version, oem
ramlog, size/CRC and get_staged succeed. The complete262144-byte log has
CRC32 9E8B9822 and uploaded in0.188 seconds.

The clock observations identify one exact phase change:

| Phase | GCC DISP_AHB127004 | GCC HF_AXI127008 |
| --- | --- | --- |
| before-foundation and every pre/post before Clock | 88000003 | 08200001 |
| pre:ClockDxe | 88000003 | 08200001 |
| post:ClockDxe | 88000002 | 08200001 |
| after-foundation / after-UFS / after-USB | 88000002 | 08200001 |

The actual AHB enable bit0 is cleared inside the native Clock StartImage
window. This is the first measured hardware transition that directly fits the
blank physical display. It is a causal candidate, not yet a repaired panel
acceptance result. HF_AXI is HALT_SKIP; bit31 is not used as running proof.

Native Clock PE analysis corroborates a mechanism: CESTA's four-bus list at
3E8A8 begins with gcc_disp_ahb_clk, followed by mdss_non_gdsc_ahb,
mdss_rscc_ahb and mdss_rscc_vsync. C420 acquires them, while C6D0 calls C4B4
to release in reverse order after initialization, leaving GCC display AHB last.
The implementation gates hardware when its total reference count reaches the
last reference. Live reference counts were not captured, so that last-reference
mechanism remains a source-supported explanation, not a measured counter fact.

All30 framebuffer CPU/GCD/PAR groups and all30 MDSS groups are unchanged. The
DISPCC mapping qualification stops being attempted after AHB off; its
NotStarted/skip fields are not evidence that mappings disappeared. No DISPCC
or DPU target register loads were performed.

The next functional repair must acquire a real product-owned display AHB
reference and keep it through UI/Shell/Setup, with exact typed retirement.
It needs actual pinned-image/ABI identity, written sentinel outputs, supported
typed IDs, no-op-mode/ref verification and independent MMIO readback. An ON bit
or EFI_SUCCESS alone cannot substitute for ownership. No unscoped direct bit
write, unconditional MDP/GDSC enable, frequency change or reset is warranted.

Standard fastboot reboot recovered Android, with boot_completed=1 checked and
retained clean UFS shutdown recorded. No flash, persistent media write or
routine partition hash pass occurred. The complete raw evidence and comparison
are in private/analysis/usb-live-test101 and private/analysis/ramlog-test-101.
