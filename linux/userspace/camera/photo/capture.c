/* SPDX-License-Identifier: MIT */
#include "capture.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define PHOTO_MARGIN_NS UINT64_C(5000000)
#define PHOTO_CONTROL_BUDGET_NS UINT64_C(5000000)
#define PHOTO_DEADLINE_NS UINT64_C(5000000000)

void piano_photo_init(struct piano_photo_capture *c, const struct piano_photo_ops *ops, void *context)
{
	memset(c, 0, sizeof(*c));
	c->ops = ops;
	c->context = context;
	c->client_fd = -1;
}

bool piano_photo_active(const struct piano_photo_capture *c) { return c->phase != PHOTO_IDLE; }
bool piano_photo_blocks_3a(const struct piano_photo_capture *c)
{ return c->phase == PHOTO_SETTLE || c->phase == PHOTO_LIT; }

int piano_photo_begin(struct piano_photo_capture *c, int client_fd, bool physical_eof_ready, uint64_t now_ns)
{
	if (piano_photo_active(c)) return -EBUSY;
	/* Optional FRAME_SYNC subscription is required; ordinary preview does
	 * not depend on this additional capture interface.
	 */
	if (!physical_eof_ready) return -EOPNOTSUPP;
	c->client_fd = client_fd;
	c->phase = PHOTO_WAIT_READY;
	c->deadline_ns = now_ns + PHOTO_DEADLINE_NS;
	return 0;
}

static int choose_pulse(struct piano_photo_capture *c)
{
	const struct piano_photo_settings *s = &c->settings;
	uint64_t required, steps;

	if (!s->exposure_ns || !s->readout_ns || !s->frame_ns ||
	    !s->line_ns_num || !s->line_ns_den || !s->flash_channels ||
	    !s->exposure_lines || !s->native_width || !s->native_height ||
	    !s->freeze_ns ||
	    !s->pulse_step_ns || s->pulse_min_ns > s->pulse_max_ns)
		return -EINVAL;
	/* One frame-period of phase uncertainty, plus the whole rolling
	 * exposure union and the measured-control-call budget. Do not clamp
	 * long exposure or change FLL behind the owner's back.
	 */
	required = s->exposure_ns + UINT64_C(1000000) +
		   (s->line_ns_num + s->line_ns_den - 1) / s->line_ns_den +
		   s->readout_ns + s->frame_ns +
		   2 * PHOTO_MARGIN_NS + PHOTO_CONTROL_BUDGET_NS;
	if (required <= s->pulse_min_ns) c->pulse_ns = s->pulse_min_ns;
	else {
		steps = (required - s->pulse_min_ns + s->pulse_step_ns - 1) / s->pulse_step_ns;
		c->pulse_ns = s->pulse_min_ns + steps * s->pulse_step_ns;
	}
	return c->pulse_ns <= s->pulse_max_ns ? 0 : -ERANGE;
}

static void finish(struct piano_photo_capture *c, int error,
		   const struct piano_photo_frame *frame, bool reply)
{
	struct piano_photo_result result = {
		.magic = PIANO_PHOTO_MAGIC, .version = PIANO_PHOTO_VERSION,
	};
	uint64_t stop_ns = 0;
	uint32_t fault = 0;
	int image_fd = -1;

	if (c->frozen) {
		int stopped = c->ops->stop_flash(c->context, &stop_ns, &fault);

		if (!error && stopped < 0) error = -stopped;
		if (!error && fault) error = EIO;
		if (stop_ns && stop_ns < c->light_end_ns) c->light_end_ns = stop_ns;
	}
	if (!error && frame) {
		if (!stop_ns || frame->exposure_start_ns < c->light_start_ns ||
		    frame->exposure_end_ns > c->light_end_ns)
			error = ETIME;
		else {
			image_fd = piano_photo_image(frame->y, frame->uv, frame->width,
						     frame->height, frame->stride);
			if (image_fd < 0) error = -image_fd;
		}
	}
	if (!error && frame) {
		result.flags = PIANO_PHOTO_PHYSICAL_EOF | PIANO_PHOTO_FLASH_COMPLETE;
		result.width = frame->width; result.height = frame->height;
		result.stride = frame->width; result.capture_stride = frame->stride;
		result.pixelformat = V4L2_PIX_FMT_NV12;
		result.colorspace = frame->colorspace; result.ycbcr_enc = frame->ycbcr_enc;
		result.quantization = frame->quantization; result.sequence = frame->sequence;
		result.flash_channels = c->settings.flash_channels;
		result.exposure_lines = c->settings.exposure_lines;
		result.native_width = c->settings.native_width; result.native_height = c->settings.native_height;
		result.data_bytes = (uint64_t)frame->width * frame->height * 3 / 2;
		result.eof_ns = frame->eof_ns;
		result.exposure_start_ns = frame->exposure_start_ns;
		result.exposure_end_ns = frame->exposure_end_ns;
		result.light_start_ns = c->light_start_ns; result.light_end_ns = c->light_end_ns;
		result.strobe_call_ns = c->strobe_call_ns; result.strobe_return_ns = c->strobe_return_ns;
		result.timeout_ns = c->pulse_ns; result.stop_call_ns = stop_ns;
		result.exposure_ns = c->settings.exposure_ns; result.readout_ns = c->settings.readout_ns;
		result.line_ns_num = c->settings.line_ns_num; result.line_ns_den = c->settings.line_ns_den;
		result.frame_ns = c->settings.frame_ns;
		result.freeze_ns = c->settings.freeze_ns; result.sof_ns = frame->sof_ns;
	}
	result.status = (uint32_t)error;
	result.flash_fault = fault;
	if (reply && c->client_fd >= 0)
		(void)piano_photo_send(c->client_fd, &result, error ? -1 : image_fd);
	if (image_fd >= 0) close(image_fd);
	if (c->client_fd >= 0) close(c->client_fd);
	if (c->frozen) c->ops->restore(c->context);
	piano_photo_init(c, c->ops, c->context);
}

