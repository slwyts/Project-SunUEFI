// SPDX-License-Identifier: BSD-3-Clause
/* Exercise the real event code against the factory packet exchanges. No TEE,
 * UIO, firmware, user templates or biometric device is opened by this test.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../linux/userspace/fingerprint/piano-goodix-events.h"

struct reply {
	uint32_t request_phase, mask, operation, phase;
	int32_t status;
	int transport_error;
	uint8_t mistaken_touch;
};

struct exchange {
	uint8_t buffer[GOODIX_EVENT_BUFFER_BYTES + 1];
	const struct reply *replies;
	size_t count, index;
	int cancel_count;
	int32_t cancel_status;
};

static uint32_t word(const uint8_t *buffer, size_t offset)
{
	uint32_t value;

	memcpy(&value, buffer + offset, sizeof(value));
	return value;
}

static void put_word(uint8_t *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

static int command(void *context, uint32_t number, size_t bytes, int32_t *status)
{
	struct exchange *exchange = context;
	uint8_t *buffer = exchange->buffer;

	assert(buffer[GOODIX_EVENT_BUFFER_BYTES] == 0x6d);
	if (number == 1009) {
		assert(bytes == 0x70);
		for (size_t i = 0; i < GOODIX_EVENT_BUFFER_BYTES; ++i)
			assert(buffer[i] == 0);
		++exchange->cancel_count;
		*status = exchange->cancel_status;
		return 0;
	}
	assert(number == 1016);
	assert(bytes == GOODIX_EVENT_BUFFER_BYTES);
	assert(exchange->index < exchange->count);
	const struct reply *reply = &exchange->replies[exchange->index++];

	assert(word(buffer, 0x98) == reply->request_phase);
	if (reply->request_phase == 1) {
		assert(word(buffer, 0xa8) == 512);
		assert(word(buffer, 0x2c5) == 0x02000000);
		assert(buffer[0x79] == 0);
		buffer[0x2f3] = 0x37;
		buffer[0x5d14] = 0x59;
	} else if (reply->request_phase == 8) {
		/* 0x4169c: clear exactly [0x2f4, 0x5d14), not the whole packet. */
		for (size_t i = 0x2f4; i < 0x5d14; ++i)
			assert(buffer[i] == 0);
		assert(buffer[0x2f3] == 0x37);
		assert(buffer[0x5d14] == 0x59);
	} else {
		assert(buffer[0x2f3] == 0x37);
		assert(buffer[0x5d14] == 0x59);
	}
	/* Every ordinary/continuation packet keeps the TA's opaque data. The
	 * watcher only extracts mask/operation/phase/status, never these bytes.
	 */
	if (reply->request_phase != 1)
		assert(word(buffer, 0x80) == 0xdecafbad);
	if (exchange->index > 1 &&
	    exchange->replies[exchange->index - 2].status == 1064)
		assert(word(buffer, 0x9c) == 0);
	if (reply->status == 1064)
		put_word(buffer, 0x9c, 0xffffffff);
	if (reply->phase == 8)
		memset(buffer + 0x2f4, 0xa5, 0x5a20);
	put_word(buffer, 0x80, 0xdecafbad);
	put_word(buffer, 0x6c, reply->mask);
	put_word(buffer, 0x70, reply->operation);
	put_word(buffer, 0x98, reply->phase);
	buffer[0x79] = reply->mistaken_touch;
	*status = reply->status;
	return reply->transport_error;
}

