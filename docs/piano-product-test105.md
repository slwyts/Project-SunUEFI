# Product test105: real MM request and cold-object timeout captured

Build93f17492-6c0e-4244-bad5-b90d94e6ce21, image SHA256
561cf1a7578031858a8b8ad370b0cc1bcba5f45afb48778ddc65fbeb88d3f534,
28,913,664 bytes.405 host tests passed, followed by actual product compilation,
packaging and input validation. The same product was loaded with fastboot boot.
No flash, UFS write or routine startup partition hash was performed. The staged
non-GDSC clock owner is not linked into this image; no new native Enable occurs.

## Display rail observation

NativeProbe's actual started handles identify NPA at D000F000 and VCS at
CFFC8000. Both complete FV pins, normalized live code and interface identity
checks succeed. The first MM object graph then stops with NotReady, without
retention or an unclosed Guard. Its captured values are:

| Field | Actual observation |
| --- | --- |
| MM / MX client | D02476C4 / D0247594 |
| MM resource | D0248E94 |
| MM client type / active slot | 40hex / 1 |
| MM active request / pending request | 48 / 56 decimal |
| NPA applied aggregate | 48 decimal |
| Clock cached vote / configured corner | 0 / 56 decimal |
| Clock parent ordinary / alternate refs | 0 / 0 |

**A zero Clock cached vote does not mean the NPA request is zero.** Actual NPA
state is48 while the Clock cache is0. VCS rail/applied state has not been read:
zero in its incomplete output is not an observed voltage or off state. The
refusal occurs after the resource's applied value and before the VCS rail link;
additional exact field/map diagnostics are needed to identify the rejected
object rather than broadly relaxing heap access. Physical display recovery
remains unverified; no screenshot or operator verdict was collected.

## Cold handoff and occupied objects

The real extended BootShim and single SEC hook execute. The frozen1256-byte HOB
is captured successfully; validation correctly returns NotReady because the
DTB scan times out. The handoff itself is stable and accepted:

| Field | Actual value |
| --- | --- |
| Shim / source FD | A8080000 / A80801A0 |
| Shim bytes / FD destination / FD bytes | 1A0 / A7100000 / 300000 hex |
| Original DTB | B4498000 |
| Original entry PC / SP / EL | A8080040 / A7707860 / 4 (EL1) |
| Extension flags / CRC | 7F / E80117A4 |
| Cold PC / SP / VBAR | A7106698 / A764BDE0 / A7177000 |
| SCTLR / DAIF / SPSel | 30D01988 / 3C0 / 1 |
| TTBR0 / TTBR1 | A7673000 / 0, diagnostic while M=0 |
| Counter frequency | 19,200,000Hz |
| Attempted word loads / recovered faults | 927 / 0 |
| Elapsed / last completed address | 2,000,765us / B4499BB0 |

The fixed compiled objects, real shim/source and actual HOB prefix are retained
as12 occupied-object records. No complete DTB/initrd result is fabricated. The
failure HOB remains publishable after the read budget expires, proving the new
diagnostic path works. Ownership, high DDR and allocator authority remain false.
The next fix should reduce repeated small protected reads using bounded chunks
and metadata caching, rather than simply extending the time limit.

## Recovery and evidence

Stock Fastboot returns a complete262144-byte log with CRC AF932F63. Reboot
acknowledges in0.051 seconds; live Android sys.boot_completed=1 confirms actual
recovery. Preserved logs report clean UFS shutdown and:

    PIANO_DISPLAY_RELEASE status=Success clean=1 owned=1/0 refs=1/0 retained=0

The actual product stop path reaches GCC release only after RailClose succeeds.
No manual recovery was needed. Evidence is under
private/analysis/usb-live-test105, private/analysis/ramlog-test-105,
private/analysis/stage0-test-105.json and artifacts/tests/stage0-test-105.
