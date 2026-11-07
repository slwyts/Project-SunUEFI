/* SPDX-License-Identifier: MIT */
#ifndef PIANO_PHOTO_CAPTURE_H
#define PIANO_PHOTO_CAPTURE_H

#include "protocol.h"
#include <stdbool.h>

enum piano_photo_phase { PHOTO_IDLE, PHOTO_WAIT_READY, PHOTO_SETTLE, PHOTO_LIT };

struct piano_photo_settings {
	uint64_t exposure_ns, readout_ns, frame_ns, line_ns_num, line_ns_den;
	uint64_t pulse_min_ns, pulse_step_ns, pulse_max_ns;
	uint64_t freeze_ns;
	uint32_t flash_channels;
	uint32_t exposure_lines, native_width, native_height;
};

/* Physical FRAME_SYNC SOF and same-sequence capture EOF are supplied by the
 * paired kernel. eof_ns is the upper bound of DQBUF's microsecond timestamp
 * bin. Exposure endpoints are required after controls have been frozen.
 * DMA dequeue time and loopback timestamps are never exposure inputs.
 */
struct piano_photo_frame {
	const uint8_t *y, *uv;
	uint32_t width, height, stride, colorspace, ycbcr_enc, quantization, sequence;
	uint64_t sof_ns, eof_ns, exposure_start_ns, exposure_end_ns, now_ns;
	bool complete, physical_timing;
};

struct piano_photo_ops {
	bool (*ready)(void *context);
	int (*freeze)(void *context, struct piano_photo_settings *settings);
	int (*start_flash)(void *context, uint64_t pulse_ns, uint64_t *call_ns, uint64_t *return_ns);
	int (*stop_flash)(void *context, uint64_t *call_ns, uint32_t *fault);
	void (*restore)(void *context);
};

struct piano_photo_capture {
	const struct piano_photo_ops *ops;
	void *context;
	enum piano_photo_phase phase;
	int client_fd;
	struct piano_photo_settings settings;
	uint64_t deadline_ns, pulse_ns, strobe_call_ns, strobe_return_ns;
	uint64_t light_start_ns, light_end_ns;
	uint32_t last_sequence;
	unsigned int settle_left;
	bool have_sequence, frozen;
};

void piano_photo_init(struct piano_photo_capture *capture, const struct piano_photo_ops *ops, void *context);
int piano_photo_begin(struct piano_photo_capture *capture, int client_fd, bool physical_eof_ready, uint64_t now_ns);
void piano_photo_frame(struct piano_photo_capture *capture, const struct piano_photo_frame *frame);
void piano_photo_tick(struct piano_photo_capture *capture, uint64_t now_ns);
void piano_photo_abort(struct piano_photo_capture *capture, int error, bool reply);
bool piano_photo_active(const struct piano_photo_capture *capture);
bool piano_photo_blocks_3a(const struct piano_photo_capture *capture);
int piano_photo_poll_timeout(const struct piano_photo_capture *capture, uint64_t now_ns);

#endif
