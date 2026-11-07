/* SPDX-License-Identifier: MIT */
#include "loopback-output.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>

#include <linux/videodev2.h>

static int queue_ioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR);
	return result < 0 ? -errno : 0;
}

int loopback_output_clear(struct loopback_output *output)
{
	struct v4l2_requestbuffers release = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP,
	};
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	int result = 0, error;

	if (output->streaming)
		result = queue_ioctl(output->fd, VIDIOC_STREAMOFF, &type);
	/* This is software loopback RAM, never native capture DMA. Release our
	 * mappings before REQBUFS(0), allowing the next format to reallocate.
	 * Other consumers retain their own independent VMA references.
	 */
	for (unsigned int i = 0; i < output->count; i++) {
		if (output->maps[i].data &&
		    munmap(output->maps[i].data, output->maps[i].bytes) < 0 && !result)
			result = -errno;
	}
	if (output->allocated) {
		error = queue_ioctl(output->fd, VIDIOC_REQBUFS, &release);
		if (!result)
			result = error;
	}
	free(output->maps);
	memset(output, 0, sizeof(*output));
	return result;
}

int loopback_output_setup(struct loopback_output *output, int fd, size_t bytes)
{
	struct v4l2_requestbuffers request = {
		.count = 2,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP,
	};
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	int result;

	if (output->allocated || !bytes || bytes > UINT32_MAX)
		return -EINVAL;
	output->fd = fd;
	output->bytes = bytes;
	result = queue_ioctl(fd, VIDIOC_REQBUFS, &request);
	if (result < 0)
		return result;
	output->allocated = true;
	if (!request.count) {
		result = -ENOBUFS;
		goto fail;
	}
	output->maps = calloc(request.count, sizeof(*output->maps));
	if (!output->maps) {
		result = -ENOMEM;
		goto fail;
	}
	output->count = request.count;
	for (unsigned int i = 0; i < output->count; i++) {
		struct v4l2_buffer buffer = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};

		result = queue_ioctl(fd, VIDIOC_QUERYBUF, &buffer);
		if (result < 0)
			goto fail;
		if (buffer.length < bytes) {
			result = -ENOSPC;
			goto fail;
		}
		output->maps[i].data = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
					   MAP_SHARED, fd, buffer.m.offset);
		if (output->maps[i].data == MAP_FAILED) {
			output->maps[i].data = NULL;
			result = -errno;
			goto fail;
		}
		output->maps[i].bytes = buffer.length;
	}
	/* STREAMON may precede the first QBUF. The loopback stream is then
	 * active when the complete initialization/negotiation frame arrives.
	 */
	result = queue_ioctl(fd, VIDIOC_STREAMON, &type);
	if (result < 0)
		goto fail;
	output->streaming = true;
	return 0;

fail:
	loopback_output_clear(output);
	return result;
}

int loopback_output_publish(struct loopback_output *output, const uint8_t *frame,
			    size_t bytes, const struct timeval *timestamp, uint32_t flags)
{
	struct v4l2_buffer buffer = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP,
	};
	bool initial = output->initial < output->count;
	int result;

	if (!output->streaming || bytes != output->bytes ||
	    (flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
		return -EINVAL;
	if (initial) {
		buffer.index = output->initial;
	} else {
		result = queue_ioctl(output->fd, VIDIOC_DQBUF, &buffer);
		if (result < 0)
			return result;
	}
	if (buffer.index >= output->count)
		return -EIO;
	memcpy(output->maps[buffer.index].data, frame, bytes);
	buffer.bytesused = bytes;
	buffer.timestamp = *timestamp;
	buffer.flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
		       (flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
	buffer.field = V4L2_FIELD_NONE;
	result = queue_ioctl(output->fd, VIDIOC_QBUF, &buffer);
	if (!result && initial)
		output->initial++;
	return result;
}
