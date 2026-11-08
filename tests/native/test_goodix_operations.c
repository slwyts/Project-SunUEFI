// SPDX-License-Identifier: BSD-3-Clause
/* Run actual operation framing and the shared IRQ state machine. The command
 * callback emulates only factory wire replies, never a hardware match result.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../linux/userspace/fingerprint/piano-goodix-operations.h"

#define GROUP 0x400003e8U
#define TEMPLATE 42U
#define SID UINT64_C(0x123456789abcdef0)
#define CHALLENGE UINT64_C(0x1020304050607080)

enum fault { NONE, WRONG_GROUP, WRONG_TEMPLATE, WRONG_SID, WRONG_CHALLENGE,
	     WRONG_TYPE, WRONG_VERSION, CONFIG_ERROR, SAVE_ERROR, POST_ERROR };

struct exchange {
	uint8_t *buffer;
	size_t capacity;
	unsigned int calls, irq_calls, cancels, starts, saves, finishes, posts;
	int enrollment, extended, other_template;
	int32_t result_status;
	uint8_t mistaken_touch;
	enum fault fault;
	uint8_t enrollment_hat[GOODIX_OPERATION_HAT_BYTES];
};

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

static void put(uint8_t *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

static void token(uint8_t *hat, uint32_t type, uint64_t challenge, uint64_t sid)
{
	memset(hat, 0, GOODIX_OPERATION_HAT_BYTES);
	type = htobe32(type);
	memcpy(hat + 1, &challenge, sizeof(challenge));
	memcpy(hat + 9, &sid, sizeof(sid));
	memcpy(hat + 25, &type, sizeof(type));
	memset(hat + 37, 0x5a, 32);
}

static int command(void *context, uint32_t number, size_t bytes, int32_t *status)
{
	struct exchange *exchange = context;
	uint8_t *buffer = exchange->buffer;

	++exchange->calls;
	*status = 0;
	assert(bytes <= exchange->capacity);
	assert(buffer[exchange->capacity] == 0x6d);
	switch (number) {
	case 1009:
		assert(bytes == 0x70);
		++exchange->cancels;
		return 0;
	case 1006: {
		uint64_t challenge = CHALLENGE;

		assert(bytes == 0x78 && exchange->enrollment);
		memcpy(buffer + 0x70, &challenge, sizeof(challenge));
		return 0;
	}
	case 1007:
		assert(exchange->enrollment && bytes == 0xbc);
		assert(word(buffer, 0x6c) == GROUP && word(buffer, 0x70) == 0);
		assert(buffer[0x74] == 0 && buffer[0x75] == 0);
		assert(!memcmp(buffer + 0x76, exchange->enrollment_hat,
			       sizeof(exchange->enrollment_hat)));
		++exchange->starts;
		return 0;
	case 1010:
		assert(!exchange->enrollment && bytes == 0x80);
		assert(word(buffer, 0x6c) == GROUP);
		assert(wide(buffer, 0x70) == CHALLENGE);
		assert(word(buffer, 0x78) == UINT32_MAX);
		++exchange->starts;
		return 0;
	case 1012:
		assert(exchange->enrollment && bytes == 0x78);
		assert(word(buffer, 0x6c) == GROUP && word(buffer, 0x70) == TEMPLATE);
		++exchange->saves;
		if (exchange->fault == SAVE_ERROR)
			*status = 1003;
		return 0;
	case 1008:
		assert(exchange->enrollment && bytes == 0x6c);
		++exchange->posts;
		if (exchange->fault == POST_ERROR && exchange->posts == 1)
			*status = 1003;
		return 0;
	case 1109:
		assert(!exchange->enrollment && bytes == 0xc0 && buffer[0x6c] == 1);
		if (exchange->fault == CONFIG_ERROR)
			*status = 1003;
		return 0;
	case 1086:
		assert(!exchange->enrollment && bytes == 0x148);
		assert(word(buffer, 0x70) == 1 && word(buffer, 0x74) == 0x80);
		assert(word(buffer, 0x78) == GROUP && word(buffer, 0x7c) == TEMPLATE);
		++exchange->finishes;
		return 0;
	case 1016:
		break;
	default:
		assert(!"Unexpected command: no template deletion or global cleanup is allowed");
	}
	assert(exchange->starts == 1);
	if (word(buffer, 0x98) == 1)
		assert(buffer[0x79] == 0); /* Clear AUTH1010 and previous IRQ metadata. */
	++exchange->irq_calls;
	unsigned int index = exchange->irq_calls;

	if (exchange->extended && index > 1) {
		assert(bytes == GOODIX_ENROLL_EVENT_BUFFER_BYTES);
		assert(word(buffer, 0x98) == 2);
		assert(word(buffer, 0xa4) == 0 && word(buffer, 0xd138) == 0);
		for (size_t i = 0xd13c; i < GOODIX_ENROLL_EVENT_BUFFER_BYTES; ++i)
			assert(buffer[i] == 0);
	} else {
		assert(bytes == GOODIX_EVENT_BUFFER_BYTES);
	}
	if (index == 1 || (exchange->enrollment && !exchange->extended && index == 3)) {
		put(buffer, 0x6c, 2);
		put(buffer, 0x70, exchange->enrollment ? 0 : 1);
		put(buffer, 0x98, 2);
		return 0;
	}
	put(buffer, 0x6c, 0x80);
	put(buffer, 0x70, exchange->enrollment ? 0 : 1);
	put(buffer, 0x98, exchange->extended && index == 2 ? 2 : 0);
	put(buffer, 0x80, exchange->fault == WRONG_GROUP ? GROUP + 1 : GROUP);
	put(buffer, 0x84, exchange->fault == WRONG_TEMPLATE || exchange->other_template ?
		TEMPLATE + 1 : TEMPLATE);
	uint16_t remaining = exchange->enrollment && index == 2 ? 2 : 0;

	memcpy(buffer + 0x88, &remaining, sizeof(remaining));
	if (exchange->enrollment && remaining) {
		put(buffer, 0x80, 0);
		put(buffer, 0x84, 0); /* Progress does not yet assert a saved identity. */
	}
	if (!exchange->enrollment)
		token(buffer + 0x2ac, exchange->fault == WRONG_TYPE ? 1 : 2,
		      CHALLENGE + (exchange->fault == WRONG_CHALLENGE),
		      SID + (exchange->fault == WRONG_SID));
	if (exchange->fault == WRONG_VERSION)
		buffer[0x2ac] = 1;
	if (index == 2) {
		*status = exchange->result_status;
		buffer[0x79] = exchange->mistaken_touch;
	}
	return 0;
}

