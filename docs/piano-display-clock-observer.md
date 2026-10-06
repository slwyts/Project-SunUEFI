# Fixed display control register contract and unfinished observer

The register-byte baseline is kernel commit
`352508459733d3e6d349ea5581a8dd2fd8bb4180`, not a claim about the current built
kernel. The relevant GCC/DISPCC/DTS bytes also match Root's d421 and later
charging-series checkout. The later Android runtime DT is
`private/analysis/android-memory-2026-10-05/live.dtb`, SHA256
`8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc`.
The earlier captured DT SHA256 is
`a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7`;
these are distinct files and must not be called the same capture. The register
bases/offsets below are independently consistent with the ROM resources and
the pinned driver/catalog bytes.

## Sixteen fixed addresses

| # | Address | Register and read meaning | Source / side effects / prerequisite |
| --- | --- | --- | --- |
| 1 | `00127004` | GCC display AHB branch enable, bit0 | GCC probe keeps offset27004 enabled; ordinary control read, not a held bus lease. Requires qualified GCC MMIO access. |
| 2 | `00127008` | GCC display HF AXI CBCR | GCC offset27008, bit0 enable; `BRANCH_HALT_SKIP` means bit31 is not reliable proof of a running branch. Requires qualified GCC access. |
| 3 | `0AF09000` | MDSS GDSCR control | DISPCC gdsc9000; SW_COLLAPSE bit0/HW_CONTROL bit1. Ordinary controller register; controller bus/rails must be safely accessible. |
| 4 | `0AF09004` | MDSS CFG_GDSCR status | `POLL_CFG_GDSCR`: actual ON polling uses power-up-complete bit16; down-complete bit15. Do not substitute GDSCR bit31. Same controller prerequisite. |
| 5 | `0AF080B0` | MDSS AHB CBCR | DISPCC offset80B0; bit0 enable and branch2 halt/FSM status. Bus access prerequisite; an observed bit is not ownership. |
| 6 | `0AF08010` | MDSS MDP CBCR | DISPCC offset8010, branch2 AON ops. Same controller prerequisite. |
| 7 | `0AF08034` | DSI byte0 CBCR | DISPCC offset8034; ordinary clock read, no enable/write performed. |
| 8 | `0AF0803C` | DSI byte1 CBCR | DISPCC offset803C; same. |
| 9 | `0AF08004` | DSI pixel0 CBCR | DISPCC offset8004; same. |
| 10 | `0AF08008` | DSI pixel1 CBCR | DISPCC offset8008; same. |
| 11 | `0AE3626C` | INTF1 STATUS bit0, engine enabled | DPU get_status uses normal read. Requires a genuinely held MDSS power/clock/access lifetime. |
| 12 | `0AE3726C` | INTF2 STATUS bit0 | Same requirement, dual-DSI second interface. |
| 13 | `0AE360AC` | INTF1 frame counter | Normal repeated counter read in get_status; dynamic count is not a source-buffer/display acceptance proof. |
| 14 | `0AE370AC` | INTF2 frame counter | Same requirement. |
| 15 | `0AE05014` | VIG0 programmed SRC0 low32 address | Ordinary configuration register. The source does not show read-clear, but this is not proof that VIG0 is active or the register is an active-buffer mirror. |
| 16 | `0AE25014` | DMA0 programmed SRC0 low32 address | Same; an unused pipe's stored address is not a scanout diagnosis. |

The INTF, CBCR and GDSCR source paths use normal register reads and no read-clear
operation for these locations. No FIFO, IRQ-clear, reset, trigger or MISR register
is selected. This is source evidence, not complete vendor TRM confirmation of
every implementation-defined effect. DPU timing/configuration registers must not
be loaded while domain/clock accessibility is unknown or off: a CPU exception
guard cannot interrupt a bus transaction that never completes.

ROM MDP physical base is `AE00000`; the mainline DPU child base is `AE01000`.
Thus ROM INTF36000/37000 equals child+catalog35000/36000, and ROM SSPP5000/25000
equals child+catalog4000/24000. Apply this base difference once, never twice.

## Implemented narrow behavior

