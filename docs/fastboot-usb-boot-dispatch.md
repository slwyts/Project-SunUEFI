# Standard boot request / real IN-ack transfer point

`PIANO_USB_RAM_BOOT` defaults to 0 in `PianoFastboot.h` and must be enabled for the command layer and Dwc3 together. This step changes only Fastboot/Dwc sources and host tests; Controller/profile/Launch/DownloadBlob integration is root-owned. Existing profiles still reply `FAILRAM boot handoff not implemented in debug v1`; the download advertisement remains 64 MiB.

The copied `PIANO_FB_BOOT` backend requires an explicit MaxImageBytes, Ready, **Validate** and TakeAfterAck. Context is copied and remains caller-owned. Validate is caller policy (the first-profile probe SHA allowlist, for example). `PianoFastbootSetBoot` and `PianoDwc3SetBootForExperiment` copy the callbacks; the caller retains Context for the entire run and any retained result. Missing backend/Ready/Validate, incomplete or non-exclusive download, invalid PE/container, unsupported ramdisk/second/DTBO/DTB, disallowed v4 quirk or budget failure replies FAIL before an OKAY is queued.

With the gate on, the command layer uses the actual Boot parser and accepts raw AA64 PE or its supported Android wrapper kernel view. Validation runs while BootPreparing blocks command/reset mutation, then records download pointer/size and immutable BootView. Send exactsuccess only sets BootPending: it is not a host ACK. Pending dispatch cannot queue a second command or replace staged RAM. A USB reset/disconnect before acknowledgement cancels and zero/releases ordinary source by the original cleanup path.

Only a successful DWC EP3 completion with HWO clear, residual 0 and the final exact 4-byte OKAY frame observes the ACK. BulkPump then posts no new command OUT. Once the queue is empty and pending IN is absent, the loop freezes dispatch and reset destruction, stops the core, retires and frees all 9 shared-DMA buffers, then calls Validate **again** before TakeAfterAck. Its `Source->BootProof` must show:

| Evidence | Required value |
| --- | --- |
| AckCompleted / QueueEmpty | TRUE / TRUE |
| DeviceHalted / DmaFreed | TRUE / TRUE |
| DispatchFrozen | TRUE |
| AckBytes / DmaBuffersFreed | 4 / 9 |

TakeAfterAck only moves exclusive CPU-pool ownership to persistent storage; it must not call LoadImage/StartImage. Exact success, a non-NULL token and a fully emptied Download/borrowedUpload/Expected/Received/Receiving/Complete state publish Taken=true/Status=Success/Retained=false. Internal clear then deliberately unfreezes the **empty** source before Reset. Every unsuccessful, partial or ambiguous take preserves source/context, blocks rebind/re-run and reports Retained. A callback token plus source changes on error is never treated as a complete transfer. RAM_BOOT Halt failure records state and enters CpuDeadLoop with DMA ownership retained. DMA retirement warnings are rejected in EP0 completion/stop, bulk completion/asynchronous END and final cleanup; they do not generate a true proof.

`PianoDwc3ConsumeBootAction(Action*)` returns EFI_NOT_FOUND with zeroed output when no result exists; otherwise it returns the copied Action.Status and consumes one result. Action contains Context/Token/View/Proof/Taken/Retained. Its status describes Device transfer. The later caller must check the Controller report and all-owner typed shutdown before launch. A failed action may carry a retained partial token; no consumer should discard it or start an image.

The original download adapter cannot be Take-called twice. Root's carrier handles the second ownership handoff: DWC callback binds/takes the underlying pool, records its token, then Launch's Blob.Take transfers that captured token once. Restore into a stopped DWC is not a restart; first-profile failure policy is ZeroRelease. These carrier/Controller/App/profile changes are root-owned and outside this step.

`tools/test_usb_boot_request.c` compiles actual Fastboot + Boot parser + Dwc3 source. It covers real download→boot event-loop, successful Send without ACK, missing completion timeout, backend/Ready/Validate/budget/unsupported gates, wrapper extraction and ramdisk rejection, copied callbacks, policy recheck, exact4-byte IN completion, Halt+9free before transfer, reset/dispatch freeze, post-clear ownership, reset-before-ACK cancellation, partial/NULL/error take, hardwareHalt failstop and DMA warning retention. It does not execute PE APIs or access devices.

```sh
bash tools/test_usb_fastboot.sh
```

New boot-request tests pass ASan/UBSan/leak; prior default-off suite still passes. Gate1 and gate0 AArch64 freestanding syntax checks pass. The first device activation must be a separate, explicitly enabled root-built return-probe profile; no hardware boot result is claimed by these host tests.
