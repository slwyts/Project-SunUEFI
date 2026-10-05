# Product-owned Stable/Next Linux controller

`PianoProductOsController.c/.h` provides one persistent parent-APP loader for
`\EFI\Piano\stable\Image.efi` or `\EFI\Piano\next\Image.efi`, the corresponding
pinned DTB and one `\EFI\Piano\shared\initramfs.cpio`. The controller owns its
actual FileSource objects, CPU source snapshots and Linux EFI session. It does
not create another firmware profile, reset the machine or retire owners before
preflight. This module is not yet bound to the product Core/Policy callsite.

Root supplies the actual boot-services/EBS fence, runtime instance, owners,
compiled candidate hashes/sizes, memory/buffer validators and native late-exit
provider. `ValidateSelection` must inspect the current trusted Policy selector,
sequence and owner request; it cannot accept an arbitrary permission bool. The
controller performs fresh runtime protocol lookup and actual APP-TPL checking.
It invokes that selection validation again immediately inside its wrapped
NativeLateArm, before calling Root's real native provider.

Large-source `PianoCpuInputAuthorize` runs before filesystem lookup or any file
read. With the current unproved full-DDR contract it returns NOT_READY and
leaves USB/UFS/input/policy alive. NativeLate mode is mandatory. The successful
CPU source reader path calls actual `PianoBootFileLoadBundle` and
`PianoLinuxEfiSessionRun`; there is no raw jump or Retire-then-not-ready path.

`ApprovedVolume` must validate the actual product GPT reservation and immutable
volume UUID/state before returning the exact published FAT BlockIO. The
controller observes only registered SFS handles, requires one exact BlockIO
identity and the current product Vendor DevicePath GUID/UUID plus end node.
It opens no other filesystem. Per-file CPU slices revalidate approved volume,
SFS and selection while pumping the same Root service. A partition-created child
with a different BlockIO/path is refused rather than guessed as approved.

File-specific SFS/selection checks stop after the complete immutable snapshots
are loaded and closed. Session and cleanup then use Root's CPU-only real
boot-services fence, which must remain independent of the UI runtime Alive flag
and UFS state. Native retirement can close the runtime while actual BS remains
valid. Before/EBS uncertainty retains the persistent session/resources and
forbids reuse. The controller's own EBS event is driver-lifetime storage.

Ordinary retries are allowed only after an explicit check that every prior
image, FDT/options buffer, LoadFile2 handle, event, source owner/loan/view,
FileSource allocation/file/root and retained flag is absent. It never clears
unknown or retired state. Environment/context aliasing with the controller is
refused. A missing file or memory proof returns an observable status to Root;
Root must restore its actual policy/owner request state before resuming UI,
without manufacturing a USB ACK or silently reusing an old sequence.

`tests/test_product_os_controller.py` compiles the actual controller and CPU
layer with ASAN/UBSAN for six preflight scenarios. A second actual joint fixture
links the real FileSource, Linux EFI session, CPU layer, AA64 PE parser and
libfdt: ordinary native-late return/Disarm, preflight NOT_READY, Before/EBS
fail-stop, hash mismatch, selection changed just before Arm, exact clean retry
and retained refusal. EFI/SFS and Root hardware-validator callbacks are fixtures;
no ARM OS executes, and no hardware readiness is inferred. AArch64 strict
syntax is checked independently.

Remaining product binding is owned by Root: actual Stable/Next UI request
selector, host-vs-UI request precedence, not-ready request cancellation,
approved volume adapter, live full-DDR/buffer registry and Core/prepare source
inclusion. Existing Android GPT has no permanently provisioned product volume,
so this source does not claim files are available from UFS. The bounded product
FAT also cannot contain the current approximately900MiB common bootstrap; an
explicit separately approved larger source/volume is needed before autonomous
UFS loading is possible. No original Android userdata is mounted or written.
