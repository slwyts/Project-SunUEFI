// SPDX-License-Identifier: BSD-3-Clause
/* Milan event transport recovered from the factory GF3656ZSCB HAL.
 * See docs/devel/hardware/led-ir-fingerprint.md for function addresses.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "piano-goodix-events.h"

#define GOODIX_FINGER_DOWN (1U << 1)
#define GOODIX_IMAGE_RESULT (1U << 7)
#define GOODIX_MATCH_RETRY 1064
#define GOODIX_NAV_RESULT_OFFSET 0x2f4U
#define GOODIX_NAV_RESULT_BYTES 0x5a20U

static uint32_t read_word(const uint8_t *buffer, size_t offset)
{
	uint32_t value;

	memcpy(&value, buffer + offset, sizeof(value));
	return value;
}

static void write_word(uint8_t *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

static int quality_retry(int32_t status)
{
	/* Factory err_table + result dispatcher 0x423fc: only these branches
	 * issue acquired notifications and retain the running operation. HAT,
	 * storage, CRC, unknown errors and broken-sensor counters are excluded.
	 */
	switch (status) {
	case 1011: /* ACQUIRED_PARTIAL -> acquired 1 (0x4243c). */
	case 1060: /* SENSOR_BROKEN_CHECK_ALGO_ERROR -> acquired 1. */
	case 1012: /* ACQUIRED_IMAGER_DIRTY -> acquired 1060 (0x42424). */
	case 1101: /* ALGO_DIRTY_FINGER -> acquired 1060. */
	case 1104: /* ALGO_COVER_BROKEN -> acquired 1060. */
	case 1013: /* DUPLICATE_FINGER -> acquired 1026 (0x42624). */
	case 1045: /* DUPLICATE_AREA -> acquired 1025 (0x425b8). */
	case 1094: /* TOO_FAST -> acquired 1058 / 5 (0x426cc). */
	case 1110: /* ALGO_PALM_DETECT -> acquired 1028 (0x42690). */
	case 1117: /* TOO_SLOW -> acquired 4 (0x42510). */
		return 1;
	default:
		return 0;
	}
}

static int idle(struct goodix_events *events)
{
	int32_t status = 0;

	memset(events->buffer, 0, events->buffer_size);
	/* Milan cancel at HAL 0x40188 sends GF_CMD_CANCEL (1009), not 1011. */
	int result = events->command(events->context, 1009, 0x70, &status);
	if (!result && status)
		result = -EREMOTEIO;
	events->state = result ? GOODIX_EVENTS_FAILED : GOODIX_EVENTS_IDLE;
	events->purpose = GOODIX_EVENTS_METADATA;
	events->enrollment_extended = 0;
	events->enrollment_challenge = 0;
	return result;
}

int goodix_events_start(struct goodix_events *events)
{
	if (!events->command || !events->buffer ||
	    events->buffer_size < GOODIX_EVENT_BUFFER_BYTES)
		return -EINVAL;
	if (events->state != GOODIX_EVENTS_IDLE)
		return -EBUSY;
	int result = idle(events);

	if (!result)
		events->state = GOODIX_EVENTS_WAIT_IRQ;
	return result;
}

