# Product Test94: startup logo, live standard fastboot, white SimpleInit

Build `6e86ce53-3d85-4e83-8878-61e672868aab`, image SHA256
`50ea029ab760ee28b13a2460d3a7d2cadaf051c8763356903e53671445cac838`, passed
366 host tests and was sent with fastboot boot. No persistent writes occurred.
The operator saw TianoCore first, then a white screen. GOP publication changes
therefore did not resolve the SimpleInit regression.

The host captured configuration1, interfaceFF/42/03, two endpoints,5Gbps USB,
serialSunUEFI-piano and readable/writable USB-node access for the current user.
Stock fastboot37 identified the device, returned version0.4, and acknowledged
oem ramlog. A65536-byte frozen log was uploaded and size/CRC checked. This is
actual product command/USB evidence, not merely descriptors or host tests.

The live log re-emits the saved cold HOB: RAMv3 payload2328bytes CRC7C271814,
stable cookie81EFF350, failure reason18(RamRange), no parsed bank/preload list.
The parser gate remains closed; this does not authorize high DDR. Raw records
and the SIII snapshot need durable retention before startup-log wrap.

oem screenshot acknowledged a3200x2136 capture,20505654bytes,
CRC32D2D1EDC9. get_staged timed out at20seconds and left a zero-byte host file;
that is not a screenshot and says nothing about its pixels. A subsequent
version query also timed out. The user recovered Android; boot-complete1 was
checked, without the routine partition hash pass discontinued at their request.

The recovered full ring still has successful USB IN DMA completions through
sequence9385. The actual SimpleInit GUI loop passes a100ns timer-period value
to Stall, which expects microseconds:30ms becomes300ms. One background pump per
loop also limits bulk progress. Correct units and bounded cooperative waiting
are the next repair. This is a confirmed timing defect, not proof of the white
screen cause. The menu initially uses opacity0 and a500ms animation, so actual
LVGL tick, opacity and refresh observations must be checked against working
test24 rather than attributing the regression to hardware alone.

Evidence is under `private/analysis/usb-live-test94`,
`private/analysis/usb-host-metadata-test-94.json`, and
`private/analysis/ramlog-test-94`. The original binary ring is unchanged.
