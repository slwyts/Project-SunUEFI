// SPDX-License-Identifier: BSD-3-Clause
#ifndef PIANO_GOODIX_STORAGE_H
#define PIANO_GOODIX_STORAGE_H

struct goodix_storage;

/* Diagnostic FS/GPFS services: empty, explicit Linux directory; no TA identity. */
int goodix_storage_open(const char *directory, struct goodix_storage **storage);

/* Persistent Linux fpdata only: existing files may be reopened and changed.
 * Keep Gatekeeper credentials in a separate directory, outside this service.
 * gatekeeper adds the scoped RPMB listener for enrollment; it remains closed
 * except inside the selected Linux user's create/verify command callback.
 */
int goodix_storage_open_profile(const char *directory, int gatekeeper,
			       struct goodix_storage **storage);

/* Explicit diagnostic only: log RPMB request headers and reject every command.
 * No storage device, RPMB frame or filesystem directory is accessed.
 */
int goodix_storage_open_rpmb_probe(struct goodix_storage **storage);
/* Standalone backend for the existing Gatekeeper diagnostic. Enrollment uses
 * the same backend alongside FS/GPFS in its ordinary profile object.
 */
int goodix_storage_open_rpmb_ufs(struct goodix_storage **storage);
void goodix_storage_allow_rpmb(struct goodix_storage *storage, int allow);
unsigned int goodix_storage_rpmb_probe_requests(struct goodix_storage *storage);
void goodix_storage_close(struct goodix_storage *storage);

#endif
