# DISPCC MM/MX access dependency and native request path

This is a source/binary audit, not a tablet power measurement. The held GCC
reference collector is implemented and tested; eight DISPCC LDRs remain gated
until a real controller power lifetime is bound. DPU remains independently gated.

The fixed register-byte baseline is kernel commit
`352508459733d3e6d349ea5581a8dd2fd8bb4180`. Its `sm8750.dtsi:3750` gives DISPCC
an RPMHPD_MMCX domain and low-SVS requirement; `dispcc-sm8750.c:1912` resumes
runtime PM before controller accesses. Official primary source shows the same
[DT dependency](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8750.dtsi)
and [probe ordering](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/dispcc-sm8750.c).
The ROM Android DT expresses MM/MX through regulator supplies rather than a
`power-domains` property. Neither that omission nor a mapped device page proves
those supplies are held in the current UEFI session.

The UEFI configuration supplies the exact resource-name bridge:

| Evidence | Native name | RPMh resource |
| --- | --- | --- |
| `private/analysis/xbl-sw.dts:4700` | `/vcs/vdd_mm` | `mmcx.lvl`, DRV2 |
| `private/analysis/xbl-sw.dts:4676` | `/vcs/vdd_mxa`, alias `/vcs/vdd_mx` | `mx.lvl`, DRV2 |

The decompiled XBL SW text is199529 bytes, SHA256
`4064896bfc513a59b3ea54fc989adb6056647afb224fa7b209404cd732dbbc2f`.
The checked native binaries are exactly those in
`private/analysis/native-driver-inventory.json`:

| Producer | PE SHA256 | Relevant code/data RVAs |
| --- | --- | --- |
| ClockDxe | `f9e85aa758932b4366c55ec83b58e0176f58fba2944cc2fdd34875a46fdb769e` | CESTA second node2E520/name13B4E/parent30478; rail3 object3DE90 points to `/vcs/vdd_mm` at164C3; rail4 object3E050 points to `/vcs/vdd_mx` at164CF |
| NpaDxe | `19a7df5cdf25ae247ca5628bd67496729f17f30d74e564a7d7f999ac2d5934b9` | protocol data110B8/revision10003; +1B8→1AF8→3DA8; +1B0→1AE0→4390 |
| VcsDxe | `d335dbcfd4175e6d9f189062bc777961f497e968d66acc7dacb4aae8b3055e4f` | DT parser6150..61C4/regulator-name,62AC..62C0/resource-name;632C→6D1C→6734 NPA resource creation;6A8C rail driver;backend tableA2C0/A2C8;7218 RPMh corner handling |

The second clock is `disp_cc_mdss_non_gdsc_ahb_clk`, typed ID02010006. Its
parent's mask8 selects MM rail3; this is not a direct MX vote. The parent clock
reference keeps the domain vote only if the real NPA client/backend is present
and effective. Clock B920 uses the ordinary rail request table, but a NULL client
still permits the software cached corner to change at B9F0. That cached field
cannot establish a power lifetime.

The NPA ABI is specifically a batch request, not a guessed two-argument scalar
request. Clock B73C initializes a stack batch list to NULL, B920/11824 forwards
the pointer-to-list, real client and corner to NPA+1B8. NPA3DA8 updates the pending
client request slot and adds the changed client to the list. At Clock B87C/B880,
a non-NULL list is submitted through117BC/NPA+1B0. NPA4390 dispatches one client
through its+88 callback or multiple clients through9808 resource aggregation.
The protocol wrappers return zero after their internal calls; this is not a
separate hardware acknowledgment.

VCS6A8C validates the resource/rail relationship and dispatches corner handling
through the actual rail implementation. The RPMh backend table atA2C0 contains
initialization711C and set-corner7218. At6C44, backend failure logs and leaves the
old applied corner; success reaches the applied rail state. Backend7218 builds
command sets, submits them at744C, and waits at7460 only when the command's
completion flag is set. Clock cached corner, NPA active-state and VCS applied
corner must be kept distinct. Current live client/resource/command-state data
and the active submit/completion path have not been captured by this audit.

The next implementation can acquire the precisely named second native clock
instead of writing MDP/GDSC registers. Before accepting that as a controller
access lifetime it needs actual pinned LoadedImage/vtable identity plus bounded
client→NPA resource→VCS rail→RPMh backend evidence, applied ordinary corner and
matching domain/clock counters; uncertainty or a partial request must retain
the owner. For the eight controller reads, the minimum source-backed access
dependency is MMCX plus the GCC interface clock. The actual Linux node has only
MMCX and its probe accesses the controller after that runtime resume; no
independent MX vote is introduced. The ROM MX supply can support PLL/frequency
choices, so merely finding it does not make it an extra admission gate for these
reads. MX remains useful raw diagnostics, not a required second owner for this
narrow scope. DPU/DSI operation still needs its separate domain/clock contract.
The current Clock reader only admits its exact GCC object graph. New NPA/VCS
objects need their own pinned, typed bounded reader and ABI validation; they must
not be admitted by enlarging an arbitrary low-memory range. No native rail call,
MMIO write, firmware build or device operation was performed for this audit.
