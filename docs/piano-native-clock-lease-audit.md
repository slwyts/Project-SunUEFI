# Pinned ClockDxe display bus reference audit

This is a read-only audit of the actually captured native PE and existing
product adapters. It identifies usable reference operations and their limits;
it does not call a clock protocol, change any controller code, obtain a device
lease or prove DISPCC access. Build100 remains limited to the reviewed cold
MMIO mappings and GCC reads, with no newly added native Enable invocation.

## Exact binary and protocol ABI

The Bringup and ProductFoundation ClockDxe.efi files are byte-identical,
278,528 bytes, SHA256
`f9e85aa758932b4366c55ec83b58e0176f58fba2944cc2fdd34875a46fdb769e`.
The file has ImageBase0 and equal raw/RVA section starts: .text1000,
.data28000 and .reloc41000. Addresses below are RVAs in that pinned binary,
not addresses to read/write on the device.

The protocol table is at28148, Version1000b. The repository EFIClock header's
version macro is older10009; existing UFS/USB adapters deliberately verify1000b
and the captured alias callbacks. The layout prefix itself is the ordinary
EFI_CLOCK_PROTOCOL at `QcomPkg/Include/Protocol/EFIClock.h:1073-1088`.

| Interface | Actual table target → implementation | Support |
| --- | --- | --- |
| GetClockID |156c →5a60|Real typed name lookup|
| EnableClock |15ac →8e58|Real typed dispatch and references|
| DisableClock |15f0 →8eb0|Real typed dispatch and release|
| IsClockEnabled |1634 →8f34 →7338 for type1|Real hardware query, BOOLEAN output|
| IsClockOn |1674 →8f64 →7444 for type1|Real on query, BOOLEAN output|
| GetClockPowerDomainID |156c|Exact GetClockID alias|
| EnableClockPowerDomain |15ac|Exact EnableClock alias|
| DisableClockPowerDomain |15f0|Exact DisableClock alias|
| EnterLowPowerMode |180c|EFI_UNSUPPORTED stub|
| ExitLowPowerMode |1814|EFI_UNSUPPORTED stub|

Each wrapper loads the native global driver client from3f5f0 rather than using
the incoming protocol pointer as a distinct per-EFI-caller reference client.
Its DAL result is converted through the12-entry EFI-status table at11fc8.
For example DAL-7 maps to8000000000000003, EFI_UNSUPPORTED. A header declaration
or a non-NULL method is not proof that every typed resource is supported.

## Verified typed ID encoding

For the two resource kinds needed here, the name lookup actually constructs:

```text
ID = (provider_index << 24) | (resource_kind << 16) | resource_index
resource_index = ID & 0xffff
resource_kind = (ID >> 16) & 0xff
provider_index = (ID >> 24) & 0xff
```

The emitted ID is zero-extended32 bits inside the64-bit UINTN output. Kind1 is
a local clock; kind3 is a local power domain. Kind2 is a separate source path,
not a GDSC. Other kinds exist in the generic dispatcher but were not promoted
to display lease support by this audit.

Concrete source instructions establish this encoding:

- Clock finder5204 iterates provider/module index w23 and clock index w25;
  at52f0 it ORs `w25, w23 lsl24`,52f8 zero-extends to32 bits,52fc ORs10000,
  and5308 stores the64-bit output. Clock nodes have70-byte hex stride112.
- Power-domain finder556c searches the provider's pointer array;5654 forms
  the same module/index combination,565c zero-extends,5664 ORs30000,5670
  stores the output.
- Clock decoder5b00 validates bits16..23 equal1,5b10 extracts bits24..31,
 5b20 masks the low16 index,5b24-5b2c bounds it against the chosen provider's
  count and5b38 derives its clock-node address.
- Power-domain decoder7fe8 validates kind3,7ff8 extracts provider bits,
 8008 masks the low16 index,800c-8014 checks the domain count and801c
  selects the domain pointer.

Do not derive a live ID merely from a static string/node address or truncate an
arbitrary64-bit input. Root must call the actual pinned getter, verify a newly
written output of the expected kind, and retain that exact ID in its ledger.

## Actual display names in the resource tables

These are real table references, not merely messages containing a name:

| Resource name | Name RVA | Data node/reference RVA |
| --- | --- | --- |
|gcc_disp_ahb_clk|14e02|33a68 clock node|
|disp_cc_mdss_ahb_clk|13b39|2e4b0 clock node|
|disp_cc_mdss_mdp_clk|13f8f|2f550 clock node|
|disp_cc_mdss_core_gdsc|1442c|30200 power-domain node|
|disp_cc_mdss_core_int2_gdsc|14443|Second domain in the same resource table|

The exact name `mdss_gdsc` is absent. The tables also contain
`disp_cc_mdss_non_gdsc_ahb_clk`, AHB1/RSCC and other clocks. Presence in the PE
proves the local resource description exists; live provider initialization and
GetID success still need verification. Which minimal parent/bus chain suffices
for safe DISPCC reads has not been tested here.