static void run_sequence(const struct reply *replies, const int *results,
			 size_t count)
{
	struct exchange exchange = { .replies = replies, .count = count };
	struct goodix_events events = {
		.command = command, .context = &exchange,
		.buffer = exchange.buffer, .buffer_size = GOODIX_EVENT_BUFFER_BYTES,
	};

	exchange.buffer[GOODIX_EVENT_BUFFER_BYTES] = 0x6d;
	assert(goodix_events_start(&events) == 0);
	assert(events.state == GOODIX_EVENTS_WAIT_IRQ);
	for (size_t i = 0; i < count; ++i) {
		struct goodix_sensor_event event;

		assert(goodix_events_read(&events, &event) == results[i]);
		assert(exchange.index == i + 1); /* One exchange per caller deadline check. */
		if (!replies[i].transport_error) {
			assert(event.mask == replies[i].mask);
			assert(event.operation == replies[i].operation);
			assert(event.phase == replies[i].phase);
			assert(event.application_status == replies[i].status);
		}
	}
	if (results[count - 1] < 0)
		assert(events.state == GOODIX_EVENTS_FAILED);
	else if (results[count - 1])
		assert(events.state == GOODIX_EVENTS_DRAIN || events.state == GOODIX_EVENTS_RETRY);
	else
		assert(events.state == GOODIX_EVENTS_WAIT_IRQ);
	assert(goodix_events_cancel(&events) == 0);
	assert(events.state == GOODIX_EVENTS_IDLE);
	assert(goodix_events_cancel(&events) == 0);
	assert(exchange.cancel_count == 2);
}

static void capture(void)
{
	/* 0x415a4/0x415f8: retain phase 4/5 packets; 0x4176c: advance phase 2
	 * once. An unchanged phase on its reply does not start another capture.
	 */
	const struct reply replies[] = {
		{ .request_phase = 1, .mask = 2, .operation = 5, .phase = 4 },
		{ .request_phase = 4, .mask = 2, .operation = 5, .phase = 5 },
		{ .request_phase = 5, .mask = 2, .operation = 5, .phase = 2 },
		{ .request_phase = 2, .mask = 0x80, .operation = 5, .phase = 2 },
		{ .request_phase = 1, .mask = 2, .operation = 5, .phase = 2 },
		{ .request_phase = 2, .mask = 0x80, .operation = 5, .phase = 0 },
	};
	const int results[] = { 1, 1, 1, 0, 1, 0 };

	run_sequence(replies, results, sizeof(replies) / sizeof(replies[0]));
}

static void retry(void)
{
	/* 0x41e24: u64 phase=2; keep retrying status 1064 even when image bit 7
	 * is absent. A failed initial IRQ must not start ordinary capture merely
	 * because its retry finishes with phase 2 and finger-down still set.
	 */
	const struct reply failed_initial[] = {
		{ .request_phase = 1, .mask = 0x82, .operation = 1, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .operation = 1, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .mask = 2, .operation = 1, .phase = 2 },
	};
	const struct reply after_capture[] = {
		{ .request_phase = 1, .mask = 2, .operation = 2, .phase = 2 },
		{ .request_phase = 2, .mask = 0x80, .operation = 2, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .mask = 2, .operation = 2, .phase = 2 },
	};
	const int results[] = { 1, 1, 0 };

	run_sequence(failed_initial, results, 3);
	run_sequence(after_capture, results, 3);
	for (uint32_t phase = 4; phase <= 5; ++phase) {
		const struct reply failed_continuation[] = {
			{ .request_phase = 1, .mask = 2, .operation = 1, .phase = phase },
			{ .request_phase = phase, .mask = 0x82, .operation = 1, .phase = 2, .status = 1064 },
			{ .request_phase = 2, .mask = 2, .operation = 1, .phase = 2 },
		};

		run_sequence(failed_continuation, results, 3);
	}
}

static void navigation(void)
{
	const struct reply replies[] = {
		{ .request_phase = 1, .mask = 2, .operation = 6, .phase = 8 },
		{ .request_phase = 8, .operation = 6, .phase = 8 },
		{ .request_phase = 8, .operation = 6 },
	};
	const int results[] = { 1, 1, 0 };

	run_sequence(replies, results, 3);
}