`PianoDisplayClockObserve` uses an independent short `PianoGuardedRead` session
for fixed GCC page `127000/1000`, requiring actual GCD MMIO/UC, device PAR00,
identity translation and fresh CPU/handler/lifetime validation. It reads each of
the two GCC registers twice. An unstable pair or disabled AHB prevents any
subsequent DISPCC qualification.

If the GCC pair is stable and AHB enable is observed, a second short guard session
qualifies only `AF08000/2000` using CPU/GCD/AT validation. It performs **zero
DISPCC LDRs**. This validates two mapped pages, not controller bus/rail safety.
The current module has no audited controller accessibility/hold backend, so all
eight DISPCC registers remain declared, required and skipped. No caller boolean
can relabel the observed bit as a bus lease.

The six DPU registers are always explicitly skipped while there is no actual
held domain/clock/access lifetime. Future release requires a real platform or
native Clock owner/hold interface, not synthetic readiness or a momentary double
read. Qualified CFG power-up and AHB/MDP/DSI clock states will be necessary
observations, but they do not themselves acquire that lifetime. The Linux DISPCC
probe performs runtime resume before accessing its controller; that dependency
cannot be removed merely because the register was mapped.

The observer stores at most32 CPU reports, emits short lines, and reemits only
saved data. `ControllerBusHeld` and `DpuDomainClockHeld` stay false; logs explicitly
mark `required_unfinished=1` and `hardware_ready=0`. Coherent GCC values mean only
that both observations agreed. Successful partial work returns NotReady while
these required backends are unbound. Unattempted reads remain NotStarted with
specific skip reasons.

Every successful Begin attempts exact End. Warning/unknown cleanup, retained
handler or EBS requires Root halt and preserves resident callback code. Foreign
handlers are never replaced or removed. No MMIO write, clock/domain enabling,
cache operation, new mapping, DMA or native Map invocation is used. Root owns
single-product build/Core/NativeProbe wiring; no test image or disabled feature
profile is introduced.

## Exact source byte pins

Paths below are relative to the kernel source at the baseline commit above:

| Path | SHA256 |
| --- | --- |
| `drivers/clk/qcom/gcc-sm8750.c` | `3ac38ce713871b541dd7d4bee7007b14fee36d3a2fc6faa951e7c82fd53da960` |
| `drivers/clk/qcom/dispcc-sm8750.c` | `d33b53c94c12f6f30118dbb20f5c7714aa14ee3421cec436379425cfee2be164` |
| `drivers/clk/qcom/gdsc.c` | `04c66019948835d4b0def6f96d940500a85eb3e9c277f69de06c953f7d6475d2` |
| `drivers/clk/qcom/clk-branch.c` | `d29259eb77a5d1d6a07adf4aa835588f8e04d1fa3c14be2b7d4162cce737abfa` |
| `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_intf.c` | `bfba5fb9089248f07e9e43b3dbc5a5a9c791ac24426efa1c237e053b033d8e21` |
| `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_sspp.c` | `0d71a8c897aae490f63cfc8306a9bf62157a6b3288dbf07e7da06e3a74e45c22` |
| `drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_12_0_sm8750.h` | `253994b67361b047a930681205373929c5d067cfa928e999e5e2f40e9c15062e` |
| `arch/arm64/boot/dts/qcom/sm8750.dtsi` | `531d6a6d53c8e23b94e8ba2747386f3265c1930bfa24c8db4f8fd1c0086940e0` |

Key locations: GCC HF_AXI1402 and fixed AHB probe3238; DISPCC AHB979, MDP1573,
byte997/1033, pclk1645/1663, GDSCR1753 and runtime resume1912; `gdsc.c` status
selection61; `dpu_hw_intf.c` offsets49/75 and get_status326; SSPP address offset26;
catalog INTF391 and SSPP/VIG/DMA bases54 onward. Line numbers describe baseline
bytes, not a promise about a different source revision.

Run `python3 -m unittest discover -s tests -p test_display_clock_observe.py -v`.
The actual collector and actual Guard pass17 fork cases with ASAN/UBSAN, strict
AARCH64 and real `AsciiVSPrint(256)` complete lines under180bytes. Tests forbid
every non-GCC LDR, cover double-read drift/disable, GCD/AT refusals, zero-load
DISPCC qualification, foreign handlers, read-abort/SError/EBS, cleanup retention,
32-phase capacity, CPU-only replay and long status formatting. No device is used.