static void setup(struct exchange *exchange, struct goodix_events *events,
		  int enrollment, int extended)
{
	exchange->capacity = extended ? GOODIX_ENROLL_EVENT_BUFFER_BYTES : GOODIX_EVENT_BUFFER_BYTES;
	exchange->buffer = calloc(1, exchange->capacity + 1);
	assert(exchange->buffer);
	exchange->buffer[exchange->capacity] = 0x6d;
	exchange->enrollment = enrollment;
	exchange->extended = extended;
	*events = (struct goodix_events){
		.command = command, .context = exchange,
		.buffer = exchange->buffer, .buffer_size = exchange->capacity,
	};
	assert(goodix_events_start(events) == 0);
	if (enrollment) {
		uint64_t challenge = 0;

		assert(goodix_operation_challenge(events, &challenge) == 0 && challenge == CHALLENGE);
		token(exchange->enrollment_hat, 1, CHALLENGE, SID);
	}
}

static int step(struct goodix_operation *operation, struct goodix_operation_sample *sample)
{
	struct exchange *exchange = operation->events->context;
	unsigned int before = exchange->calls;
	int result = goodix_operation_step(operation, sample);

	assert(exchange->calls == before + 1); /* Caller can cancel between every command. */
	return result;
}

static void enroll(int extended, int cancel_after_save, enum fault fault)
{
	struct exchange exchange = { .fault = fault };
	struct goodix_events events;
	struct goodix_operation operation = {0};
	struct goodix_operation_sample sample;

	setup(&exchange, &events, 1, extended);
	assert(goodix_operation_start_enroll(&operation, &events, GROUP,
		exchange.enrollment_hat, extended ? GOODIX_ENROLL_PACKET_EXTENDED :
		GOODIX_ENROLL_PACKET_NORMAL) == 0);
	assert(step(&operation, &sample) == GOODIX_OPERATION_WAIT_IRQ);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	assert(step(&operation, &sample) == (extended ? GOODIX_OPERATION_CONTINUE :
		GOODIX_OPERATION_WAIT_IRQ));
	assert(sample.kind == GOODIX_OPERATION_ENROLL_PROGRESS && sample.remaining == 2);
	assert(sample.group == 0 && sample.template_id == 0);
	if (!extended)
		assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	assert(sample.kind == GOODIX_OPERATION_ENROLL_PROGRESS && sample.remaining == 0);
	assert(sample.group == GROUP && sample.template_id == TEMPLATE);
	assert(exchange.saves == 0);
	int result = step(&operation, &sample);

	if (fault == SAVE_ERROR) {
		assert(result == -EREMOTEIO && exchange.saves == 1 && !operation.saved);
		goodix_operation_cancel(&operation);
		assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
		assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
		assert(sample.kind == GOODIX_OPERATION_CANCELLED);
		free(exchange.buffer);
		return;
	}
	assert(result == GOODIX_OPERATION_CONTINUE);
	assert(sample.kind == GOODIX_OPERATION_TEMPLATE_SAVED && exchange.saves == 1);
	if (cancel_after_save)
		goodix_operation_cancel(&operation);
	result = step(&operation, &sample);
	if (fault == POST_ERROR) {
		assert(result == -EREMOTEIO && operation.saved);
		goodix_operation_cancel(&operation);
		assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
		assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
		assert(sample.kind == GOODIX_OPERATION_ENROLLED && exchange.posts == 2);
		free(exchange.buffer);
		return;
	}
	assert(result == GOODIX_OPERATION_COMPLETE);
	assert(sample.kind == GOODIX_OPERATION_ENROLLED && sample.group == GROUP);
	assert(sample.template_id == TEMPLATE && exchange.posts == 1 && exchange.cancels == 1);
	free(exchange.buffer);
}