static void errors(void)
{
	const struct reply replies[] = {
		{ .request_phase = 1, .operation = 0, .phase = 2 },
		{ .request_phase = 1, .operation = 6, .phase = 2 },
		{ .request_phase = 1, .operation = 47, .phase = 2 },
		{ .request_phase = 1, .operation = 1, .phase = 8 },
		{ .request_phase = 1, .operation = 1, .phase = 2, .status = 1064 },
		{ .request_phase = 1, .mask = 0x80, .operation = 1, .status = 1006 },
		{ .request_phase = 1, .mask = 0x80, .operation = 1 },
		{ .request_phase = 1, .mask = 0x82, .operation = 2, .phase = 2 },
		{ .request_phase = 1, .mask = 0x80, .operation = 4 },
		{ .request_phase = 1, .transport_error = -EIO },
	};
	const int results[] = {
		-EOPNOTSUPP, -EOPNOTSUPP, -EOPNOTSUPP, -EOPNOTSUPP,
		-EREMOTEIO, -EREMOTEIO, -EOPNOTSUPP, -EOPNOTSUPP, -EOPNOTSUPP, -EIO,
	};

	for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); ++i)
		run_sequence(&replies[i], &results[i], 1);
	const struct reply pending[] = {
		{ .request_phase = 1, .mask = 0x80, .operation = 1, .phase = 2, .status = 1064 },
	};
	const int again[] = { 1 };

	run_sequence(pending, again, 1); /* Cancellation while retry is pending. */
	const struct reply authentication_result[] = {
		{ .request_phase = 1, .mask = 0x80, .operation = 1, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .mask = 0x80, .operation = 1 },
	};
	const int authentication_results[] = { 1, -EOPNOTSUPP };

	/* A successful image after retry still requires unimplemented finish
	 * commands. It must cancel, not re-enable IRQ or report authentication.
	 */
	run_sequence(authentication_result, authentication_results, 2);
	const struct reply fido_result[] = {
		{ .request_phase = 1, .mask = 0x80, .operation = 4, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .mask = 0x80, .operation = 4 },
	};

	run_sequence(fido_result, authentication_results, 2);
	const struct reply no_finger_down[] = {
		{ .request_phase = 1, .operation = 1, .phase = 2 },
		{ .request_phase = 1, .operation = 6, .phase = 8 },
	};
	const int done[] = { 0, 0 };

	run_sequence(no_finger_down, done, 2);
	struct exchange exchange = { .cancel_status = 1007 };
	struct goodix_events events = {
		.command = command, .context = &exchange,
		.buffer = exchange.buffer, .buffer_size = GOODIX_EVENT_BUFFER_BYTES,
	};

	exchange.buffer[GOODIX_EVENT_BUFFER_BYTES] = 0x6d;
	assert(goodix_events_start(&events) == -EREMOTEIO);
	assert(events.state == GOODIX_EVENTS_FAILED);
	assert(exchange.cancel_count == 1);
}

struct enrollment_exchange {
	uint8_t *buffer;
	unsigned int calls;
	int32_t second_status;
};

static int enrollment_command(void *context, uint32_t number, size_t bytes, int32_t *status)
{
	struct enrollment_exchange *exchange = context;
	uint8_t *buffer = exchange->buffer;

	assert(buffer[GOODIX_ENROLL_EVENT_BUFFER_BYTES] == 0x6d);
	*status = 0;
	if (number == 1009)
		return 0;
	assert(number == 1016);
	++exchange->calls;
	if (exchange->calls == 1) {
		assert(bytes == GOODIX_EVENT_BUFFER_BYTES);
		assert(word(buffer, 0x98) == 1);
		memset(buffer + GOODIX_EVENT_BUFFER_BYTES, 0xa5,
		       GOODIX_ENROLL_EVENT_BUFFER_BYTES - GOODIX_EVENT_BUFFER_BYTES);
	} else {
		assert(bytes == GOODIX_ENROLL_EVENT_BUFFER_BYTES);
		assert(word(buffer, 0x98) == 2);
		assert(word(buffer, 0xa4) == 0 && word(buffer, 0xd138) == 0);
		for (size_t i = 0xd13c; i < GOODIX_ENROLL_EVENT_BUFFER_BYTES; ++i)
			assert(buffer[i] == 0);
	}
	put_word(buffer, 0x70, 0); /* ENROLL. */
	put_word(buffer, 0x6c, 0x82);
	put_word(buffer, 0x98, exchange->calls == 3 ? 0 : 2);
	if (exchange->calls == 2)
		*status = exchange->second_status;
	return 0;
}

