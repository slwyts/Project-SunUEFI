# Product test100: native display CESTA initialization hits missing MMIO mapping

Product build `89e0e34d-275a-4bd3-8c7d-83ebe2547db2`, image SHA256
`9ff6f258e5183731fda4eb5ef12995a7cd0acc51981fd74e4d2c926c0b0c2d4c`, passed
386 host methods and actual build/identity checks. RAM-only fastboot boot
succeeded. The operator saw a blank white/grey screen; product USB did not
enumerate within40 seconds. Manual power recovery restored Android, with
sys.boot_completed=1 checked and the preserved firmware/exception log collected.
No flash, media writes or routine partition hash pass occurred.

The new cold table has52 descriptors: original49 unchanged and three separately
bounded Device/MMIO mappings for GCC, DPU and DISPCC. The actual observation
reached26 complete phases, through pre:ClockDxe. There is no post:ClockDxe,
after-foundation, UFS or USB initialization in this run.

All26 GCC pairs are coherent: DISP_AHB127004=`88000003`, HF_AXI127008=`08200001`.
All GCC Guard sessions end cleanly after104 total protected loads. DISPCC
AF08000/2000 mapping qualification also succeeds with Device PAR/GCD UC,
zero target loads and exact cleanup. Eight DISPCC and six DPU registers remain
explicitly skipped. The26 MDSS sessions perform14248 loads and end with active0,
handlers0/0, retained0. The parent closes every observer before native StartImage.

The default CPU handler then records a native ClockDxe fault:

| Field | Value |
| --- | --- |
| Clock image base / PC | `CFF34000 / CFF40234` |
| PE relative instruction | `C234: STR W11,[X10,X12]` |
| X10 / X12 / store target | `AF27800 / 56C / AF27D6C` |
| Store value | `38C15000` |
| ESR | `96000047`: same-EL write, level3 translation fault |

This is inside ClockDxe's own StartImage initialization, after the observer
returned. A protected observer did not leave an exception handler active and
did not perform this store. The store failed CPU translation; this is not an
observed completed device transaction or a bus fault diagnosis.

Pinned ClockDxe PE f9e85aa758932b4366c55ec83b58e0176f58fba2944cc2fdd34875a46fdb769e
uses descriptor3E4B0 (`disp_cesta`, `/soc/cesta@af27000`). Runtime reg slots are
filled from the native XBL DT's exact five names. The actual input
private/analysis/xbl_config_a-0x8358.dtb has SHA256
634ec73dc6d69a07b5af8246e03b0d2ae84dfb9ce9901135121c220443c9cd10.

| Native CESTA resource | Base | Bytes |
| --- | --- | --- |
| SDE_CRMB | `AF27000` | `400` |
| SDE_CRMB_PT | `AF27400` | `400` |
| SDE_CRMC | `AF27800` | `2000` |
| SDE_CRMV | `AF29800` | `700` |
| SDE_CRM_COMMON | `AF29F00` | `100` |

The exact contiguous union is AF27000..AF2A000,3000 bytes. Android's CRMC
syscon and crm_c resource corroborate it. Ordinary DISPCC ends atAF20000; its
window does not cover the target. The separate RSC atAF20000 is also a different
resource and cannot fix this store by itself. Both native initialization tables
remain in their declared subresources: CRMC99 words at56C..6F8 exclusive and
CRMV28 words at4EC..55C exclusive.

The next mapping correction must add the distinct verified CESTA resource,
not enlarge DISPCC based only on FAR. Native initialization writes real vote
commands; source-only mapping/observation must not be described as proving
display readiness or as making native initialization read-only.

Test99 had a completed post:ClockDxe with the same PE/DEPEX. New mappings may
have exposed a deeper initialization path, but the precise branch difference
has not yet been proved. This locates test100's stop point; it does not establish
the earlier physical white-screen cause.

Raw preserved data is in private/analysis/ramlog-test-100/console.txt and the
bounded comparison in private/analysis/usb-live-test100/display-fault-comparison.json.
