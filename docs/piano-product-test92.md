# Product Test92: real cold SMEM and payload rejection

The RAM-only product image build `f0f3ef5c-5ab0-4b26-ba0a-f16a466a7f65`, SHA256
`18e1cb449b56005024df19d3a0cd59d90846ce88045287071a2829b201ef80cb`, was sent
through fastboot boot after the user confirmed presence and ability to recover.
No partitions were flashed. The tablet displayed TianoCore and did not enumerate
the product USB fastboot. The user forced Android recovery. All26 boot partition
SHA256 values match the original capture; Android completed boot on slotA.

Actual retained console:
`private/analysis/ramlog-test-92/product-uefi-session.txt`.

- Early SMEM major12 is readable:1,762 loads, no recovered abort, stable metadata
  and payload across two reads.
- RAM item402 is version3, address81D06AD0,2328 bytes, CRC32 7C271814.
- The stable cookie is81EFF350, inside the existing2MiB SMEM read window.
- The former v1/v2 parser rejected version3; this does not prove unavailable RAM.
- GOP reached3200x2136, stride3200, framebufferFC800000.
- ProductCore returned Security Violation before native foundation, UFS/USB
  service startup and CORE_READY. This is not a controller-retirement failure.

The original product assumed APPv1 began at FDT linux,initrd-start. Android's
[vendor-boot specification](https://source.android.com/docs/core/architecture/partitions/vendor-boot-partitions)
requires selected vendor ramdisks before the generic ramdisk, with bootconfig
handling separate. The published Qualcomm BootLinux implementation likewise
sets the combined initrd range and copies vendor data before generic data.
The captured Piano vendor ramdisk is33,271,900 bytes. A start-only APP parser is
therefore incompatible with that combined layout.

The fix searches only the fully bounded, known-mapped FDT initrd span. It retains
the exact compiled application byte count, SHA256 and ARM64 PE checks. Invalid
markers are ignored, two completely valid pinned applications are rejected,
and a valid application elsewhere in mapped RAM cannot authorize a load. New
actual-source tests cover vendor prefix, bootconfig-like tail, bogus vendor
marker, duplicate and outside-range candidates. New diagnostics expose exact
handoff bounds and matched offset, or the first header fields on rejection.
The fix has not yet been boot-tested. The next unique product will combine it
with strict native-backed RAMv3 parsing and the larger GOP boot splash.

Collected logs are evidence, not full-memory permission. Bank/preloaded/SIII
contents and current-owner placements still need verification before enabling
large DDR or a1GiB transport. The small logo is also not proof that our splash
was drawn: Test92 reports the old BootLogo2 path Not Found.
