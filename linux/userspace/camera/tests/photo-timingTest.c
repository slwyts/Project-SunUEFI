/* Exercise the real physical-event adapter. No device calls or image input. */
#include "../photo/timing.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
	struct piano_photo_timing timing;
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC, .timestamp = { .tv_sec = 1 },
		.u.frame_sync.frame_sequence = 42,
	};
	struct v4l2_buffer buffer = {
		.sequence = 42, .timestamp = { .tv_sec = 1, .tv_usec = 32732 },
		.flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,
	};
	struct piano_photo_frame frame = { .now_ns = 1040000000 };

	piano_photo_timing_reset(&timing);
	assert(piano_photo_timing_frame(&timing, &buffer, 3707329, &frame) == -ENODATA);
	assert(piano_photo_timing_event(&timing, &event) == 0);
	assert(piano_photo_timing_frame(&timing, &buffer, 3707329, &frame) == 0);
	assert(frame.sof_ns == 1000000000 && frame.eof_ns == 1032732999);
	assert(frame.exposure_start_ns == 996292671 && frame.exposure_end_ns == frame.eof_ns);
	assert(timing.readout_ns == 32732999); /* Actual endpoint, not 32.224580ms. */
	buffer.sequence++;
	assert(piano_photo_timing_frame(&timing, &buffer, 0, &frame) == -ENODATA);
	buffer.sequence--;
	buffer.flags |= V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
	assert(piano_photo_timing_frame(&timing, &buffer, 0, &frame) == -EPROTO);
	buffer.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	assert(piano_photo_timing_frame(&timing, &buffer, 0, &frame) == -EPROTO);
	buffer.flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_ERROR;
	assert(piano_photo_timing_frame(&timing, &buffer, 0, &frame) == -EPROTO);
	event.u.frame_sync.frame_sequence = 44;
	assert(piano_photo_timing_event(&timing, &event) == -EPIPE);
	piano_photo_timing_reset(&timing);
	event.u.frame_sync.frame_sequence = UINT32_MAX;
	assert(piano_photo_timing_event(&timing, &event) == 0);
	event.u.frame_sync.frame_sequence = 0;
	assert(piano_photo_timing_event(&timing, &event) == 0);
	puts("physical SOF/EOF sequence, endpoint precision and error checks passed");
	return 0;
}
