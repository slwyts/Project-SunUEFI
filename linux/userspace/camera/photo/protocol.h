/* SPDX-License-Identifier: MIT */
#ifndef PIANO_PHOTO_PROTOCOL_H
#define PIANO_PHOTO_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PIANO_PHOTO_MAGIC UINT32_C(0x50484f54)
#define PIANO_PHOTO_VERSION 1
#define PIANO_PHOTO_PHYSICAL_EOF (1U << 0)
#define PIANO_PHOTO_FLASH_COMPLETE (1U << 1)

/* One fixed-size little-endian record accompanies one SCM_RIGHTS descriptor.
 * A failure record contains a positive errno and no descriptor. All image
 * bytes are packed NV12; capture_stride records the original DMA stride.
 */
struct piano_photo_result {
	uint32_t magic, version, status, flags;
	uint32_t width, height, stride, capture_stride;
	uint32_t pixelformat, colorspace, ycbcr_enc, quantization;
	uint32_t sequence, flash_fault, flash_channels, exposure_lines;
	uint32_t native_width, native_height;
	uint64_t data_bytes, eof_ns, exposure_start_ns, exposure_end_ns;
	uint64_t light_start_ns, light_end_ns, strobe_call_ns, strobe_return_ns;
	uint64_t timeout_ns, stop_call_ns, exposure_ns, readout_ns;
	uint64_t line_ns_num, line_ns_den, frame_ns;
	uint64_t freeze_ns, sof_ns;
};
_Static_assert(sizeof(struct piano_photo_result) == 208, "photo protocol layout");

int piano_photo_send(int socket_fd, const struct piano_photo_result *result, int image_fd);
int piano_photo_receive(int socket_fd, struct piano_photo_result *result, int *image_fd);
int piano_photo_image(const uint8_t *y, const uint8_t *uv,
		      uint32_t width, uint32_t height, uint32_t capture_stride);
int piano_photo_validate_image(int fd, const struct piano_photo_result *result);

#endif