static void authenticate(enum fault fault)
{
	struct exchange exchange = { .fault = fault };
	struct goodix_events events;
	struct goodix_operation operation = {0};
	struct goodix_operation_sample sample;
	uint32_t allowed = TEMPLATE;

	setup(&exchange, &events, 0, 0);
	assert(goodix_operation_start_authenticate(&operation, &events, GROUP, SID,
		CHALLENGE, &allowed, 1, 1) == 0);
	assert(step(&operation, &sample) == GOODIX_OPERATION_WAIT_IRQ);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	int result = step(&operation, &sample);

	if (fault == WRONG_TEMPLATE) {
		assert(result == GOODIX_OPERATION_CONTINUE);
		assert(sample.kind == GOODIX_OPERATION_NOT_MATCH);
		assert(!sample.group && !sample.template_id && !operation.template_id);
		assert(!wide(operation.hat, 9) && !exchange.finishes);
		goodix_operation_cancel(&operation);
		assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
		assert(sample.kind == GOODIX_OPERATION_CANCELLED);
	} else if (fault != NONE && fault != CONFIG_ERROR) {
		assert(result == -EACCES);
		assert(exchange.finishes == 0);
		goodix_operation_cancel(&operation);
		assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
		assert(sample.kind == GOODIX_OPERATION_CANCELLED);
	} else {
		assert(result == GOODIX_OPERATION_CONTINUE);
		assert(sample.kind != GOODIX_OPERATION_AUTHENTICATED);
		result = step(&operation, &sample);
		if (fault == CONFIG_ERROR) {
			assert(result == -EREMOTEIO && exchange.finishes == 0);
			goodix_operation_cancel(&operation);
			assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
			assert(sample.kind == GOODIX_OPERATION_CANCELLED);
		} else {
			assert(result == GOODIX_OPERATION_CONTINUE);
			assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
			assert(sample.kind == GOODIX_OPERATION_AUTHENTICATED);
			assert(sample.group == GROUP && sample.template_id == TEMPLATE);
			assert(wide(sample.hat, 9) == SID && wide(sample.hat, 1) == CHALLENGE);
			assert(word(sample.hat, 25) == htobe32(2));
			assert(exchange.finishes == 1);
		}
	}
	free(exchange.buffer);
}