static void classified_status(int32_t status, enum goodix_events_purpose purpose,
			      uint32_t operation, uint32_t mask, int expected,
			      enum goodix_event_outcome outcome, uint8_t mistaken_touch)
{
	const struct reply reply = { .request_phase = 1, .mask = mask,
		.operation = operation, .status = status, .mistaken_touch = mistaken_touch };
	struct exchange exchange = { .replies = &reply, .count = 1 };
	struct goodix_events events = {
		.command = command, .context = &exchange,
		.buffer = exchange.buffer, .buffer_size = GOODIX_EVENT_BUFFER_BYTES,
	};
	struct goodix_sensor_event sample;

	exchange.buffer[GOODIX_EVENT_BUFFER_BYTES] = 0x6d;
	assert(!goodix_events_start(&events));
	events.purpose = purpose;
	assert(goodix_events_read(&events, &sample) == expected);
	assert(sample.application_status == status && sample.outcome == outcome);
	assert(events.state != GOODIX_EVENTS_RESULT); /* Nonzero is never success. */
	if (expected < 0)
		assert(events.state == GOODIX_EVENTS_FAILED);
	assert(!goodix_events_cancel(&events));
}

static void quality(void)
{
	const int32_t recoverable[] = {1011, 1012, 1013, 1045, 1060, 1094, 1101, 1104, 1110, 1117};
	const int32_t fatal[] = {1003, 1022, 1026, 1031, 1055, 1056, 1057, 1061, 1099,
		1100, 1234, -EIO};

	for (size_t i = 0; i < sizeof(recoverable) / sizeof(*recoverable); ++i) {
		classified_status(recoverable[i], GOODIX_EVENTS_ENROLL, 0, 0x80, 0,
			GOODIX_EVENT_QUALITY_RETRY, 0);
		classified_status(recoverable[i], GOODIX_EVENTS_AUTHENTICATE, 1, 0x80, 0,
			GOODIX_EVENT_QUALITY_RETRY, 0);
		classified_status(recoverable[i], GOODIX_EVENTS_METADATA, 1, 0x80, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 0);
		classified_status(recoverable[i], GOODIX_EVENTS_AUTHENTICATE, 0, 0x80, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 0);
		classified_status(recoverable[i], GOODIX_EVENTS_ENROLL, 0, 2, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 0);
	}
	for (size_t i = 0; i < sizeof(fatal) / sizeof(*fatal); ++i) {
		classified_status(fatal[i], GOODIX_EVENTS_ENROLL, 0, 0x80, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 1);
		classified_status(fatal[i], GOODIX_EVENTS_AUTHENTICATE, 1, 0x80, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 1);
	}
	for (int32_t status = 1006; status <= 1119; status += 113) {
		classified_status(status, GOODIX_EVENTS_AUTHENTICATE, 1, 0x80, 0,
			GOODIX_EVENT_NOT_MATCH, 0);
		classified_status(status, GOODIX_EVENTS_AUTHENTICATE, 2, 0x80, 0,
			GOODIX_EVENT_QUALITY_RETRY, 1);
		classified_status(status, GOODIX_EVENTS_AUTHENTICATE, 1, 0x80, 0,
			GOODIX_EVENT_QUALITY_RETRY, 0x80);
		classified_status(status, GOODIX_EVENTS_ENROLL, 0, 0x80, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 1);
		classified_status(status, GOODIX_EVENTS_METADATA, 1, 0x80, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 1);
		classified_status(status, GOODIX_EVENTS_AUTHENTICATE, 1, 2, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 1);
		classified_status(status, GOODIX_EVENTS_AUTHENTICATE, 4, 0x80, -EREMOTEIO,
			GOODIX_EVENT_ORDINARY, 1);
	}
	classified_status(1064, GOODIX_EVENTS_AUTHENTICATE, 1, 0x80, 1, GOODIX_EVENT_ORDINARY, 0);
	classified_status(1064, GOODIX_EVENTS_ENROLL, 1, 0x80, -EREMOTEIO, GOODIX_EVENT_ORDINARY, 0);
	classified_status(1064, GOODIX_EVENTS_AUTHENTICATE, 4, 0x80, -EREMOTEIO,
		GOODIX_EVENT_ORDINARY, 0);

	/* A bad extended frame does not become a remaining=0 enrollment result;
	 * retain the factory phase-2 exchange until a genuinely successful reply.
	 */
	struct enrollment_exchange exchange = {
		.buffer = calloc(1, GOODIX_ENROLL_EVENT_BUFFER_BYTES + 1), .second_status = 1011,
	};
	assert(exchange.buffer);
	exchange.buffer[GOODIX_ENROLL_EVENT_BUFFER_BYTES] = 0x6d;
	struct goodix_events events = {
		.command = enrollment_command, .context = &exchange,
		.buffer = exchange.buffer, .buffer_size = GOODIX_ENROLL_EVENT_BUFFER_BYTES,
	};
	struct goodix_sensor_event sample;
	assert(!goodix_events_start(&events));
	events.purpose = GOODIX_EVENTS_ENROLL;
	events.enrollment_extended = 1;
	assert(!goodix_events_read(&events, &sample) && events.state == GOODIX_EVENTS_RESULT);
	assert(goodix_events_resume(&events) == 1);
	assert(goodix_events_read(&events, &sample) == 1);
	assert(sample.outcome == GOODIX_EVENT_QUALITY_RETRY && events.state == GOODIX_EVENTS_DRAIN);
	assert(!goodix_events_read(&events, &sample) && events.state == GOODIX_EVENTS_RESULT);
	assert(sample.outcome == GOODIX_EVENT_ORDINARY);
	assert(!goodix_events_cancel(&events));
	free(exchange.buffer);
}

