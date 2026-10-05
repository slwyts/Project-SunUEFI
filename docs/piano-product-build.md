# One PianoUEFI product integration image

The product build exports exactly one boot image:
`artifacts/product/PianoUEFI-product.img`. The same FD, product supervisor and
APPv1 SimpleInit payload are present regardless of whether the image is loaded
by `fastboot boot`, a boot entry or a recovery entry. Physical acceptance of
those three entry paths is still pending. The builder does not flash partitions,
change slots or contact a device.

```sh
bash tools/build_product.sh > build/logs/product-integration-build.txt 2>&1
python3 tools/build_integrity.py validate --profile product
python3 tools/product_contract.py --build-manifest artifacts/product/manifest.json
```

The build first installs the pinned CoreWait/GUI and standard Shell/Setup source
hooks, compiles SimpleInit with the real product pump, prepares a dedicated
`pianoProductPkg`, builds the actual firmware and packages it. Diagnostic GUI
flags are not used to manufacture a combined product test image. ProductCore is
a distinct FV application with its own real entry point and parent lifetime.

The product BDS uses standard LogoDxe and BootLogoLib to display the actual
TianoCore bitmap, then starts the single ProductCore FV application. It does not
schedule the old 45/75/120-second diagnostic reset. Product SimpleInit uses
`boot.timeout=-1`, the actual bootmenu value that prevents a countdown timer;
the macro-disabled diagnostic build retains 150 seconds. The supervisor starts
SimpleInit automatically and receives F12 through real key-notify enrollment.

Core owns the native dependency foundation, standalone physical keys, persistent
UFS owner and read-only BlockIO, storage bridge, resident USB service and UI
policy. The USB owner is started once. CoreWait and GUI cooperative slices pump
that same service while children are active. UI return does not itself retire
the controllers. Deferred reboot/continue/boot actions pass through the typed
all-owner retirement manager. No diagnostic polling-loop application is loaded.

The source assembly includes actual standard FAT, Partition/DiskIo, complete
Shell command libraries including install1/bcfg, standard TianoCore UiApp/HII
and USB Host consumers. Source-only Host/pogo helpers do not imply a verified
physical producer. Unbound hardware backends remain explicitly NOT_READY.

## Build identity and packaging

`prepare_product.py` directly assembles the platform from canonical sources,
hash-verified native foundation PEs and original dependency expressions.
`bootprofiles/product-support` is the canonical tested GOP/serial/BDS source;
the ignored GUI staging checkout is not used as a required build input.
`PianoProductSimpleInitDigest.h` is generated from the actual separately built
ARM64 SimpleInit application. The ramdisk has the same APPv1 bytes whose complete
digest is compiled into ProductCore and rechecked against the live handoff.

Product build integrity fingerprints the actual prepared platform, canonical
Core/service sources, support sources, real CoreWait and Shell/Setup hooks,
native binaries, boot shim, DTB, product contract, build tools and product
SimpleInit application/payload/identity. A failed or changed build cannot retain
a packageable success marker. Packaging also checks the actual ProductCore and
DXE Core linker lists use the real pump library, FV READ_STATUS, shim payload
offset, DTB handoff cleanliness and every Android boot-image kernel/ramdisk
byte. The default format is Android boot header v3; v4 can be selected at
packaging time without changing the shared core or feature set.

## Current candidate is incomplete

`manifest.json` has `status=INCOMPLETE_NOT_RELEASE`. Every requested feature is
kept in the required enabled contract, and every backend has a separate explicit
implementation/validation status. A required feature declaration never means
that its physical backend is ready.

Original UFS media remains read-only. The earlier bounded FAT read/write and
restore test is evidence for that controlled transaction, not a general writable
product provider. EFI variables are RAM emulated. Host PCI_IO/NC DMA and Type-C
ownership, real pogo and touch transport, generalized OS image boot, the full
EFI DRAM contract and a verified 1 GiB download arena remain incomplete. Current
fastboot download capacity is 64 MiB; the contract retains the 1024 MiB target.

This artifact is a genuinely assembled integration candidate, not a renamed
diagnostic image or a completed release. No product device boot, background USB
acceptance across all UIs, persistent storage operation or final OS boot is
claimed by a host build.