static void invalid_identity_with_other_template(void)
{
	/* An unrequested template must not hide malformed authorization fields. */
	for (enum fault fault = WRONG_GROUP; fault <= WRONG_VERSION; ++fault) {
		if (fault == WRONG_TEMPLATE)
			continue;
		struct exchange exchange = { .fault = fault, .other_template = 1 };
		struct goodix_events events;
		struct goodix_operation operation = {0};
		struct goodix_operation_sample sample;
		uint32_t allowed = TEMPLATE;

		setup(&exchange, &events, 0, 0);
		assert(!goodix_operation_start_authenticate(&operation, &events, GROUP, SID,
			CHALLENGE, &allowed, 1, 1));
		assert(step(&operation, &sample) == GOODIX_OPERATION_WAIT_IRQ);
		assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
		assert(step(&operation, &sample) == -EACCES);
		assert(sample.kind != GOODIX_OPERATION_NOT_MATCH);
		assert(!operation.template_id && !wide(operation.hat, 9) && !exchange.finishes);
		goodix_operation_cancel(&operation);
		assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
		assert(sample.kind == GOODIX_OPERATION_CANCELLED);
		free(exchange.buffer);
	}
}

static void cancellation(void)
{
	struct exchange exchange = {0};
	struct goodix_events events;
	struct goodix_operation operation = {0};
	struct goodix_operation_sample sample;

	setup(&exchange, &events, 1, 1);
	assert(goodix_operation_start_enroll(&operation, &events, GROUP,
		exchange.enrollment_hat, GOODIX_ENROLL_PACKET_EXTENDED) == 0);
	assert(step(&operation, &sample) == GOODIX_OPERATION_WAIT_IRQ);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	goodix_operation_cancel(&operation);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
	assert(sample.kind == GOODIX_OPERATION_CANCELLED);
	assert(exchange.cancels == 2 && exchange.posts == 1 && exchange.saves == 0);
	free(exchange.buffer);
}

static void validation(void)
{
	struct exchange exchange = {0};
	struct goodix_events events;
	struct goodix_operation operation = {0};
	uint32_t allowed = TEMPLATE;

	setup(&exchange, &events, 1, 0);
	assert(goodix_operation_start_enroll(&operation, &events, 0,
		exchange.enrollment_hat, GOODIX_ENROLL_PACKET_NORMAL) == -EACCES);
	assert(goodix_operation_start_enroll(&operation, &events, GROUP,
		exchange.enrollment_hat, GOODIX_ENROLL_PACKET_EXTENDED) == -EMSGSIZE);
	exchange.enrollment_hat[1] ^= 1;
	assert(goodix_operation_start_enroll(&operation, &events, GROUP,
		exchange.enrollment_hat, GOODIX_ENROLL_PACKET_NORMAL) == -EACCES);
	assert(exchange.starts == 0);
	assert(goodix_operation_start_authenticate(&operation, &events, GROUP, 0,
		CHALLENGE, &allowed, 1, 1) == -EINVAL);
	assert(goodix_operation_start_authenticate(&operation, &events, GROUP, SID,
		CHALLENGE, &allowed, 0, 1) == -EINVAL);
	assert(goodix_operation_post_enroll(&events) == 0);
	assert(exchange.posts == 1 && !events.enrollment_challenge);
	free(exchange.buffer);
}

