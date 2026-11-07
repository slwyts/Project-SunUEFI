/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../loopback-output.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/videodev2.h>

#define OUTPUT_FD 901
#define FRAME_BYTES 96
static unsigned int queued, live_maps, released, stopped, dequeued;
static bool streaming, backpressure, fail_second_query;
static void *addresses[2];
static uint8_t expected[FRAME_BYTES];
static struct timeval expected_timestamp;
static uint32_t expected_flags;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *argument;

	assert(fd == OUTPUT_FD);
	va_start(args, request);
	argument = va_arg(args, void *);
	va_end(args);
	if (request == VIDIOC_REQBUFS) {
		struct v4l2_requestbuffers *r = argument;

		assert(r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT && r->memory == V4L2_MEMORY_MMAP);
		if (!r->count) {
			assert(!streaming && !live_maps);
			released++;
		} else {
			assert(r->count == 2);
			r->count = 2;
		}
		return 0;
	}
	if (request == VIDIOC_QUERYBUF) {
		struct v4l2_buffer *b = argument;

		assert(b->index < 2);
		if (fail_second_query && b->index == 1) {
			errno = EIO;
			return -1;
		}
		b->length = FRAME_BYTES;
		b->m.offset = b->index * 4096;
		return 0;
	}
	if (request == VIDIOC_STREAMON) {
		assert(*(int *)argument == V4L2_BUF_TYPE_VIDEO_OUTPUT && live_maps == 2);
		streaming = true;
		return 0;
	}
	if (request == VIDIOC_STREAMOFF) {
		assert(*(int *)argument == V4L2_BUF_TYPE_VIDEO_OUTPUT && streaming);
		streaming = false;
		stopped++;
		return 0;
	}
	if (request == VIDIOC_DQBUF) {
		struct v4l2_buffer *b = argument;

		assert(streaming && queued >= 2);
		if (backpressure) {
			errno = EAGAIN;
			return -1;
		}
		b->index = dequeued++ % 2;
		return 0;
	}
	if (request == VIDIOC_QBUF) {
		struct v4l2_buffer *b = argument;

		assert(streaming && b->index < 2 && b->bytesused == FRAME_BYTES);
		assert(b->timestamp.tv_sec == expected_timestamp.tv_sec);
		assert(b->timestamp.tv_usec == expected_timestamp.tv_usec);
		assert(b->flags == expected_flags);
		assert(!memcmp(addresses[b->index], expected, FRAME_BYTES));
		queued++;
		return 0;
	}
	assert(!"unexpected ioctl");
	return -1;
}

void *__real_mmap(void *, size_t, int, int, int, off_t);
int __real_munmap(void *, size_t);

void *__wrap_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
	assert(fd == OUTPUT_FD && length == FRAME_BYTES);
	assert(protection == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED);
	void *mapped = __real_mmap(address, length, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	assert(mapped != MAP_FAILED);
	addresses[offset / 4096] = mapped;
	live_maps++;
	return mapped;
}

int __wrap_munmap(void *address, size_t bytes)
{
	assert(!streaming && bytes == FRAME_BYTES);
	live_maps--;
	return __real_munmap(address, bytes);
}

int main(void)
{
	struct loopback_output output = { 0 };
	memset(expected, 0x71, sizeof(expected));
	expected_timestamp = (struct timeval){ .tv_sec = 500, .tv_usec = 123456 };
	expected_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	assert(loopback_output_setup(&output, OUTPUT_FD, FRAME_BYTES) == 0);
	assert(loopback_output_publish(&output, expected, FRAME_BYTES, &expected_timestamp,
				       V4L2_BUF_FLAG_TIMESTAMP_COPY) == -EINVAL);
	assert(!queued);
	assert(loopback_output_publish(&output, expected, FRAME_BYTES, &expected_timestamp,
				       expected_flags) == 0);
	expected_timestamp.tv_usec += 16667;
	expected_flags |= V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
	assert(loopback_output_publish(&output, expected, FRAME_BYTES, &expected_timestamp,
				       expected_flags) == 0);
	assert(!dequeued && queued == 2);
	backpressure = true;
	assert(loopback_output_publish(&output, expected, FRAME_BYTES, &expected_timestamp,
				       expected_flags) == -EAGAIN);
	assert(queued == 2);
	backpressure = false;
	expected_timestamp.tv_usec += 16666;
	assert(loopback_output_publish(&output, expected, FRAME_BYTES, &expected_timestamp,
				       expected_flags) == 0);
	assert(dequeued == 1 && queued == 3);
	assert(loopback_output_clear(&output) == 0);
	assert(stopped == 1 && released == 1 && !live_maps && !output.allocated);
	assert(loopback_output_clear(&output) == 0);
	assert(released == 1);

	fail_second_query = true;
	assert(loopback_output_setup(&output, OUTPUT_FD, FRAME_BYTES) == -EIO);
	assert(released == 2 && !live_maps && !output.allocated);
	puts("loopback output: actual queue timestamp/source, reuse, backpressure and teardown checks passed");
	return 0;
}
