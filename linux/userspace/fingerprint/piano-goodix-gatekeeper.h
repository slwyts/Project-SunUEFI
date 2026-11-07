// SPDX-License-Identifier: BSD-3-Clause
#ifndef PIANO_GOODIX_GATEKEEPER_H
#define PIANO_GOODIX_GATEKEEPER_H

#include <stddef.h>
#include <stdint.h>

#define GOODIX_GATEKEEPER_HAT_BYTES 69U

/* The callback uses the native Keymaster service, not QSEECOMPAT. Its return
 * value is a transport/framing error; TA status and the complete reply are
 * separate outputs. Requests contain an opcode followed by exact-length CBOR.
 */
struct goodix_gatekeeper {
	int (*command)(void *context, const void *request, size_t request_bytes,
		       uint8_t *response, size_t capacity, size_t *actual, int32_t *status);
	void *context;
};

struct goodix_gatekeeper_status {
	int32_t value;
	uint32_t retry_ms;
};

/* This project reserves its high UID range explicitly; no Android/GSI user
 * number or password handle is reused. Linux UIDs above 0x0fffffff are rejected.
 */
int goodix_gatekeeper_uid(uint32_t linux_uid, uint32_t *hardware_uid);

/* Pass a private gatekeeper/ directory, separate from the Goodix GPFS tree.
 * The caller runs as root, owns that 0700 directory and supplies a correctly
 * initialized native Keymaster service. The API never creates parent paths.
 *
 * create is idempotent after a complete credential has been persisted. A
 * pending record retains its random credential across a provisioning failure;
 * a complete record is never silently re-enrolled or reset.
 */
int goodix_gatekeeper_create(const struct goodix_gatekeeper *gatekeeper,
			    int directory_fd, uint32_t linux_uid,
			    struct goodix_gatekeeper_status *status);

/* Verify the stored Linux credential against Goodix's PRE_ENROLL challenge.
 * Success returns the signed 69-byte HAT without printing or persisting it.
 * Positive TA retry status returns -EAGAIN with retry_ms; no automatic retry.
 */
int goodix_gatekeeper_verify(const struct goodix_gatekeeper *gatekeeper,
			    int directory_fd, uint32_t linux_uid, uint64_t challenge,
			    uint8_t hat[GOODIX_GATEKEEPER_HAT_BYTES],
			    struct goodix_gatekeeper_status *status);

#endif
