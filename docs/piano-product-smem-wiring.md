# Shared product SMEM observation build wiring

The unique ProductCore build includes Root's `PianoProductSmem.c` and byte-identical
canonical `early-memory/PianoSmemRam.c/h` and `guarded-read/PianoGuardedRead.c/h`.
`prepare_product.py` copies both implementation families flat into ProductCore;
their ordinary quoted headers resolve from the actual module directory. The INF
lists both C sources and both headers, the wrapper source, the existing real CPU
Arch protocol and ExitBootServices GUID declarations. No separate profile or test
image is created.

Root calls the wrapper after native Foundation and before native RAM inventory
and USB/UFS startup. It performs a bounded guarded DXE observation and then
closes the guard. The collector's repeated metadata/payload comparisons are
observations, not an atomic snapshot or authority to map DDR. A refused mapping
is diagnostic; no attribute is changed to make the read succeed. Retained handler
or lifetime state is terminal and must not continue product initialization.

The prepared manifest records each family's original C/header SHA-256, its actual
INF source list, and `READ_ONLY_DXE_OBSERVATION_BOUND_UNTESTED`. It explicitly
reports `device_validated=false`, `sec_early_ready=false`,
`high_ddr_mapped=false` and `memory_ownership_granted=false`. The backend status
also marks the observation as bound but untested. This does not make a SEC-time
memory provider, a full-DDR contract, a 1 GiB arena, or DMA ownership ready.

`build_integrity.py` hashes the actual canonical family files and checks the
prepared manifest, generated platform and upstream compilation copies against
those bytes. It refuses missing or stale C/header copies, altered family identity
and omitted source/event/protocol declarations. A coherent reprepare of changed
canonical code changes the build input fingerprint. Existing OS-loader freshness
tests now provide real family fixtures rather than skipping this added dependency.

```sh
python3 -m unittest discover -s tests -p test_product_observation_wiring.py -v
python3 -m unittest discover -s tests -p test_product_os_boot_wiring.py -v
```

The new four tests stage the real files and compile the two implementations plus
the actual Root wrapper into AArch64 objects from the generated INF layout. They
exercise 20 family source/header/target/upstream/INF mutation refusals, coherent
reprepare for each family, and the unchanged readiness limits. They mock unrelated
pump/UI preparation only to isolate the product freshness boundary; they do not
mock the family copies, generated INF or compilation. The existing four OS-loader
tests continue to pass. The original guard and collector tests remain the actual
C lifecycle/parser failure tests. Complete product linking, tablet snapshots and
real exception/mapping behavior still require Root's build and physical validation.

## Snapshot retrieval through resident fastboot

Root stores its own immutable guard/SMEM reports before the temporary reader
session ends. The saved report is independent of the singleton guard's later
sessions. `PIANO_DWC3_SERVICE_CONFIG.BeforeRamlog` is bound to
`PianoProductSmemReemit`: the existing `oem ramlog` command re-emits these CPU
reports immediately before freezing the console. `get_staged` therefore retrieves
the bank/preloaded/CRC, GCD/PAR, cookie and fault records through the existing
fastboot interface. No new command or MMIO re-read is needed.

Unbound services preserve their old log path. The callback executes exactly once
per command at APP; an error/warning refuses a successful snapshot. Actual EBS
loss prevents snapshot allocation and FAIL DMA. Replay tests prove unchanged
BS/AT/GCD/load/handler counters and preservation of the saved report after another
real guarded-read session replaces its singleton diagnostics.

The actual product wrapper tests cover 13 integrated cases using the real
wrapper, guarded reader and SMEM parser. Component fixtures cover 57 parser /
collector cases and 41 guard cases. The Device/Controller tests add eight actual
callback-order/failure/copy-identity cases. These remain host boundary fixtures,
not physical memory, USB or early-SEC proof.

## Current host result

The complete product build and package succeeded at build ID
`3b357c5b-1614-4a19-af36-8bcfca7b3972`. The unique image is 28,864,512 bytes,
SHA256 `206e31cd5d24909cf102962a1b3c78fe13cda5665a53caab8817b6b3d001531e`.
Freshness validation and the RAM candidate check without `--execute` pass.
Status remains `INCOMPLETE_NOT_RELEASE`, with no physical boot or device write.

The final link map retains `PianoProductSmemReemit`, `PianoGuardedTryRead`,
`ServicesAlive` and the saved guard snapshot; the actual PE contains the SMEM
payload/cookie/snapshot diagnostic markers. LTO inlines Observe/Collect/Begin
into CoreEntry, so absence of those public names alone is not evidence of dead
code. Root's direct call, actual three-module tests and these linked callbacks
distinguish this binding from the still-unbound generic OS loader.

The full host suite passed 252 methods in
`build/logs/product-smem-final-tests.log`. The final product build log is
`build/logs/product-smem-ramlog-final-build.log`; exact private host evidence is
`private/analysis/product-smem-host-evidence.json`. First product RAM test92
remains pending an operator able to recover with the power key; no diagnostic
reboot timer is added to the product.