static void retry_results(void)
{
	const int32_t statuses[] = {1011, 1006, 1119, 1064, 1056};
	for (size_t i = 0; i < sizeof(statuses) / sizeof(*statuses); ++i) {
		struct exchange exchange = { .result_status = statuses[i] };
		struct goodix_events events;
		struct goodix_operation operation = {0};
		struct goodix_operation_sample sample;
		uint32_t allowed = TEMPLATE;

		setup(&exchange, &events, 0, 0);
		assert(!goodix_operation_start_authenticate(&operation, &events, GROUP, SID,
			CHALLENGE, &allowed, 1, 1));
		assert(step(&operation, &sample) == GOODIX_OPERATION_WAIT_IRQ);
		assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
		int result = step(&operation, &sample);
		assert(!exchange.finishes && !exchange.saves);
		if (statuses[i] == 1056) {
			assert(result == -EREMOTEIO);
		} else if (statuses[i] == 1064) {
			assert(result == GOODIX_OPERATION_CONTINUE);
			assert(sample.kind == GOODIX_OPERATION_ACQUIRED);
			/* Internal retry is neither acquisition failure nor final no-match. */
			assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
			assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
			assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
			assert(sample.kind == GOODIX_OPERATION_AUTHENTICATED && exchange.finishes == 1);
			free(exchange.buffer);
			continue;
		} else {
			assert(result == GOODIX_OPERATION_WAIT_IRQ);
			assert(sample.application_status == statuses[i]);
			assert(sample.kind == (statuses[i] == 1011 ? GOODIX_OPERATION_QUALITY_RETRY :
				GOODIX_OPERATION_NOT_MATCH));
		}
		goodix_operation_cancel(&operation);
		assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
		assert(sample.kind == GOODIX_OPERATION_CANCELLED && !exchange.finishes);
		free(exchange.buffer);
	}
	/* Factory authenticate_not_match emits only acquired 1028 for this
	 * flag. A later genuine result still needs identity checks and finish.
	 */
	for (int32_t status = 1006; status <= 1119; status += 113) {
		for (enum fault fault = NONE; fault <= WRONG_SID; ++fault) {
			struct exchange exchange = { .result_status = status,
				.mistaken_touch = 1, .fault = fault };
			struct goodix_events events;
			struct goodix_operation operation = {0};
			struct goodix_operation_sample sample;
			uint32_t allowed = TEMPLATE;

			setup(&exchange, &events, 0, 0);
			assert(!goodix_operation_start_authenticate(&operation, &events, GROUP, SID,
				CHALLENGE, &allowed, 1, 1));
			assert(step(&operation, &sample) == GOODIX_OPERATION_WAIT_IRQ);
			assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
			assert(step(&operation, &sample) == GOODIX_OPERATION_WAIT_IRQ);
			assert(sample.kind == GOODIX_OPERATION_QUALITY_RETRY);
			assert(!sample.group && !sample.template_id && !exchange.finishes);
			int result = step(&operation, &sample);
			if (fault == NONE) {
				assert(result == GOODIX_OPERATION_CONTINUE);
				assert(!exchange.finishes);
				assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
				assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
				assert(sample.kind == GOODIX_OPERATION_AUTHENTICATED);
				assert(exchange.finishes == 1);
			} else if (fault == WRONG_TEMPLATE) {
				assert(result == GOODIX_OPERATION_CONTINUE);
				assert(sample.kind == GOODIX_OPERATION_NOT_MATCH && !exchange.finishes);
				goodix_operation_cancel(&operation);
				assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
				assert(sample.kind == GOODIX_OPERATION_CANCELLED);
			} else {
				assert(result == -EACCES && !exchange.finishes);
			}
			free(exchange.buffer);
		}
	}
	struct exchange exchange = { .result_status = 1011 };
	struct goodix_events events;
	struct goodix_operation operation = {0};
	struct goodix_operation_sample sample;
	setup(&exchange, &events, 1, 1);
	assert(!goodix_operation_start_enroll(&operation, &events, GROUP,
		exchange.enrollment_hat, GOODIX_ENROLL_PACKET_EXTENDED));
	assert(step(&operation, &sample) == GOODIX_OPERATION_WAIT_IRQ);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	assert(sample.kind == GOODIX_OPERATION_QUALITY_RETRY && !exchange.saves);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	assert(sample.kind == GOODIX_OPERATION_ENROLL_PROGRESS && !sample.remaining);
	assert(step(&operation, &sample) == GOODIX_OPERATION_CONTINUE);
	assert(sample.kind == GOODIX_OPERATION_TEMPLATE_SAVED && exchange.saves == 1);
	assert(step(&operation, &sample) == GOODIX_OPERATION_COMPLETE);
	assert(sample.kind == GOODIX_OPERATION_ENROLLED && exchange.posts == 1);
	free(exchange.buffer);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "enroll")) {
		enroll(0, 0, NONE);
		enroll(1, 0, NONE);
		enroll(1, 1, NONE);
		enroll(1, 0, SAVE_ERROR);
		enroll(1, 0, POST_ERROR);
	} else if (!strcmp(argv[1], "authenticate")) {
		authenticate(NONE);
	} else if (!strcmp(argv[1], "identity")) {
		for (enum fault fault = WRONG_GROUP; fault <= CONFIG_ERROR; ++fault)
			authenticate(fault);
		invalid_identity_with_other_template();
	} else if (!strcmp(argv[1], "validation")) {
		validation();
	} else if (!strcmp(argv[1], "quality")) {
		retry_results();
	} else {
		assert(!strcmp(argv[1], "cancel"));
		cancellation();
	}
	return 0;
}
