// SPDX-License-Identifier: BSD-3-Clause
/* Goodix operation packets follow the current GF3656ZSCB factory HAL. The
 * event module owns all 1016 capture/retry exchanges for both diagnostics and
 * real operations; this module owns command framing, identity and completion.
 */
#include <endian.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "piano-goodix-operations.h"

#define IMAGE_RESULT (1U << 7)

enum operation_kind { ENROLL = 1, AUTHENTICATE };
enum operation_stage {
	BEGIN = 1, EVENTS, SAVE, POST_ENROLL, UPDATE_CONFIG, FINISH_AUTHENTICATION,
	CANCEL, CANCEL_POST_ENROLL, DONE, FAILED,
};

static void clear_private(void *memory, size_t bytes)
{
	volatile uint8_t *position = memory;

	while (bytes--)
		*position++ = 0;
}

static uint32_t word(const uint8_t *buffer, size_t offset)
{
	uint32_t result;

	memcpy(&result, buffer + offset, sizeof(result));
	return result;
}

static uint64_t wide(const uint8_t *buffer, size_t offset)
{
	uint64_t result;

	memcpy(&result, buffer + offset, sizeof(result));
	return result;
}

static void put_word(uint8_t *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

static int command(struct goodix_events *events, uint32_t number, size_t bytes)
{
	int32_t status = 0;
	int result = events->command(events->context, number, bytes, &status);

	if (!result && status)
		fprintf(stderr, "Goodix command %u failed: application=%d\n", number, status);
	return result ? result : status ? -EREMOTEIO : 0;
}

int goodix_operation_challenge(struct goodix_events *events, uint64_t *challenge)
{
	*challenge = 0;
	if (events->state != GOODIX_EVENTS_WAIT_IRQ ||
	    events->purpose != GOODIX_EVENTS_METADATA)
		return -EINVAL;
	events->enrollment_challenge = 0;
	memset(events->buffer, 0, events->buffer_size);
	int result = command(events, 1006, 0x78);

	if (result)
		return result;
	events->enrollment_challenge = wide(events->buffer, 0x70);
	if (!events->enrollment_challenge)
		return -EBADMSG;
	*challenge = events->enrollment_challenge;
	return 0;
}

int goodix_operation_post_enroll(struct goodix_events *events)
{
	memset(events->buffer, 0, events->buffer_size);
	int result = command(events, 1008, 0x6c);

	if (!result) {
		events->enrollment_challenge = 0;
		events->purpose = GOODIX_EVENTS_METADATA;
		events->state = GOODIX_EVENTS_IDLE;
	}
	return result;
}

static int validate_start(struct goodix_operation *operation,
			  struct goodix_events *events, uint32_t group)
{
	if (operation->stage)
		return -EBUSY;
	if (events->state != GOODIX_EVENTS_WAIT_IRQ ||
	    events->purpose != GOODIX_EVENTS_METADATA)
		return -EINVAL;
	/* These commands must never select Android's groups, even if a caller
	 * accidentally passes a Linux UID instead of its explicit hardware UID.
	 */
	if (group < 0x40000000U || group > 0x4fffffffU)
		return -EACCES;
	return 0;
}

int goodix_operation_start_enroll(struct goodix_operation *operation,
				  struct goodix_events *events, uint32_t group,
				  const uint8_t hat[GOODIX_OPERATION_HAT_BYTES],
				  enum goodix_enrollment_packet packet)
{
	int result = validate_start(operation, events, group);

	if (result)
		return result;
	if (packet != GOODIX_ENROLL_PACKET_NORMAL && packet != GOODIX_ENROLL_PACKET_EXTENDED)
		return -EINVAL;
	if (packet == GOODIX_ENROLL_PACKET_EXTENDED &&
	    events->buffer_size < GOODIX_ENROLL_EVENT_BUFFER_BYTES)
		return -EMSGSIZE;
	if (hat[0] || !events->enrollment_challenge ||
	    wide(hat, 1) != events->enrollment_challenge || !wide(hat, 9) ||
	    word(hat, 25) != htobe32(1))
		return -EACCES;
	operation->events = events;
	operation->kind = ENROLL;
	operation->stage = BEGIN;
	operation->group = group;
	operation->sid = wide(hat, 9);
	memcpy(operation->hat, hat, sizeof(operation->hat));
	events->purpose = GOODIX_EVENTS_ENROLL;
	events->enrollment_extended = packet == GOODIX_ENROLL_PACKET_EXTENDED;
	return 0;
}

int goodix_operation_start_authenticate(struct goodix_operation *operation,
					struct goodix_events *events, uint32_t group,
					uint64_t sid, uint64_t operation_id,
					const uint32_t *allowed_templates, size_t count,
					uint8_t interaction_policy)
{
	int result = validate_start(operation, events, group);

	if (result)
		return result;
	if (!sid || !count || count > GOODIX_OPERATION_MAX_TEMPLATES)
		return -EINVAL;
	for (size_t i = 0; i < count; ++i)
		if (!allowed_templates[i])
			return -EINVAL;
	operation->events = events;
	operation->kind = AUTHENTICATE;
	operation->stage = BEGIN;
	operation->group = group;
	operation->sid = sid;
	operation->operation_id = operation_id;
	operation->interaction_policy = interaction_policy;
	memcpy(operation->allowed_templates, allowed_templates, count * sizeof(*allowed_templates));
	operation->allowed_count = count;
	events->purpose = GOODIX_EVENTS_AUTHENTICATE;
	return 0;
}

static int begin(struct goodix_operation *operation)
{
	struct goodix_events *events = operation->events;
	uint8_t *buffer = events->buffer;

	memset(buffer, 0, events->buffer_size);
	put_word(buffer, 0x6c, operation->group);
	int result;

	if (operation->kind == ENROLL) {
		/* HAL 0x21684..0x216e4: reserved +0x70 remains zero. +0x74
		 * enables optional debug dumping only; neither field is a timeout.
		 */
		memcpy(buffer + 0x76, operation->hat, sizeof(operation->hat));
		result = command(events, 1007, 0xbc);
		clear_private(operation->hat, sizeof(operation->hat));
	} else {
		memcpy(buffer + 0x70, &operation->operation_id, sizeof(operation->operation_id));
		put_word(buffer, 0x78, UINT32_MAX);
		result = command(events, 1010, 0x80);
	}
	if (!result)
		operation->stage = EVENTS;
	return result;
}

static int authentication_result(struct goodix_operation *operation, uint32_t template_id)
{
	const uint8_t *hat = operation->events->buffer + 0x2ac;

	if (hat[0] || wide(hat, 1) != operation->operation_id ||
	    wide(hat, 9) != operation->sid || word(hat, 25) != htobe32(2))
		return -EACCES;
	for (size_t i = 0; i < operation->allowed_count; ++i)
		if (operation->allowed_templates[i] == template_id) {
			memcpy(operation->hat, hat, sizeof(operation->hat));
			return 1;
		}
	/* The TA can recognize another finger in this Linux group. A valid
	 * result outside the request's allowed IDs is a normal negative match.
	 */
	return 0;
}

static int read_event(struct goodix_operation *operation,
		       struct goodix_operation_sample *sample)
{
	struct goodix_events *events = operation->events;
	struct goodix_sensor_event event;
	int result = goodix_events_read(events, &event);

	if (result < 0)
		return result;
	sample->mask = event.mask;
	sample->application_status = event.application_status;
	if (event.mask)
		sample->kind = GOODIX_OPERATION_ACQUIRED;
	if (event.outcome == GOODIX_EVENT_QUALITY_RETRY)
		sample->kind = GOODIX_OPERATION_QUALITY_RETRY;
	else if (event.outcome == GOODIX_EVENT_NOT_MATCH)
		sample->kind = GOODIX_OPERATION_NOT_MATCH;
	if (events->state != GOODIX_EVENTS_RESULT) {
		if ((event.mask & IMAGE_RESULT) && !event.application_status)
			return -EPROTO; /* A different operation's result is never ours. */
		return result;
	}
	const uint8_t *buffer = events->buffer;
	uint32_t group = word(buffer, 0x80);
	uint32_t template_id = word(buffer, 0x84);

	operation->result_mask = event.mask;
	operation->result_operation = event.operation;
	if (operation->kind == ENROLL) {
		memcpy(&operation->remaining, buffer + 0x88, sizeof(operation->remaining));
		sample->kind = GOODIX_OPERATION_ENROLL_PROGRESS;
		sample->remaining = operation->remaining;
		if (operation->remaining)
			return goodix_events_resume(events);
		if (group != operation->group || !template_id)
			return -EACCES;
		operation->template_id = template_id;
		sample->group = group;
		sample->template_id = template_id;
		operation->stage = SAVE;
	} else {
		if (group != operation->group || !template_id)
			return -EACCES;
		result = authentication_result(operation, template_id);
		if (result < 0)
			return result;
		if (!result) {
			sample->kind = GOODIX_OPERATION_NOT_MATCH;
			return GOODIX_OPERATION_CONTINUE;
		}
		operation->template_id = template_id;
		operation->stage = UPDATE_CONFIG;
	}
	return GOODIX_OPERATION_CONTINUE;
}

static void completed(struct goodix_operation *operation,
		      struct goodix_operation_sample *sample,
		      enum goodix_operation_sample_kind kind)
{
	sample->kind = kind;
	if (kind != GOODIX_OPERATION_CANCELLED) {
		sample->group = operation->group;
		sample->template_id = operation->template_id;
		if (kind == GOODIX_OPERATION_AUTHENTICATED)
			memcpy(sample->hat, operation->hat, sizeof(sample->hat));
	}
	clear_private(operation->hat, sizeof(operation->hat));
	operation->events->purpose = GOODIX_EVENTS_METADATA;
	operation->events->state = GOODIX_EVENTS_IDLE;
	operation->stage = DONE;
}

int goodix_operation_step(struct goodix_operation *operation,
			  struct goodix_operation_sample *sample)
{
	memset(sample, 0, sizeof(*sample));
	if (!operation->stage || operation->stage == DONE)
		return -EINVAL;
	if (operation->cancelled && (!operation->saved || operation->stage == FAILED) &&
	    operation->stage != CANCEL && operation->stage != CANCEL_POST_ENROLL)
		operation->stage = CANCEL;
	if (operation->stage == FAILED)
		return -ECANCELED;
	struct goodix_events *events = operation->events;
	uint8_t *buffer = events->buffer;
	int result;

	if (operation->stage == BEGIN) {
		result = begin(operation);
		if (!result)
			return GOODIX_OPERATION_WAIT_IRQ;
	} else if (operation->stage == EVENTS) {
		result = read_event(operation, sample);
		if (result >= 0)
			return result;
	} else if (operation->stage == CANCEL) {
		result = goodix_events_cancel(events);
		if (!result && operation->kind == ENROLL) {
			operation->stage = CANCEL_POST_ENROLL;
			return GOODIX_OPERATION_CONTINUE;
		}
		if (!result) {
			completed(operation, sample, GOODIX_OPERATION_CANCELLED);
			return GOODIX_OPERATION_COMPLETE;
		}
	} else if (operation->stage == POST_ENROLL || operation->stage == CANCEL_POST_ENROLL) {
		result = goodix_operation_post_enroll(events);
		if (!result) {
			completed(operation, sample, operation->saved ?
				GOODIX_OPERATION_ENROLLED : GOODIX_OPERATION_CANCELLED);
			return GOODIX_OPERATION_COMPLETE;
		}
	} else {
		memset(buffer, 0, events->buffer_size);
		if (operation->stage == SAVE) {
			put_word(buffer, 0x6c, operation->group);
			put_word(buffer, 0x70, operation->template_id);
			result = command(events, 1012, 0x78);
			if (!result) {
				operation->saved = 1;
				operation->stage = POST_ENROLL;
				sample->kind = GOODIX_OPERATION_TEMPLATE_SAVED;
				sample->group = operation->group;
				sample->template_id = operation->template_id;
				return GOODIX_OPERATION_CONTINUE;
			}
		} else if (operation->stage == UPDATE_CONFIG) {
			buffer[0x6c] = operation->interaction_policy;
			result = command(events, 1109, 0xc0);
			if (!result) {
				operation->stage = FINISH_AUTHENTICATION;
				return GOODIX_OPERATION_CONTINUE;
			}
		} else if (operation->stage == FINISH_AUTHENTICATION) {
			/* 0x428fc..0x42920 swaps event mask/operation into this order. */
			put_word(buffer, 0x70, operation->result_operation);
			put_word(buffer, 0x74, operation->result_mask);
			put_word(buffer, 0x78, operation->group);
			put_word(buffer, 0x7c, operation->template_id);
			result = command(events, 1086, 0x148);
			if (!result) {
				completed(operation, sample, GOODIX_OPERATION_AUTHENTICATED);
				return GOODIX_OPERATION_COMPLETE;
			}
		} else {
			result = -EINVAL;
		}
	}
	operation->stage = FAILED;
	clear_private(operation->hat, sizeof(operation->hat));
	return result;
}

void goodix_operation_cancel(struct goodix_operation *operation)
{
	operation->cancelled = 1;
}