## Reference updates and success-without-ownership paths

Kind1 Enable enters6404. It verifies the resource/client relationship under
the native lock, then64f4-6508 increments two16-bit counters: clock-node+50
and the matched native driver-client reference+10. The alternate vote class
uses node+52/client+12 at6510-6524. Disable enters5320, checks the client has a
reference, and5414-542c selects the matching total counter. Only the last total
reference calls the HAL disable at5438; later code decrements the ledger.
This is an actual software reference mechanism, not an ON-bit observation.

Kind3 Enable enters802c, decodes the power-domain ID through7fe8, and calls9548
under the same lock. That routine also updates node/client references around
96f4-9718. Kind3 Disable enters5684 and calls9bc8. Existing product UFS/USB code
already uses these exact alias callbacks with a held-count/domain ledger and
reverse cleanup; reuse that mechanism rather than introducing a guessed vote.

Important limits in the actual binary:

- The global driver-state pointer at3fe48 has byte+2d bit3 skip branches in
  Get, Enable and query paths. For example5a7c-5a80 reaches5adc, returning
  success without storing the GetID output;8e60-8e64 reaches8e98, returning
  success without the normal Enable/ref path;8f6c-8f70 similarly skips query
  output. Initialize output sentinels and validate state/identity; success
  alone cannot establish a lease. The live skip flag is unknown in this audit.
- Type1 Enable's HAL-on poll at65d8-65fc retries about300 times. The exhausted
  path logs at6608-6628, then still branches to the reference increment at64f0.
  Therefore a counted exact-success Enable is not itself proof the bus is on.
- IsClockOn writes one byte at74f4/7528, matching BOOLEAN*. IsClockEnabled
  similarly uses a byte store. Initialize FALSE, require exact success and
  an actual TRUE result for the clock being checked.
- IsClockOn accepts kind1/2 only at8f74-8f98. A kind3 GDSC ID returns DAL-7 /
  EFI_UNSUPPORTED. IsClockEnabled accepts only kind1. These methods cannot
  serve as an unsupported power-domain readback API.
- In some parent-source states, IsClockOn7444 makes a temporary source request
  at74d8 throughb73c, reads the HAL state throughf98c, then requests zero at7510.
  This query is not universally side-effect-free. Its dependency and cleanup
  path must be accounted for even when the caller only wants a status bit.

Because EFI callers share the native driver client, Root needs its own
persistent exact-once ledger. A successful native Enable increments one real
reference; a matching successful Disable releases that acquired reference.
Do not claim the entire hardware clock became off on release: another native
consumer may still hold it. Errors/warnings or uncertain native mutation retain
state, code and ledger; they cannot be converted to clean release.

## DEPEX and runtime dependencies

The original ClockDxe DEPEX has three GUID pushes followed by AND/AND/END:

- ae37b942-457f-4c91-a196-d9669fd347a3;
- b0760469-970c-487a-a4b5-28db7b45cef1, ChipInfo;
- 1c34f691-d33d-4e14-b8f8-32c6c029e95b.

Its captured DEPEX SHA256 is
`c8001e9cea134cf8205ad4167fd0ff3b8dab6ecf325c723158cc6916e4a9b1cf`.
NativeProbe.c preserves original DEPEX and separately checks ChipInfo before
QcomScmiDxe. The current prepared UFS foundation group includes CmdDb,
PwrUtils, Rpmh, Npa, Vcs and Clock, but does not include Mailbox/QcomScmi.
Do not assume those latter runtime providers exist because Clock was started.

The PE contains concrete SCMI LocateProtocol failure text at24eda and NPA
missing-protocol text at26546. Enable may require its parent/source path through
60e8; status queries can also issue temporary parent votes. Rpmh/NPA/VCS and
possible SCMI initialization/transport requirements must be verified against
the selected live resource path. Their presence in source/staging or DEPEX
satisfaction does not prove runtime operation. This audit did not run those
providers or infer that all display resources require exactly the same subset.

## Narrow next owner and unresolved guarantee

A future Root-owned candidate can first acquire a real `gcc_disp_ahb_clk`
reference after verified Clock/framework initialization, then evaluate the
actual DISPCC AHB parent path. It must use fresh pinned LoadedImage/protocol/
Version/method identity, exact typed ID, its own held ledger, actual clock-on
result and protected mapping/register readback. Register mapping is a separate
cold source change; no native call is safe merely because a name exists.

Do not unconditionally enable MDP/core GDSC, change rates, reset controllers or
use unsupported low-power/query stubs to obtain a bus lease. Native refcounting
can protect against releases that follow that same driver mechanism, but this
static audit cannot prove the display framework avoids direct gate/GDSC writes.
Thus real references are implementable; a guarantee of continued safe DISPCC
access remains unproved until the selected runtime path is observed. No ON bit,
EFI_SUCCESS callback or invented ownership boolean replaces those checks.
