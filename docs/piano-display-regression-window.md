# Physical display regression: earliest evidence window

The first explicit empty physical display report after the known-visible GUI is
between test24 and test25. Test25 was grey; the first explicit white product
screen report is test93. The grey and white symptoms are not yet proved to have
the same cause, and the later white product splash is not established as the
initial cause.

Test24's archived image SHA is
`32e0b363887340ce72f38f90969b04beb9874ac0840baabde31e73fa4dc212e2`.
The operator confirmed volume navigation, two power-release Enter events and
visible SimpleInit tools. The archived FD's extracted FV lacks ClockDxe and
HALIOMMU GUIDs and the foundation-start text; this is from the sealed artifact,
not today's overwritten profile configuration.

Test25 image SHA is
`cc811309f74d4422592bfe3ac211ec3e0b45622270973ee6883927a4d6427429`.
Its recorded operator observation is a grey screen. Its retained log explicitly
starts the native foundation, including HALIOMMU/NPA/VCS/Clock/QUP/SPI, before
GOP roundtrip and GUI. GOP_QUERY succeeds, invalid mode is rejected and
GOP_BLT_ROUNDTRIP matches, yet this does not prove physical scanout. The log
also reaches GUI driver initialization and font72/64 selection. This is the
first known observation of software display access succeeding while the
operator sees an empty physical display.

Test92's old-logo stall is a distinct failure: ProductCore payload rejection
occurs before its real USB/UFS/UI startup. Test93 explicitly reports a white
product screen. Test94 reports TianoCore first, then white. Test95 provides a
complete CRC-verified framebuffer menu, while the operator confirms the panel
still white. Thus the new white splash is not established as the first cause.

Test96's cached GOP inventory observes the same two interfaces, framebuffer
FC800000,3200x2136 and stride3200 before/after foundation/UFS/USB. SimpleInit
selects the same physical preferred interface. Some long diagnostic lines were
truncated by the256byte DebugLib formatter; the remaining recorded pointers
and base/stride are consistent, but missing fields must not be invented.
The emitter is being split into bounded lines for the next build.

Test97 replays the exact sealed test24 image, with the same SHA listed above,
on the current tablet through fastboot boot. The operator explicitly confirmed
"正常进入simpleinit而且可以操作工具箱": the physical SimpleInit display and
toolbox operation both still work. The baseline excludes the test25 foundation
and touch configuration workflow; the test97 replay does not flash partitions
and does not run routine partition hash checks. The retained test97 log confirms
PIANO_MENU_SELECT/PIANO_MENU_EXECUTE simple-init, subsequent tool input and
PIANO_STAGE0_RETURN_TO_ANDROID. Live sys.boot_completed=1 confirms Android
recovery. The complete evidence is recorded in
private/analysis/stage0-test-97.json and private/analysis/ramlog-test-97/uefi.txt.

This strengthens the version-regression window and establishes a current
known-visible control. It does not isolate one variable: test24 differs from the
current product in GUI/runtime code and multiple hardware features. Test25 also
adds touch RAM configuration before foundation and SPI Open before SimpleInit.
The version transition coincides with these additions; the exact step that
changes physical display behavior is not yet proved. Test98 completed a separate
temporary read-only guard session before and after every actual native StartImage
and four outer phases: all 30 snapshots are coherent and cleaned up. The measured
MDSS/global/SID800-mask2/CB2 fields are identical in every phase, while the
operator still sees a blank white/grey screen. Thus this experiment does not
support a native-start change in these SMMU fields. CPU framebuffer translation,
cache metadata, display clock and DPU scanout remain separate unmeasured paths.
See piano-product-test98.md for exact observations and recovery.

Test99 added no-target-load CPU/GCD/AT framebuffer observations at three fixed
pages, twice per phase. All30 CPU groups and all30 MDSS groups are complete and
unchanged. The measured CPU PAR44/NormalNC versus static/GCD WT discrepancy is
already present before foundation; it does not identify a native pre/post
trigger. The physical screen remains blank. DPU/DSI/display-clock state still
requires a separate trusted mapping/observation contract. See
piano-product-test99.md; the standard 256KiB fastboot log and clean reboot were
also verified on hardware.

Test100 verified mapped GCC reads but native Clock initialization stopped at
a separate CESTA CRMC write, RVA C234/FAR AF27D6C, with a missing CPU translation.
Test101 mapped the exact native CESTA role and Clock StartImage returned. Its
30-phase clock trace identifies GCC display AHB bit0 clearing at post:ClockDxe
(88000003 to88000002), persisting through UFS/USB. Framebuffer and MDSS fields
remain unchanged. This provides a specific hardware causal candidate for the
blank display; acquiring and retaining a real product-owned clock reference
still needs physical validation. See piano-product-test100.md and
piano-product-test101.md for the distinct fault, corrected mapping and phase
evidence. No direct clock bit patch or old-image success alone proves a repair.

Test103 subsequently measures a real product AHB reference0→1, restores
88000003, retains it through USB/UI, and retires it1→0 before normal Android
recovery. The physical panel result remains pending; this is not a confirmed
white-screen fix. See piano-product-test103.md.

The native CESTA temporary-clock list also contains non-GDSC AHB and RSCC
AHB/vsync. Linux explicitly keeps the RSCC branches at DISPCC offsets C00C
and C008 enabled in its probe (the pinned local driver lines1932–1933;
[official probe](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/dispcc-sm8750.c)).
These are additional specific candidates for a handoff regression. Their live
values and native references have not yet been measured. Controller access
requires an effective MMCX lifetime and interface clock; neither the successful
GCC reference nor a software framebuffer screenshot supplies that evidence.

Sessions are fully closed before native code runs. No driver
is disabled for that product observation, and no display/clock/MMU/
SMMU register is written to obtain it. Test97 is an explicitly requested replay
of a historical diagnostic image, not a replacement product profile or a change
to the single product feature set.
