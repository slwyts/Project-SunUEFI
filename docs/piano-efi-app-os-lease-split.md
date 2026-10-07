# Returning EFI application loans and OS retirement

The existing native fastboot boot path requires real4byte IN completion, queue
drain, DWC halt and9 DMA buffer retirements before TakeAfterAck. The product
owner manager then verifies USB/SMMU/clocks, fresh retired-peer proof, UFS and
input retirement. These proofs must remain unchanged for actual OS handoff.
`PianoFastbootLaunchRun` requires ShutdownAll; a dummy successful callback is
not a legitimate way to reuse that OS launcher while devices remain running.

A returning application can preserve shared USB/UFS through the existing
`PianoApplicationRunBuffer` lifecycle used by trusted UI apps. It handles fresh
LoadedImage identity, normal return/auto-unload and exact cleanup. Its EBS event
only detects services already lost; it does not prevent an arbitrary EFI image
from calling ExitBootServices. PE EFI_APPLICATION and the .efi extension also
include Linux EFI stubs. Thus generic returning-app admission needs a reviewed
application contract or a real pre-EBS owner gate. No new such policy, dispatcher
or EBS authorization is implemented here.

`uefi/components/os-boot/PianoCpuImageLoan.c` is the shared data-only component.
It accepts a PIANO_LAUNCH_BLOB whose owner has already been taken once. It loans
one stable CPU view with an atomic driver-lifetime monotonic token shared
across all session instances (no wrap or reuse), copies no full image and
creates no second ownership path. It can make bounded CPU reads and revalidate
SHA256 of the same view. Mutation or unknown Unborrow retains the source loan.
It never calls Take, Restore, ZeroRelease, LoadImage, StartImage, Stop or EBS.
End requires the caller to have retired all view consumers; it releases only the
borrow, not the owned source. It is not an image-unload or all-owner proof.

The actual DownloadBlob adapter already isolates taken memory from ordinary
fastboot reset/new download. Its ActiveLoan prevents source restore/free. Restore
is allowed only when the download state remains empty; a later background
download therefore makes restore refuse, and the original owner must be
released separately after its consumers have ended.

`python3 -m unittest discover -s tests/unit -p test_cpu_image_loan.py -v` passes actual
DownloadBlob/parser-state plus loan integration under ASan/UBSan and AArch64
compilation: one external Take, no loan image allocation, new-download/reset
survival, bounds/alias/stale-token rejection, mutation and warning retention.
Tests also reject cross-instance and reinitialized-state stale tokens, counter
exhaustion, and Begin output slots that alias state, source or each other.
Outputs are written on success only; a source-alias refusal unwinds the
unpublished borrow with exact Unborrow acknowledgement, retaining on unknown
cleanup. Linking this helper requires the standard SynchronizationLib for the
atomic64bit counter. The test explicitly leaves native ACK/DMA proof unset. It tests CPU memory,
not a wired USB boot command, returned application or OS launch.

Remaining integration: real accepted-command/ACK binding for an app intent,
APP dispatcher/consumer lifetime, safe EBS admission, and true OS owner-retire
lease. The current product generic .efi/.img backend is not made ready by this
library. Service, Root, immutable reader and Linux session code are unchanged.
