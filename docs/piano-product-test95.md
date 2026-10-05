# Product Test95: fast GUI service, valid screenshots and exact cold raw data

Unique product build`e1ccca0b-0407-4d4c-89d0-14273aa4a17c`, image SHA256
`6a529da955a0e19817c704d2dc06e2832ea16f04684daa00cac92b66d77dce18`, passed
368 host tests and successful fastboot boot. No persistent writes or routine
partition hash pass were performed.

The stock CLI completed version, oem ramlog, two metadata queries and a65536byte
upload in approximately0.08s. Frozen log CRC9C00FA93 was checked. GUI observations
show real tick2160..6706ms with root/last-child opacity255; software rendering is
not stalled at its zero-opacity animation.

The complete framebuffer screenshot arrived in7.481s:3200x2136,20505654bytes,
CRC D2D1EDC9 matching the device's screen metadata. It has4921 distinct pixel
colors and a full Chinese boot menu including BIOS and Shell. The displayed PNG
is only a lossless format conversion of the device BMP. This verifies actual
framebuffer contents, not the operator's physical panel view. That distinction
remains pending the operator's reply. Its CRC matches the capture reported in
test94, so the timing repair demonstrably improves retrieval but does not prove
it repaired a physical white-screen fault.

Revision2 HOB replay reconstructs all2328 RAM402 bytes in97 records, CRC7C271814,
and a20byte SIII descriptor, CRC2776A43D. SIII identifies81D00000/200000 with675
items and zero TLVs. The RAMv3 header has15 entries. They include a category14,
type1 range with nonzero raw size but zero available length, and three type1
entries with raw size0 and nonzero available lengths. These exact observations
explain the legacy range rejection; the native ABI must establish their meanings
before changing its observation model. No high-DDR allocation permission follows.

oem setup was accepted; the following stock version query and screenshot
capture also succeeded. The large Setup upload timed out at45s after receiving
17825792 of20505654 bytes. Its prefix differs from the SimpleInit image, but
the incomplete BMP is not a verified Setup screenshot or physical UI acceptance.
The outstanding upload/recovery and physical panel state remain to be resolved.

Exact evidence is in`private/analysis/usb-live-test95`, including device metadata,
CRC-checked raw binaries, decoded RAM records, screenshot and transfer timings.
The product remains an integration candidate, with full-DDR and general OS
admission still closed.
