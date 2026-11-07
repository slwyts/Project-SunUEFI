/* SPDX-License-Identifier: MIT */
#include "jpeg.h"

#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <jpeglib.h>
#include <time.h>
#include <linux/videodev2.h>

struct jpeg_error { struct jpeg_error_mgr base; jmp_buf jump; };
struct photo_encoder {
	struct jpeg_compress_struct jpeg;
	struct jpeg_error error;
	JSAMPLE *row;
	bool limited, rec709;
};

static void encode_error(j_common_ptr jpeg)
{
	struct jpeg_error *error = (struct jpeg_error *)jpeg->err;

	longjmp(error->jump, 1);
}

static uint8_t channel(int value)
{
	return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static void rgb_row(JSAMPLE *rgb, const uint8_t *y, const uint8_t *uv,
		    uint32_t width, bool limited, bool rec709)
{
	for (uint32_t x = 0; x < width; x++) {
		int cb = uv[x & ~1U] - 128, cr = uv[(x & ~1U) + 1] - 128;
		int luma = y[x], r, g, b;

		if (limited) {
			luma = 298 * (luma - 16);
			r = (luma + (rec709 ? 459 : 409) * cr + 128) >> 8;
			g = (luma - (rec709 ? 55 : 100) * cb - (rec709 ? 136 : 208) * cr + 128) >> 8;
			b = (luma + (rec709 ? 541 : 516) * cb + 128) >> 8;
		} else {
			r = luma + (((rec709 ? 403 : 359) * cr + 128) >> 8);
			g = luma - (((rec709 ? 48 : 88) * cb + (rec709 ? 120 : 183) * cr + 128) >> 8);
			b = luma + (((rec709 ? 475 : 454) * cb + 128) >> 8);
		}
		rgb[3 * x] = channel(r); rgb[3 * x + 1] = channel(g); rgb[3 * x + 2] = channel(b);
	}
}

int piano_photo_jpeg(int fd, const struct piano_photo_result *r, FILE *output)
{
	struct photo_encoder *encoder;
	const uint8_t *pixels;
	uint32_t ycbcr = r->ycbcr_enc, quantization = r->quantization;
	int checked = piano_photo_validate_image(fd, r), ret = 0;

	if (checked < 0) return checked;
	if (r->width > JPEG_MAX_DIMENSION || r->height > JPEG_MAX_DIMENSION) return -ERANGE;
	if (ycbcr == V4L2_YCBCR_ENC_DEFAULT) ycbcr = V4L2_MAP_YCBCR_ENC_DEFAULT(r->colorspace);
	if (quantization == V4L2_QUANTIZATION_DEFAULT)
		quantization = V4L2_MAP_QUANTIZATION_DEFAULT(false, r->colorspace, ycbcr);
	if ((ycbcr != V4L2_YCBCR_ENC_601 && ycbcr != V4L2_YCBCR_ENC_709) ||
	    (quantization != V4L2_QUANTIZATION_FULL_RANGE && quantization != V4L2_QUANTIZATION_LIM_RANGE))
		return -EOPNOTSUPP;
	pixels = mmap(NULL, (size_t)r->data_bytes, PROT_READ, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) return -errno;
	encoder = calloc(1, sizeof(*encoder));
	if (!encoder) { munmap((void *)pixels, (size_t)r->data_bytes); return -ENOMEM; }
	encoder->limited = quantization == V4L2_QUANTIZATION_LIM_RANGE;
	encoder->rec709 = ycbcr == V4L2_YCBCR_ENC_709;
	encoder->jpeg.err = jpeg_std_error(&encoder->error.base);
	encoder->error.base.error_exit = encode_error;
	if (setjmp(encoder->error.jump)) { ret = -EIO; goto out; }
	jpeg_create_compress(&encoder->jpeg);
	jpeg_stdio_dest(&encoder->jpeg, output);
	encoder->jpeg.image_width = r->width; encoder->jpeg.image_height = r->height;
	encoder->jpeg.input_components = 3; encoder->jpeg.in_color_space = JCS_RGB;
	jpeg_set_defaults(&encoder->jpeg);
	jpeg_set_quality(&encoder->jpeg, 95, TRUE);
	encoder->row = malloc((size_t)r->width * 3);
	if (!encoder->row) { ret = -ENOMEM; goto out; }
	jpeg_start_compress(&encoder->jpeg, TRUE);
	while (encoder->jpeg.next_scanline < r->height) {
		uint32_t row = encoder->jpeg.next_scanline;
		const uint8_t *y = pixels + (size_t)row * r->stride;
		const uint8_t *uv = pixels + (size_t)r->stride * r->height + (size_t)(row / 2) * r->stride;

		rgb_row(encoder->row, y, uv, r->width, encoder->limited, encoder->rec709);
		jpeg_write_scanlines(&encoder->jpeg, &encoder->row, 1);
	}
	jpeg_finish_compress(&encoder->jpeg);
	if (fflush(output) != 0 || ferror(output)) ret = -EIO;
out:
	jpeg_destroy_compress(&encoder->jpeg);
	free(encoder->row);
	free(encoder);
	munmap((void *)pixels, (size_t)r->data_bytes);
	return ret;
}