static void authorized(void)
{
	const struct reply reply = { .request_phase = 1, .mask = 0x80, .operation = 1 };
	struct exchange exchange = { .replies = &reply, .count = 1 };
	struct goodix_events events = {
		.command = command, .context = &exchange,
		.buffer = exchange.buffer, .buffer_size = GOODIX_EVENT_BUFFER_BYTES,
	};
	struct goodix_sensor_event sample;

	exchange.buffer[GOODIX_EVENT_BUFFER_BYTES] = 0x6d;
	assert(goodix_events_start(&events) == 0);
	events.purpose = GOODIX_EVENTS_AUTHENTICATE;
	assert(goodix_events_read(&events, &sample) == 0);
	assert(events.state == GOODIX_EVENTS_RESULT);
	assert(goodix_events_read(&events, &sample) == -EINVAL);
	assert(goodix_events_resume(&events) == 0);
	assert(events.state == GOODIX_EVENTS_WAIT_IRQ);
	assert(goodix_events_cancel(&events) == 0);
	assert(events.purpose == GOODIX_EVENTS_METADATA);

	struct enrollment_exchange enrollment = {
		.buffer = calloc(1, GOODIX_ENROLL_EVENT_BUFFER_BYTES + 1),
	};

	assert(enrollment.buffer);
	enrollment.buffer[GOODIX_ENROLL_EVENT_BUFFER_BYTES] = 0x6d;
	struct goodix_events enrolling = {
		.command = enrollment_command, .context = &enrollment,
		.buffer = enrollment.buffer, .buffer_size = GOODIX_ENROLL_EVENT_BUFFER_BYTES,
	};

	assert(goodix_events_start(&enrolling) == 0);
	enrolling.purpose = GOODIX_EVENTS_ENROLL;
	enrolling.enrollment_extended = 1;
	for (unsigned int i = 0; i < 3; ++i) {
		assert(goodix_events_read(&enrolling, &sample) == 0);
		assert(enrolling.state == GOODIX_EVENTS_RESULT);
		assert(goodix_events_resume(&enrolling) == (i != 2));
	}
	assert(enrollment.calls == 3);
	assert(goodix_events_cancel(&enrolling) == 0);
	free(enrollment.buffer);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "capture"))
		capture();
	else if (!strcmp(argv[1], "retry"))
		retry();
	else if (!strcmp(argv[1], "navigation"))
		navigation();
	else if (!strcmp(argv[1], "authorized"))
		authorized();
	else if (!strcmp(argv[1], "quality"))
		quality();
	else {
		assert(!strcmp(argv[1], "errors"));
		errors();
	}
	puts("Goodix event packet checks passed; no hardware commands were sent.");
	return 0;
}
