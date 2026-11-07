/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "protocol.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define PHOTO_U32_FIELDS(X) \
	X(magic) X(version) X(status) X(flags) X(width) X(height) X(stride) \
	X(capture_stride) X(pixelformat) X(colorspace) X(ycbcr_enc) X(quantization) \
	X(sequence) X(flash_fault) X(flash_channels) X(exposure_lines) X(native_width) X(native_height)
#define PHOTO_U64_FIELDS(X) \
	X(data_bytes) X(eof_ns) X(exposure_start_ns) X(exposure_end_ns) \
	X(light_start_ns) X(light_end_ns) X(strobe_call_ns) X(strobe_return_ns) \
	X(timeout_ns) X(stop_call_ns) X(exposure_ns) X(readout_ns) X(line_ns_num) X(line_ns_den) X(frame_ns) \
	X(freeze_ns) X(sof_ns)

static void wire_result(struct piano_photo_result *wire, const struct piano_photo_result *host)
{
#define ENCODE32(field) wire->field = htole32(host->field);
#define ENCODE64(field) wire->field = htole64(host->field);
	PHOTO_U32_FIELDS(ENCODE32)
	PHOTO_U64_FIELDS(ENCODE64)
#undef ENCODE32
#undef ENCODE64
}

static void host_result(struct piano_photo_result *host, const struct piano_photo_result *wire)
{
#define DECODE32(field) host->field = le32toh(wire->field);
#define DECODE64(field) host->field = le64toh(wire->field);
	PHOTO_U32_FIELDS(DECODE32)
	PHOTO_U64_FIELDS(DECODE64)
#undef DECODE32
#undef DECODE64
}

int piano_photo_send(int fd, const struct piano_photo_result *result, int image_fd)
{
	struct piano_photo_result wire;
	union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
	struct iovec iov = { .iov_base = &wire, .iov_len = sizeof(wire) };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	ssize_t bytes;

	wire_result(&wire, result);
	if (image_fd >= 0) {
		struct cmsghdr *cmsg;

		msg.msg_control = control.bytes;
		msg.msg_controllen = sizeof(control.bytes);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(image_fd));
		memcpy(CMSG_DATA(cmsg), &image_fd, sizeof(image_fd));
	}
	do { bytes = sendmsg(fd, &msg, MSG_NOSIGNAL); } while (bytes < 0 && errno == EINTR);
	return bytes == (ssize_t)sizeof(wire) ? 0 : -(bytes < 0 ? errno : EIO);
}

int piano_photo_validate_image(int fd, const struct piano_photo_result *r)
{
	const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
	struct stat st;
	uint64_t expected;
	int actual;

	if (!r->width || !r->height || (r->width & 1) || (r->height & 1) ||
	    r->stride != r->width || r->capture_stride < r->width ||
	    r->pixelformat != V4L2_PIX_FMT_NV12 ||
	    r->flags != (PIANO_PHOTO_PHYSICAL_EOF | PIANO_PHOTO_FLASH_COMPLETE) ||
	    r->flash_fault || !r->flash_channels ||
	    r->exposure_start_ns < r->light_start_ns ||
	    r->exposure_end_ns > r->light_end_ns ||
	    r->exposure_start_ns > r->exposure_end_ns || !r->eof_ns ||
	    !r->sof_ns || r->sof_ns > r->eof_ns || !r->freeze_ns ||
	    !r->line_ns_num || !r->line_ns_den || !r->exposure_lines ||
	    !r->native_width || !r->native_height || !r->frame_ns)
		return -EPROTO;
	expected = (uint64_t)r->width * r->height;
	if (expected > SIZE_MAX - expected / 2) return -EPROTO;
	expected += expected / 2;
	if (r->data_bytes != expected)
		return -EPROTO;
	if (fstat(fd, &st) < 0)
		return -errno;
	actual = fcntl(fd, F_GET_SEALS);
	if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != expected ||
	    actual < 0 || (actual & seals) != seals)
		return -EPROTO;
	return 0;
}

int piano_photo_receive(int fd, struct piano_photo_result *result, int *image_fd)
{
	struct piano_photo_result wire = {0};
	union { struct cmsghdr align; char bytes[CMSG_SPACE(4 * sizeof(int))]; } control;
	struct iovec iov = { .iov_base = &wire, .iov_len = sizeof(wire) };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
	int received = -1, count = 0, error = 0;
	ssize_t bytes;

	*image_fd = -1;
	do { bytes = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC | MSG_TRUNC); } while (bytes < 0 && errno == EINTR);
	if (bytes < 0)
		return -errno;
	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_len < CMSG_LEN(0)) { error = EPROTO; break; }
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
			error = EPROTO;
			continue;
		}
		for (size_t offset = 0; offset + sizeof(int) <= cmsg->cmsg_len - CMSG_LEN(0);
		     offset += sizeof(int)) {
			int item;

			memcpy(&item, (char *)CMSG_DATA(cmsg) + offset, sizeof(item));
			if (!count++) received = item;
			else close(item);
		}
	}
	host_result(result, &wire);
	if (bytes != (ssize_t)sizeof(wire) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
	    result->magic != PIANO_PHOTO_MAGIC || result->version != PIANO_PHOTO_VERSION ||
	    result->status > 4095 || count != (result->status ? 0 : 1))
		error = EPROTO;
	if (!error && !result->status) {
		int checked = piano_photo_validate_image(received, result);

		if (checked < 0) error = -checked;
	}
	if (error && received >= 0) close(received);
	else *image_fd = received;
	return -error;
}

int piano_photo_image(const uint8_t *y, const uint8_t *uv,
		      uint32_t width, uint32_t height, uint32_t capture_stride)
{
	uint64_t pixels = (uint64_t)width * height, bytes;
	char path[64];
	uint8_t *mapped;
	int fd, ro, error;

	if (!y || !uv || !width || !height || (width & 1) || (height & 1) || capture_stride < width ||
	    pixels > SIZE_MAX - pixels / 2)
		return -EINVAL;
	bytes = pixels + pixels / 2;
	if (bytes > INT64_MAX) return -EOVERFLOW;
	fd = memfd_create("camera-photo", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) return -errno;
	if (ftruncate(fd, (off_t)bytes) < 0) goto fail;
	mapped = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) goto fail;
	for (uint32_t row = 0; row < height; row++)
		memcpy(mapped + (size_t)row * width, y + (size_t)row * capture_stride, width);
	for (uint32_t row = 0; row < height / 2; row++)
		memcpy(mapped + (size_t)width * height + (size_t)row * width,
		       uv + (size_t)row * capture_stride, width);
	munmap(mapped, (size_t)bytes);
	if (fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) < 0) goto fail;
	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	ro = open(path, O_RDONLY | O_CLOEXEC);
	error = errno;
	close(fd);
	return ro < 0 ? -error : ro;
fail:
	error = errno;
	close(fd);
	return -error;
}
