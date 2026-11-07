// SPDX-License-Identifier: BSD-3-Clause
#ifndef PIANO_GOODIX_EVENTS_H
#define PIANO_GOODIX_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#define GOODIX_EVENT_BUFFER_BYTES 0xd134U
#define GOODIX_ENROLL_EVENT_BUFFER_BYTES 0x10e9e0U

enum goodix_events_state {
	GOODIX_EVENTS_IDLE,
	GOODIX_EVENTS_WAIT_IRQ,
	GOODIX_EVENTS_DRAIN,
	GOODIX_EVENTS_RETRY,
	GOODIX_EVENTS_RESULT,
	GOODIX_EVENTS_FAILED,
};

enum goodix_events_purpose {
	GOODIX_EVENTS_METADATA,
	GOODIX_EVENTS_ENROLL,
	GOODIX_EVENTS_AUTHENTICATE,
};

enum goodix_capture_state {
	GOODIX_CAPTURE_NONE,
	GOODIX_CAPTURE_PENDING,
	GOODIX_CAPTURE_ADVANCED,
	GOODIX_CAPTURE_EXTENDED,
};

enum goodix_event_outcome {
	GOODIX_EVENT_ORDINARY,
	GOODIX_EVENT_QUALITY_RETRY,
	GOODIX_EVENT_NOT_MATCH,
};

/* Sensor metadata only. An interrupt or event mask is never a match result. */
struct goodix_sensor_event {
	uint32_t mask;
	uint32_t operation;
	uint32_t phase;
	int32_t application_status;
	/* A recoverable image condition is never a successful RESULT packet. */
	enum goodix_event_outcome outcome;
};

/* The caller serializes commands and owns the shared buffer and IRQ fd.
 * command returns transport errors separately from the TA's operation status.
 * This interface does not install signals, poll, spawn a thread or log secrets.
 */
struct goodix_events {
	int (*command)(void *context, uint32_t command, size_t bytes,
		       int32_t *application_status);
	void *context;
	uint8_t *buffer;
	size_t buffer_size;
	enum goodix_events_state state;
	enum goodix_events_state resume_state;
	enum goodix_capture_state capture_state;
	enum goodix_events_purpose purpose;
	uint64_t enrollment_challenge;
	int enrollment_extended;
};

int goodix_events_start(struct goodix_events *events);

/* Call after consuming a UIO IRQ, then again while the result is 1.
 * A zero result completes that IRQ; only then may the caller re-enable it.
 * Each call performs one command, so the caller can check cancellation and
 * its deadline between continuations. Only the ordinary event packet is used;
 * enrollment's optional extended packet is not supported by this watcher.
 * Image-result metadata is not an authentication decision. Group, template ID,
 * image data and HAT remain private to the shared buffer and are not returned.
 * An image result with status 0 and authentication operation 1/2/4 returns
 * -EOPNOTSUPP: its required authentication-finish exchange is not implemented.
 * The watcher must cancel it rather than acknowledge it or re-enable the IRQ.
 * Negative results require cancel/close, not a synthetic authentication result.
 */
int goodix_events_read(struct goodix_events *events, struct goodix_sensor_event *event);

/* An explicit operation consumer must inspect a paused image result before
 * continuing the handshake. Metadata watchers cannot enter RESULT state.
 */
int goodix_events_resume(struct goodix_events *events);
int goodix_events_cancel(struct goodix_events *events);

#endif
