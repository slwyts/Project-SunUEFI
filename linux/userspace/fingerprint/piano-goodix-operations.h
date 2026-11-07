// SPDX-License-Identifier: BSD-3-Clause
#ifndef PIANO_GOODIX_OPERATIONS_H
#define PIANO_GOODIX_OPERATIONS_H

#include "piano-goodix-events.h"

#define GOODIX_OPERATION_HAT_BYTES 69U
#define GOODIX_OPERATION_MAX_TEMPLATES 10U

enum goodix_operation_step_result {
	GOODIX_OPERATION_WAIT_IRQ,
	GOODIX_OPERATION_CONTINUE,
	GOODIX_OPERATION_COMPLETE,
};

enum goodix_operation_sample_kind {
	GOODIX_OPERATION_SAMPLE_NONE,
	GOODIX_OPERATION_ACQUIRED,
	GOODIX_OPERATION_QUALITY_RETRY,
	GOODIX_OPERATION_NOT_MATCH,
	GOODIX_OPERATION_ENROLL_PROGRESS,
	GOODIX_OPERATION_TEMPLATE_SAVED,
	GOODIX_OPERATION_ENROLLED,
	GOODIX_OPERATION_AUTHENTICATED,
	GOODIX_OPERATION_CANCELLED,
};

enum goodix_enrollment_packet {
	GOODIX_ENROLL_PACKET_NORMAL,
	GOODIX_ENROLL_PACKET_EXTENDED,
};

struct goodix_operation_sample {
	enum goodix_operation_sample_kind kind;
	uint32_t mask, group, template_id;
	int32_t application_status;
	uint16_t remaining;
	uint8_t hat[GOODIX_OPERATION_HAT_BYTES];
};

/* Allocate as zeroed state. The application serializes commands and supplies
 * the shared memory through events; no worker, poll loop or logging is hidden
 * in this module. HAT and identifiers are backend-private, not log messages.
 */
struct goodix_operation {
	struct goodix_events *events;
	unsigned int stage, kind;
	uint32_t group, template_id;
	uint32_t result_mask, result_operation;
	uint64_t sid, operation_id;
	uint32_t allowed_templates[GOODIX_OPERATION_MAX_TEMPLATES];
	size_t allowed_count;
	uint16_t remaining;
	uint8_t hat[GOODIX_OPERATION_HAT_BYTES];
	uint8_t interaction_policy;
	int cancelled;
	int saved;
};

/* First call goodix_events_start(), before asking for a challenge. It sends
 * the real cancel once; do not cancel again between PRE_ENROLL and ENROLL.
 * Each of challenge/step performs at most one synchronous command.
 */
int goodix_operation_challenge(struct goodix_events *events, uint64_t *challenge);
int goodix_operation_post_enroll(struct goodix_events *events);
int goodix_operation_start_enroll(struct goodix_operation *operation,
				  struct goodix_events *events, uint32_t group,
				  const uint8_t hat[GOODIX_OPERATION_HAT_BYTES],
				  enum goodix_enrollment_packet packet);
int goodix_operation_start_authenticate(struct goodix_operation *operation,
					struct goodix_events *events, uint32_t group,
					uint64_t sid, uint64_t operation_id,
					const uint32_t *allowed_templates, size_t count,
					uint8_t interaction_policy);

/* Call initially, then after a consumed UIO interrupt, and while CONTINUE.
 * WAIT_IRQ permits re-enabling UIO; COMPLETE is emitted only after save/finish
 * and post-enroll commands succeed. The caller checks its deadline/signals
 * between steps, and clears each sample's HAT after consuming it. A progress
 * sample with remaining==0 precedes command 1012, permitting a pending profile
 * record; TEMPLATE_SAVED is its durable commit, before post-enroll teardown.
 */
int goodix_operation_step(struct goodix_operation *operation,
			  struct goodix_operation_sample *sample);

/* Request teardown; continue step until CANCELLED/COMPLETE. Cancelling never
 * deletes a template or rolls back a template already saved by command 1012.
 * After that commit, it finishes post-enroll and returns ENROLLED as usual.
 */
void goodix_operation_cancel(struct goodix_operation *operation);

#endif
