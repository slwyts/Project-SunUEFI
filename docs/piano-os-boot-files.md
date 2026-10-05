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
real platform binding, not alternate feature-specific firmware images. This
change does not enable a menu entry or change the unique product artifact.
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
The full run is recorded in `build/logs/os-boot-final-host-tests.log`. A direct
reader-plus-session joint fixture and physical SFS-to-Linux test are still
missing; separate passing fixtures do not establish those results.
The interface/lifetime review found the intended ordering compatible, but the
future `BootServicesAlive` binding must use a genuine BS/EBS fence independent
of `ProductRuntime.Alive`: policy retirement closes that runtime while Boot
Services still exist. Using the UI runtime flag would wrongly reject source
release and normal session cleanup after owner retirement.

The existing unique product build remains fresh at build ID
`b4b90bee-4a74-48f1-b653-2497aaf002ea`, Image SHA256
`06dd2587ef3e83163b21912331fddda46bcd2ac1feba54eef828fe5b2e4f6f23`.
Its status remains `INCOMPLETE_NOT_RELEASE`; the source work here is outside
that build's input set. No product RAM boot, device write or new product build
was performed in this change.
