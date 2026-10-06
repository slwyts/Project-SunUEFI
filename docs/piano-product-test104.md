# Product test104: non-GDSC display clock has no active reference

Build70f59ef1-fb96-4195-9a78-7397788bbe93, image SHA256
bc9458208ec443363b6f5c529ffba8a139e0c389f1532a5320f0ca62ad0bca18,
28,905,472 bytes. The single product image passed firmware compilation,
packaging and input-identity validation, then RAM-only fastboot boot. Targeted
actual Lease/Reader/Guard/selector/pipeline, product owner and flat source wiring
tests passed. No flash, UFS write or routine partition hash pass was performed.

The first real selector capture succeeds with two matching observations:

| Field | Actual value |
| --- | --- |
| Clock image | CFEEC000 |
| non-GDSC expected selector ID | 02010006 |
| Node / parent | CFF1A520 / CFF1C478 |
| Ordinary / alternate node refs | 0 / 0 |
| Ordinary / alternate client refs | 0 / 0; client entry present |
| Ordinary / alternate parent domain refs | 0 / 0 |
| Parent rail mask | 8, selects MM |
| Current config | CFF1D220 = image+31220, pinned |
| Config corner / cached vote / alternate | 56 decimal (38 hex) / 0 / 0 |
| MM / MX client pointers | D00F16C4 / D00F1594 |

The native client pointers are four-byte aligned. A future typed NPA reader
must audit the actual allocation ABI rather than reject them on an invented
eight-byte alignment rule. No NPA client object was dereferenced by this
selector, and no native second-clock or rail request was issued. The pinned
configuration corner is not an applied voltage; cached zero does not establish
physical power-off. These values identify a missing native clock/domain
reference to investigate, not a proven white-screen cause.

The resident GCC AHB owner remains held1/owned1/retained0. Three real product
observations borrow and freshly return it successfully, tokens1/2/3, each with
ordinary total/client1/1 before and after. The reader finishes4868 short sessions
and47788 word reads without retention. DISPCC and DPU loads remain unstarted.

Stock Fastboot returns the complete262144-byte log, CRC32 86E965A7. Reboot
acknowledges successfully in1.103 seconds, followed by live Android
sys.boot_completed=1. The preserved console confirms clean UFS shutdown and:

    PIANO_DISPLAY_RELEASE status=Success clean=1 owned=1/0 refs=1/0 retained=0

No manual recovery was needed. This run deliberately collects no screenshot
or operator panel verdict; physical white-screen recovery remains unverified.
The isolated cold BootObjects candidate is not bound into this image.

Evidence: private/analysis/usb-live-test104/{manifest.json,ramlog.bin,ramlog.txt},
private/analysis/ramlog-test-104/console.txt, private/analysis/stage0-test-104.json,
and the sealed artifacts/tests/stage0-test-104 image/FD/build manifest.
