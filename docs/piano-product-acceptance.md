# First product fastboot acceptance

The first acceptance covers SimpleInit background fastboot, CPU RAM transfer,
CRC-frozen logs, current GOP capture and readonly UFS partition fetch. It does
not establish all UI routes, product storage writes, OS boot or release readiness.

After Root successfully sends the candidate with `ram_boot_stage0.py --profile
product --test-id TEST_ID --execute`, use:

```bash
python3 tools/check_product_fastboot.py --test-id TEST_ID --phase simpleinit
```

This script uses the installed stock `fastboot` only. It first validates the
matching successful product RAM test record, sealed image/FD/BootShim hashes,
build ID and build input snapshot. No CLI command reaches the device unless
that association and the original PC partition backup validate. This is host
provenance, not build identity attestation over the USB protocol.

Evidence remains private under
`private/analysis/product-fastboot-test-TEST_ID/simpleinit/`. The manifest
records every command/status/timeout. A failed run also saves its manifest;
existing evidence directories are never overwritten. Each enumeration wait
and each subprocess timeout is bounded to at most 60 seconds. The phase label
does not navigate the UI or prove that the captured contents belong to that UI.

The default sequence has no reboot/boot/continue action and no storage writes:

- Verify product/version/serial/storage policy, current 64 MiB download limit,
  64 KiB fetch chunk limit and readonly partition geometry.
- Transfer a deterministic 65,553-byte CPU pattern with `stage`, check its
  `oem sha256`, then compare `get_staged` byte for byte.
- Freeze console with `oem ramlog`, retrieve it with `get_staged`, and check
  size, CRC and generation. Logs can wrap; absence of an old session marker
  does not itself fail the check. Product CORE_READY/USB_START presence is
  recorded as evidence, not required to survive every later snapshot.
- Capture `oem screenshot`, retrieve a BMP, and validate its CRC, dimensions,
  row pitch, file size and 24-bit bottom-up layout against advertised metadata.
  Root still needs to inspect the actual picture.
- Fetch the complete 524,288-byte `xbl_config_a` and compare it to the verified
  original PC backup. Capture another CRC log and discard the CPU staging data.

To send an ordinary reboot only after all checks pass:

```bash
python3 tools/check_product_fastboot.py --test-id TEST_ID --phase final --reboot-after-check
```

An acknowledged reboot is not owner retirement or Android recovery proof. Root
must subsequently collect the retained console, verify the exact USB/UFS/input
retirement evidence, and check all 26 boot partition hashes after Android returns.

## Exact current command surface

All direct commands select `-s SunUEFI-piano`:

| Stock fastboot arguments | Current expected behavior |
| --- | --- |
| `getvar product` / `getvar version` | `piano-sunuefi` / `0.4` |
| `getvar max-download-size` | `0x04000000` (64 MiB) |
| `getvar max-fetch-size` | `0x00010000` (64 KiB) |
| `getvar partition-size:xbl_config_a` | `0x0000000000080000` |
| `getvar SunUEFI:ram-boot` | `disabled`; product has not registered a boot backend |
| `oem status` | Readonly policy and DWC status; not a full owner retirement report |
| `oem ramlog` → `get_staged FILE` | Frozen console tail, up to 65,536 bytes |
| `oem screenshot` → `get_staged FILE.bmp` | Current native GOP screenshot |
| `fetch xbl_config_a FILE.img` | Guarded readonly UFS reads, complete PC comparison |
| `reboot` | True IN ACK, cooperative UI return, full owner manager retirement, then cold reset |
| `continue` | Currently the same clean cold reset; it does not start an OS |
| `boot FILE` | Currently rejects: `RAM boot backend unavailable` |
| `oem log` | Rejects: `log service unavailable`; use `oem ramlog` |
| `oem setup` / `oem shell` / `oem simpleinit` | Resident product only: validate fresh runtime, latch target UI and reply; core dispatches after normal child cleanup |
| reboot targets / flash / erase / slot commands | No registered backend; command denied |

The resident product also accepts these stock CLI navigation commands:

```bash
fastboot -s SunUEFI-piano oem setup
fastboot -s SunUEFI-piano oem shell
fastboot -s SunUEFI-piano oem simpleinit
```

They use the existing runtime actions and preserve USB/DMA owners. The current
child returns through its normal cleanup before the parent dispatches the next
UI. A command ACK means its request was accepted, not that the target UI has
already appeared. Check the captured contents and run the same checker under
separate private phase labels after each transition. A request for the current
UI is an idempotent success. F12 remains the input path for Setup.

Old diagnostic images with SERVICE disabled still deny these commands. A
resident service without the actual matching live runtime backend returns a
failure. Earlier archived product revisions before this navigation change have
no OEM navigation entry; use the matching new single product revision for this
acceptance. Do not infer Setup/Shell acceptance from a SimpleInit capture.
