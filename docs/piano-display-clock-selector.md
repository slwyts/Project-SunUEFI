# Fixed native clock selector observations

`PianoDisplayClockReadSnapshotClock` adds a bounded read-only diagnostic path
to the existing actual Clock Reader. Its default `ReadCpu` and `DcrResolve`
contracts are unchanged. The selector API requires completed pinned PE/live
text verification, fresh LoadedImage identity, the real INTERNAL registry/client
graph and independent EFI/GCD/PAR protected CPU reads. It never calls GetID,
Enable, Disable, IsOn, NPA or a control-register read.

| Selector | Expected ID | Module / array / count | Node / parent |
|---|---|---|---|
| GCC display AHB | `04010033` | `28678 / 32418 / 149` | `33A68 / 37528` |
| CESTA non-GDSC AHB | `02010006` | `28548 / 2E280 / 61` | `2E520 / 30478` |

All addresses above are RVAs in the hash-pinned Clock PE. Expected IDs identify
the fixed selector; they are not new observed native GetID results. The source
Reader retains its real primary GCC identity while the second graph is read.

Two complete matching observations are required before publishing a
`PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT`. It records node/client/domain counters,
flags, the parent rail mask, current-config pointer, cached corner and static
MM/MX client pointer slots. Zero or missing selected client refs are legitimate
diagnostic observations. A non-GDSC current-config pointer matching exactly one
of `31220 / 31258 / 31290` can expose its pinned corner `38 / 80 / 100`; any other
producer remains explicitly unknown and is not dereferenced. Output stays
unchanged on failure; known source/output aliases and malformed or unbounded
reference chains are rejected.

The second parent mask8 selects `/vcs/vdd_mm` in the native BSP. The collected
MM/MX pointers and cached corner do not prove an applied rail request, RPMh
acknowledgment, MMCX readiness, a held second clock or DISPCC/DPU permission.
Those require separate actual resource, request and lifecycle evidence.

28 ASAN/UBSAN actual Reader+Guard selector cases and AArch64 Reader/Observe
combined-header checks pass. The existing69 Reader cases also pass. The host
fixture uses the actual captured PE and guarded-read implementation; CPU/EFI
boundaries are fixtures, and no native clock or hardware method is executed.
Root binds one read-only snapshot after successful GCC acquire and caches it
for later replay. Test104 verifies that actual path: two matching snapshots,
zero node/client/domain refs, pinned config corner38hex, cached vote0 and
non-NULL MM/MX client pointers. It also verifies clean owner retirement and
Android recovery. See [test104](piano-product-test104.md); actual rail application
and physical panel recovery remain unverified.
