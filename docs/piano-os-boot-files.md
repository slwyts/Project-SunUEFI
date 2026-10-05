# Pinned stable and next boot-file bundles

`tools/export_os_boot_bundle.py` exports the exact independently built Image,
initramfs and explicitly selected complete DTB for stable or next. It checks
kernel/config/initramfs hashes, canonical source pins and the matching test71/73
raw ARM64 smoke evidence before copying. Existing outputs are not overwritten;
failed exports are marked incomplete. There is no device or partition writer.

Current exports:

| Name | Directory | Kernel + initrd + DTB bytes |
| --- | --- | ---: |
| Piano Stable | `artifacts/os-boot-bundles/stable-v1/` | 41043416 |
| Piano Next | `artifacts/os-boot-bundles/next-v1/` | 41493978 |

Each manifest supplies the fixed `/EFI/Piano/stable/` or `/EFI/Piano/next/`
file names, complete hashes and source commit. The raw evidence is copied with
its original `VERIFIED_RAW_ARM64_RAM_SMOKE_ONLY` scope. Entries are not enabled
as default boot targets: export is neither EFI-handoff nor distribution proof.
The 14MiB firmware-state proposal is too small for these kernels, so the final
installation needs an external or explicitly approved dedicated OS volume.

```sh
python3 tools/export_os_boot_bundle.py --profile stable \
  --dtb private/captures/2026-10-03-piano/live.dtb \
  --output artifacts/os-boot-bundles/NEW_STABLE_EXPORT
python3 tools/export_os_boot_bundle.py --profile next \
  --dtb private/captures/2026-10-03-piano/live.dtb \
  --output artifacts/os-boot-bundles/NEW_NEXT_EXPORT
```

The shared loader must acquire immutable files through actual SFS methods,
verify content and size, close file handles, then maintain explicit CPU source
loans throughout preparation. Linux initrd uses standard LoadFile2 and the full
DTB configuration table. Before actual StartImage, a trusted full-DDR and
all-owner OS transition must succeed. Current product lacks that verified memory
contract and has no active generic OS loader. The Continue menu therefore still
returns Android through managed retirement instead of claiming OS startup.

The canonical loader sources are `bootprofiles/os-boot/PianoBootFileSource.c`
and `PianoLinuxEfiSession.c`. They are shared implementation work awaiting the
real platform binding, not alternate feature-specific firmware images. Product
preparation now compiles all three shared OS modules through its actual INF and
includes their byte-identical sources and headers in build freshness checks.
Core has not called these APIs yet; unused functions may be removed by the
linker. This does not enable a menu entry or establish a usable OS backend.
The exporter preserves immutable provenance bytes and rejects source metadata
changes during copying; refusal tests cover changed payloads, overstated smoke
scope, output overwrites and provenance races. DTBs are limited to 2MiB.

The standard path follows the Linux [EFI stub documentation](https://www.kernel.org/doc/html/latest/admin-guide/efi-stub.html)
and [AArch64 boot requirements](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html).
The session treats a first ExitBootServices attempt as a lifecycle fence even
when the call fails: the stub may retry its memory map/exit sequence, while the
parent cannot return to ordinary device/UI operations. See the [UEFI Boot
Services specification](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html).

Current host verification: `python3 -m unittest discover -s tests -v` passed
238 methods, including the actual reader's 29 isolated C scenarios, the Linux
session's 35 EFI/libfdt scenarios, DownloadBlob/CPU-loan integration and three
export provenance tests. The actual ARM64 sources passed strict compilation.
The full run is recorded in `build/logs/os-boot-final-host-tests.log`.

A subsequent `test_os_boot_joint.py` now links the actual reader, Linux session,
PE parser, libfdt and SHA into one fixture. Five ASAN/UBSAN cases pass: actual
SFS files become closed immutable snapshots; the session consumes and releases
all three; NotReady refuses retirement/Start; retirement uses CPU data without
any later SFS call; and Before/Exit-return forbids ordinary BS cleanup. The test
also checks that a stopped UI flag does not invalidate a genuinely live BS
fence. The SFS/BS/kernel entry boundary remains a host fixture; physical
SFS-to-Linux verification is still missing.
The interface/lifetime review found the intended ordering compatible, but the
future `BootServicesAlive` binding must use a genuine BS/EBS fence independent
of `ProductRuntime.Alive`: policy retirement closes that runtime while Boot
Services still exist. Using the UI runtime flag would wrongly reject source
release and normal session cleanup after owner retirement.

The earlier product build `b4b90bee-4a74-48f1-b653-2497aaf002ea` predates this
build wiring and is superseded. The complete current build ID and image SHA
are authoritative in `artifacts/product/manifest.json`. Product status remains
`INCOMPLETE_NOT_RELEASE`. No product RAM boot or device write has been performed
by this host integration work.

The full rebuild subsequently completed successfully: build ID
`dd6d13cd-8047-4917-ae1e-2e7f7fbd4a46`, image size 28,860,416 bytes, SHA256
`7e35d9a4597a668f8b9fb952654d620c2ce9d12e18f1d4ce9b124996f16d86cf`.
`build_integrity.py validate --profile product` and the product RAM boot
candidate check without `--execute` passed. The actual product build produced
the three ARM64 objects (49,320 / 15,104 / 54,036 bytes); the final link map
contains none of their public loader APIs because Core does not yet reference
them. This is concrete evidence that compile wiring alone has not enabled the
loader. Final runtime binding remains required rather than forcing unused code
into the image and calling that implementation complete.

The subsequent full suite passed 243 methods in
`build/logs/product-os-joint-final-tests.log`; complete product build output is
`build/logs/product-os-loader-build.log`. These checks include the joint fixture
and canonical/generated/upstream source identity mutation tests. The tablet was
not rebooted or written by these checks.