int goodix_events_read(struct goodix_events *events, struct goodix_sensor_event *event)
{
	int32_t status = 0;
	int retry = events->state == GOODIX_EVENTS_RETRY;
	int navigation = 0;
	int can_begin_capture = events->state == GOODIX_EVENTS_WAIT_IRQ;
	int extended = 0;
	size_t packet_bytes = GOODIX_EVENT_BUFFER_BYTES;

	if (events->state != GOODIX_EVENTS_WAIT_IRQ &&
	    events->state != GOODIX_EVENTS_DRAIN && !retry)
		return -EINVAL;
	memset(event, 0, sizeof(*event));
	if (events->state == GOODIX_EVENTS_WAIT_IRQ) {
		events->capture_state = GOODIX_CAPTURE_NONE;
		memset(events->buffer, 0, events->buffer_size);
		/* Factory Milan IRQ at 0x41390 starts phase 1, with a 512-byte
		 * result limit. The output HAT's authenticator_type is a big-endian
		 * HW_AUTH_FINGERPRINT value, not an enrollment authorization token.
		 */
		write_word(events->buffer, 0x98, 1);
		write_word(events->buffer, 0xa8, 512);
		write_word(events->buffer, 0x2c5, 0x02000000);
	} else if (retry) {
		/* HAL 0x41e18..0x41e28 stores a 64-bit 2, clearing the word
		 * after the phase as well. Preserve the rest of the returned packet.
		 */
		write_word(events->buffer, 0x98, 2);
		write_word(events->buffer, 0x9c, 0);
	} else if (events->capture_state == GOODIX_CAPTURE_PENDING ||
		   events->capture_state == GOODIX_CAPTURE_EXTENDED) {
		extended = events->purpose == GOODIX_EVENTS_ENROLL && events->enrollment_extended;
		if (extended) {
			if (events->buffer_size < GOODIX_ENROLL_EVENT_BUFFER_BYTES) {
				events->state = GOODIX_EVENTS_FAILED;
				return -EMSGSIZE;
			}
			/* HAL 0x417ac..0x418dc uses the same base packet, with this
			 * extension reset before each enrollment capture exchange.
			 */
			if (events->capture_state == GOODIX_CAPTURE_PENDING)
				memset(events->buffer + GOODIX_EVENT_BUFFER_BYTES, 0,
				       GOODIX_ENROLL_EVENT_BUFFER_BYTES - GOODIX_EVENT_BUFFER_BYTES);
			memset(events->buffer + 0xd13c, 0,
			       GOODIX_ENROLL_EVENT_BUFFER_BYTES - 0xd13c);
			write_word(events->buffer, 0xa4, 0);
			write_word(events->buffer, 0xd138, 0); /* ENROLL operation. */
			packet_bytes = GOODIX_ENROLL_EVENT_BUFFER_BYTES;
			events->capture_state = GOODIX_CAPTURE_EXTENDED;
		} else {
			events->capture_state = GOODIX_CAPTURE_ADVANCED;
		}
	} else if (read_word(events->buffer, 0x98) == 8) {
		navigation = 1;
		/* HAL 0x4169c..0x416b8 consumes and clears the navigation result
		 * before the next exchange. The watcher does not emit navigation keys.
		 */
		memset(events->buffer + GOODIX_NAV_RESULT_OFFSET, 0,
		       GOODIX_NAV_RESULT_BYTES);
	} else {
		uint32_t phase = read_word(events->buffer, 0x98);

		can_begin_capture = phase == 4 || phase == 5;
	}
	int result = events->command(events->context, 1016,
				     packet_bytes, &status);
	if (result) {
		events->state = GOODIX_EVENTS_FAILED;
		return result;
	}
	event->mask = read_word(events->buffer, 0x6c);
	event->operation = read_word(events->buffer, 0x70);
	event->phase = read_word(events->buffer, 0x98);
	event->application_status = status;

	/* Only a successful initial/phase-4/5 exchange can enter ordinary capture.
	 * HAL 0x41450..0x41458 and 0x41614..0x41618 clear the phase on errors;
	 * a later retry response cannot turn those errors into a pending capture.
	 * An already pending ordinary exchange survives a nested retry.
	 */
	if (!status && can_begin_capture && event->phase == 2 &&
	    events->capture_state == GOODIX_CAPTURE_NONE &&
	    (event->mask & GOODIX_FINGER_DOWN))
		events->capture_state = GOODIX_CAPTURE_PENDING;

	/* The image-result handler enters the 1064 loop only for the ordinary
	 * fingerprint operations (HAL 0x41bc8..0x41dac). Later retries may omit
	 * image bit 7; HAL 0x41e30..0x41e48 still continues while status is 1064.
	 * Enrollment is deliberately excluded: it may require an extended packet.
	 */
	if (status == GOODIX_MATCH_RETRY && event->operation >= 1 &&
	    event->operation <= 4 && (retry || (event->mask & GOODIX_IMAGE_RESULT)) &&
	    (events->purpose == GOODIX_EVENTS_METADATA ||
	     (events->purpose == GOODIX_EVENTS_AUTHENTICATE && event->operation <= 2))) {
		events->state = GOODIX_EVENTS_RETRY;
		return 1;
	}
	if (status) {
		int enrollment = events->purpose == GOODIX_EVENTS_ENROLL && !event->operation;
		int authentication = events->purpose == GOODIX_EVENTS_AUTHENTICATE &&
			(event->operation == 1 || event->operation == 2);
		/* The dispatcher is an image-result handler. A status from a
		 * different operation or a packet without image bit 7 is not proof
		 * that an authorized fingerprint attempt can recover from it.
		 */
		if ((event->mask & GOODIX_IMAGE_RESULT) && (enrollment || authentication) &&
		    quality_retry(status))
			event->outcome = GOODIX_EVENT_QUALITY_RETRY;
		else if ((event->mask & GOODIX_IMAGE_RESULT) && authentication &&
			 (status == 1006 || status == 1119))
			/* HAL 0x42454 -> authenticate_not_match reads byte +0x79
			 * at 0x24c98: a mistaken touch emits acquired 1028 and keeps
			 * the operation running instead of reporting a failed match.
			 */
			event->outcome = events->buffer[0x79] ? GOODIX_EVENT_QUALITY_RETRY :
				GOODIX_EVENT_NOT_MATCH;
		else {
			fprintf(stderr, "Goodix command 1016 failed: application=%d\n", status);
			events->state = GOODIX_EVENTS_FAILED;
			return -EREMOTEIO;
		}
		fprintf(stderr, "Goodix capture: outcome=%s application=%d operation=%u phase=%u mistaken-touch=%u\n",
			event->outcome == GOODIX_EVENT_NOT_MATCH ? "not-match" : "quality-retry",
			status, event->operation, event->phase, events->buffer[0x79]);
	}
	/* HAL 0x42774..0x42834 completes image/FF authentication with UPDATE_CONFIG
	 * (1109) and AUTHENTICATE_FINISH (1086). FIDO takes 0x424a0 -> 0x427cc and
	 * still reaches AUTHENTICATE_FINISH at 0x42830. Until a real authentication
	 * caller owns those flows, cancel instead of declaring the IRQ drained or
	 * exposing group, template ID, HAT or FIDO result bytes.
	 */
	if ((event->mask & GOODIX_IMAGE_RESULT) &&
	    ((events->purpose != GOODIX_EVENTS_AUTHENTICATE &&
	      (event->operation == 1 || event->operation == 2)) || event->operation == 4)) {
		events->state = GOODIX_EVENTS_FAILED;
		return -EOPNOTSUPP;
	}

	/* HAL 0x41728..0x41778 advances ordinary phase 2 once with the same packet;
	 * the matching algorithm runs in the signed TA, not in the Android HAL.
	 * The following result handler at 0x41a8c can retry, but does not repeat
	 * the ordinary phase-2 exchange merely because its phase is unchanged.
	 * Operation 0 may select the 0x10e9e0 enrollment packet at 0x41760.
	 * NAV (6) and stable-factor testing (47) use different branches.
	 */
	if ((event->phase == 2 && ((event->operation == 0 &&
				  events->purpose != GOODIX_EVENTS_ENROLL) ||
				 event->operation == 6 || event->operation == 47)) ||
	    (event->phase == 8 && event->operation != 6)) {
		events->state = GOODIX_EVENTS_FAILED;
		return -EOPNOTSUPP;
	}
	/* New capture/navigation branches require finger-down bit 1 at HAL
	 * 0x415e4/0x4164c. An ongoing navigation loop only tests its phase.
	 */
	if (extended && event->phase != 2)
		events->capture_state = GOODIX_CAPTURE_ADVANCED;
	if (events->capture_state == GOODIX_CAPTURE_PENDING ||
	    (extended && event->phase == 2) ||
	    event->phase == 4 || event->phase == 5 ||
	    (event->phase == 8 && (navigation || (event->mask & GOODIX_FINGER_DOWN)))) {
		events->state = GOODIX_EVENTS_DRAIN;
	} else {
		events->state = GOODIX_EVENTS_WAIT_IRQ;
	}
	if (!status && (event->mask & GOODIX_IMAGE_RESULT) &&
	    ((events->purpose == GOODIX_EVENTS_ENROLL && event->operation == 0) ||
	     (events->purpose == GOODIX_EVENTS_AUTHENTICATE &&
	      (event->operation == 1 || event->operation == 2)))) {
		events->resume_state = events->state;
		events->state = GOODIX_EVENTS_RESULT;
		return 0;
	}
	return events->state == GOODIX_EVENTS_DRAIN;
}

int goodix_events_resume(struct goodix_events *events)
{
	if (events->state != GOODIX_EVENTS_RESULT)
		return -EINVAL;
	events->state = events->resume_state;
	return events->state == GOODIX_EVENTS_DRAIN;
}

int goodix_events_cancel(struct goodix_events *events)
{
	if (events->state == GOODIX_EVENTS_IDLE)
		return 0;
	return idle(events);
}
