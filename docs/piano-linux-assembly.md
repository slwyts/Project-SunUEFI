# Shared Stable/Next Linux EFI candidate assembly

`tools/assemble_piano_linux.py` verifies the actual sealed Stable d421587 and
Next4985691 Image/config/module sets, a complete current-ROM folded DTB chain,
and one common GNU-root/bootstrap. It creates no firmware profile. The two
entries have the same complete device scope and use one shared initramfs.

Default behavior only verifies actual source files and prints a report. Explicit
`--output artifacts/linux-assembled/<new-name>` creates the real EFI file tree
by copying the already-built inputs after verification; it never repacks or
changes the source root. The destination must not exist. Do not point it at a
live ESP or device. The layout is:

```text
EFI/Piano/stable/Image.efi
EFI/Piano/stable/config
EFI/Piano/stable/piano.dtb
EFI/Piano/next/Image.efi
EFI/Piano/next/config
EFI/Piano/next/piano.dtb
EFI/Piano/shared/initramfs.cpio
manifest.json
```

Both entries always remain `enabled=false`, with typed admission blockers for
live full-DDR EFI memory ownership, native late-APP ExitBootServices retirement,
and real hardware route/context readback. There is no host `--ready` override.
ARM64 EFI headers are required; a successful raw RAM-smoke boot cannot satisfy
this interface. This manifest describes inputs for the product's standard EFI
Linux session; it is not installed as Boot#### or a fabricated hardware proof.

An explicit verification command once Root provides the final common archive
and matching bootstrap is:

```sh
python tools/assemble_piano_linux.py \
  --rootfs build/distros/debian13-piano-full \
  --root-payload artifacts/linux-full-rootfs/FINAL_COMMON_ROOT \
  --bootstrap artifacts/linux-full-rootfs/FINAL_COMMON_BOOTSTRAP \
  --dtb private/analysis/piano-linux-managed-dsp-pcie-v1/Piano-full-linux-managed-dsp-pcie.dtb \
  --dtb-manifest private/analysis/piano-full-dtb-fd6266-impact-fixed/manifest.json \
  --dtb-manifest private/analysis/piano-linux-owned-dma/manifest.json \
  --dtb-manifest private/analysis/piano-linux-managed-clocks-v1/manifest.json \
  --dtb-manifest private/analysis/piano-linux-managed-dsp-pcie-v1/manifest.json
```

The two `FINAL_*` names are mandatory explicit caller inputs, not fallbacks to
an obsolete root. `--stable` and `--next` optionally select the exact sealed
artifact directories; their source commits remain fixed in the interface.

Verification reads the actual ARM64 EFI Image, full configuration hash and50
requirements, all ELF modules and vermagic, all original module SHA256 values,
complete kernel dependency closure, staged per-release provenance, each release's
external v4l2loopback ABI, and the staged root's complete module indexes. It
requires those exact files to exist as archive members with equal bytes. The
GNU archive is streamed through the existing complete-member checker, including
guest ownership, hard links, SHA256, capabilities and ACL evidence. Its tree
fingerprint and counts must match the payload manifest.

The newc bootstrap is read with bounded chunks. Its complete file hash, actual
embedded compressed root hash/size, GNU tar runtime members, static BusyBox,
entry script and page-budget/hash metadata are verified. No 1GiB buffer is
allocated. Both kernel+DTB+shared-bootstrap totals must fit1GiB and the expanded
root must fit the separate4GiB tmpfs page budget.

Every DTB chain stage's actual output bytes are checked with libfdt. Later
stages must retain the exact node set, phandle map and reservations; their
property diffs must match the declared changes and recognized class. Complete
fold metadata remains tied to ROM base4/overlay0 and published fd6266. Old
initrd addresses and random seeds are refused. Binding and bypass diagnostics
remain in the manifest; these checks do not grant DDR ownership or prove DMA.

`CONFIG_CMDLINE_FORCE=y` is explicitly reported for both current kernels.
External LoadOptions cannot enable Linux USB-debug flags while this configuration
is active. The RAM distro now supplies validated USB/shell/network defaults via
`/etc/piano/linux-debug.conf`; compiled command-line values still take priority.
The assembler preserves the existing console/debug command line, refuses Android
userdata root arguments and records the public BT_LE=n limitation.

Six component tests use the actual sealed kernels and four-stage DTB chain,
reject the old archive missing the final releases before a large read, test
real small newc members/corruption, reject duplicate/escaping tree paths, and actually copy a small disabled EFI file tree with overwrite rejection.
The existing runtime-complete archive is deliberately rejected as the common
root for these two new releases. Final common archive assembly has not yet run;
Root is producing those inputs. No root repack, device access or boot claim is
made by this unit.
