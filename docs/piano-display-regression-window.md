# Physical display regression: earliest evidence window

The first explicit display regression report is between test24 and test25,
not the later white product splash change.

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

The regression window is localized to the transition that added native hardware
initialization. Which exact native driver changes display behavior is not yet
proved. A separate temporary read-only guard session around each actual native
StartImage will compare the uniquely matched MDSS SID800/mask2 route and fixed
CB2 state. Sessions must be fully closed before native code runs. No driver
is disabled, no alternate feature profile is used, and no display/clock/MMU/
SMMU register is written to obtain the comparison.