void piano_photo_abort(struct piano_photo_capture *c, int error, bool reply)
{
	if (piano_photo_active(c)) finish(c, error > 0 ? error : EIO, NULL, reply);
}

void piano_photo_frame(struct piano_photo_capture *c, const struct piano_photo_frame *f)
{
	int ret;

	if (!piano_photo_active(c)) return;
	if (f->now_ns >= c->deadline_ns) { piano_photo_abort(c, ETIMEDOUT, true); return; }
	if (!f->complete) { piano_photo_abort(c, EIO, true); return; }
	if (!f->physical_timing) { piano_photo_abort(c, EOPNOTSUPP, true); return; }
	if (!f->sof_ns || !f->eof_ns || f->sof_ns > f->eof_ns || f->eof_ns > f->now_ns ||
	    (c->phase == PHOTO_LIT &&
	     (!f->exposure_start_ns || f->exposure_start_ns > f->sof_ns ||
	      f->exposure_start_ns > f->exposure_end_ns || f->exposure_end_ns != f->eof_ns))) {
		piano_photo_abort(c, EPROTO, true); return;
	}
	if (c->have_sequence && f->sequence != c->last_sequence + 1) {
		piano_photo_abort(c, EIO, true); return;
	}
	c->have_sequence = true;
	c->last_sequence = f->sequence;
	if (c->phase == PHOTO_WAIT_READY) {
		if (!c->ops->ready(c->context)) return;
		ret = c->ops->freeze(c->context, &c->settings);
		if (ret < 0) { piano_photo_abort(c, -ret, true); return; }
		c->frozen = true;
		if (c->settings.freeze_ns < f->now_ns) { piano_photo_abort(c, EPROTO, true); return; }
		ret = choose_pulse(c);
		if (ret < 0) { piano_photo_abort(c, -ret, true); return; }
		c->settle_left = 2;
		c->phase = PHOTO_SETTLE;
		return;
	}
	if (c->phase == PHOTO_SETTLE) {
		/* Dequeued buffers may predate the freeze. Count only two real
		 * frame starts after all ownership/control reads completed.
		 */
		if (f->sof_ns <= c->settings.freeze_ns) return;
		if (--c->settle_left) return;
		ret = c->ops->start_flash(c->context, c->pulse_ns,
					 &c->strobe_call_ns, &c->strobe_return_ns);
		if (ret < 0) { piano_photo_abort(c, -ret, true); return; }
		if (c->strobe_return_ns < c->strobe_call_ns ||
		    c->pulse_ns <= c->strobe_return_ns - c->strobe_call_ns + 2 * PHOTO_MARGIN_NS) {
			piano_photo_abort(c, ETIME, true); return;
		}
		c->light_start_ns = c->strobe_return_ns + PHOTO_MARGIN_NS;
		c->light_end_ns = c->strobe_call_ns + c->pulse_ns - PHOTO_MARGIN_NS;
		c->phase = PHOTO_LIT;
		return;
	}
	if (f->exposure_start_ns >= c->light_start_ns && f->exposure_end_ns <= c->light_end_ns)
		finish(c, 0, f, true);
	else if (f->exposure_end_ns > c->light_end_ns)
		piano_photo_abort(c, ETIMEDOUT, true);
}

void piano_photo_tick(struct piano_photo_capture *c, uint64_t now_ns)
{
	if (piano_photo_active(c) && (now_ns >= c->deadline_ns ||
	    (c->phase == PHOTO_LIT && now_ns >= c->light_end_ns)))
		piano_photo_abort(c, ETIMEDOUT, true);
}

int piano_photo_poll_timeout(const struct piano_photo_capture *c, uint64_t now_ns)
{
	uint64_t deadline = c->deadline_ns, ms;

	if (!piano_photo_active(c)) return 1000;
	if (c->phase == PHOTO_LIT && c->light_end_ns < deadline) deadline = c->light_end_ns;
	if (now_ns >= deadline) return 0;
	ms = (deadline - now_ns + 999999) / 1000000;
	return ms < 1000 ? (int)ms : 1000;
}
