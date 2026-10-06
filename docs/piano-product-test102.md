# Product test102: clean text-map refusal precedes native clock acquisition

Product build2498deeb-b372-40c6-8662-e22460b92c42, image SHA256
4a6869991a3a9540afba1aef4a73d61904aa2154f5bd129e5087f6f9277e5713,
passed393 host tests after correcting the bounded-reader structure-padding
comparison, plus actual build/identity checks. The joint pipeline includes the
real lease, CPU reader, Guard and product GCC adapter, with native calls still
represented by host boundaries.

RAM-only boot was sent successfully. The operator observed TianoCore followed
by millisecond self-test text, then a white/grey screen. Product fastboot did
not enumerate within60 seconds. Manual power recovery returned Android;
boot_completed=1 was checked and the preserved log collected. No flash,
persistent media writes or routine partition hash pass occurred.

Native foundation/Clock initialization completes. The product then refuses its
first live Clock text read atCFF0B000 (ClockBaseCFF0A000+1000):

| Field | Observed |
| --- | --- |
| Lease identity/status | Not Ready |
| GetID / Enable / counters / hardware proof | Not started |
| Held / owned references | 0 / 0 |
| Reader role / bytes | Text / 100 hex |
| Reader Guard sessions / words | 0 / 0 |
| Reader retained / Lease retained | 0 / 1 |
| Cleanup | Not started |

There is no evidence of an attempted native Enable or a display-reference
increment in this run. Thus its white screen does not test the proposed clock
repair. The cached stage report locates the refusal before Guard Begin, in the
EFI memory-map admission. This build did not log the rejected descriptor's
exact type/attributes; that value must not be invented.

Source audit found two implementation problems to correct before retry:

1. Reader required EFI map cache bits to equal WB exactly. The actual Mu
   CoreGetMemoryMap path reports capability-derived cache attributes; multiple
   supported cache bits are not the actual current cache mode. Current cache
   still requires independent exact GCD WB and CPU PAR FF checks. New diagnostic
   fields must retain the rejected descriptor and precise reason.
2. Lease marked every ReadCpu error retained. Even this first pre-Guard,
   pre-native clean refusal bypassed exact FreeCopy. A fresh typed reader
   outcome can distinguish this narrow zero-session case, release both
   temporary FV copies and retain standard background diagnosis. Unknown or
   partial cleanup, any native Enable attempt, event or lost lifetime must still
   retain; a generic error code alone is not sufficient evidence.

Evidence is in private/analysis/ramlog-test-102/console.txt and the archived
test102 hardware-result. Physical display recovery and successful reference
acquire/release remain unverified.
