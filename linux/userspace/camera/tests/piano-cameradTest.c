/* SPDX-License-Identifier: MIT */
/* Exercise the real owner functions and converter. Only device syscalls are
 * wrapped; this program neither opens a camera nor reads an image file.
 */
#define main camera_daemon_main
#include "../piano-camerad.c"
#undef main

#include <assert.h>

enum { CAPTURE_FD = 901, LOOPBACK_FD = 902 };
static struct cam *current;
static uint8_t *dma, *published;
static size_t dma_bytes, published_bytes;
static unsigned int qbuf_calls, start_calls, published_frames, dequeue_calls;
static struct timeval published_timestamp;
static uint32_t published_timestamp_flags;
static bool qbuf_ok, dequeue_error, qbuf_error;
static size_t used;

int __real_ioctl(int fd, unsigned long request, ...);

int __real_quad_worker_start(struct quad_worker *worker);

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	void *argument;

	va_start(arguments, request);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	assert(fd == CAPTURE_FD);
	if (request == VIDIOC_DQBUF) {
		struct v4l2_buffer *buffer = argument;

		buffer->index = 0;
		buffer->length = 1;
		buffer->sequence = 37;
		buffer->flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
			(dequeue_error ? V4L2_BUF_FLAG_ERROR : 0);
		buffer->timestamp = (struct timeval){ .tv_sec = 500,
			.tv_usec = 100000 + 16667 * dequeue_calls++ };
		buffer->m.planes[0] = (struct v4l2_plane) {
			.length = dma_bytes, .bytesused = used,
		};
		qbuf_ok = false;
		return 0;
	}
	if (request == VIDIOC_QBUF) {
		qbuf_calls++;
		if (qbuf_error) {
			errno = EIO;
			return -1;
		}
		qbuf_ok = true;
		/* DMA becomes writable by hardware once requeued. Changing it here
		 * detects any accidental retained pointer used by the real render.
		 */
		memset(dma, 0, dma_bytes);
		return 0;
	}
	/* No hardware colour controls may be sent down the RDI line. */
	assert(!"unexpected ioctl");
	return -1;
}

int __wrap_loopback_output_publish(struct loopback_output *output, const uint8_t *buffer,
				   size_t bytes, const struct timeval *timestamp, uint32_t flags)
{
	assert(output == &current->loop_output);
	assert(bytes == frame_size(current->cfg));
	memcpy(published, buffer, bytes);
	published_bytes = bytes;
	published_frames++;
	published_timestamp = *timestamp;
	published_timestamp_flags = flags;
	return 0;
}

int __wrap_quad_worker_start(struct quad_worker *worker)
{
	assert(qbuf_ok);
	start_calls++;
	return __real_quad_worker_start(worker);
}

static void fill_raw(uint8_t *raw, unsigned int width, unsigned int height,
		     unsigned int stride, unsigned int value)
{
	memset(raw, 0xce, (size_t)stride * height);
	for (unsigned int y = 0; y < height; y++) {
		for (unsigned int x = 0; x < width; x += 4) {
			uint8_t *group = raw + (size_t)y * stride + x / 4 * 5;

			memset(group, value >> 2, 4);
			group[4] = (value & 3) * 0x55;
		}
	}
}

static void check_modes(void)
{
	for (unsigned int camera = 0; camera < NUM_CAMS; camera++) {
		const struct cam_cfg *base = &cam_cfgs[camera];

		for (unsigned int i = 0; i < base->num_modes; i++) {
			struct cam_cfg mode = *base;
			const struct capture_mode *requested = &base->modes[i];

			assert(select_capture_mode(&mode, requested->width, requested->height,
						   requested->fps) == 0);
			assert(mode.in_w == requested->width && mode.in_h == requested->height);
		}
	}
	struct cam_cfg rear = cam_cfgs[0];
	assert(select_capture_mode(&rear, 1920, 1440, 30) == 0);
	assert(select_capture_mode(&rear, 1920, 1080, 60) == 0);
	assert(select_capture_mode(&rear, 1920, 1440, 10) == 0);
	assert(rear.in_w == 8160 && rear.in_h == 6120);

	uint32_t codes[] = { MEDIA_BUS_FMT_SRGGB10_1X10, MEDIA_BUS_FMT_SBGGR10_1X10,
		MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SGBRG10_1X10 };
	uint32_t formats[] = { V4L2_PIX_FMT_SRGGB10P, V4L2_PIX_FMT_SBGGR10P,
		V4L2_PIX_FMT_SGRBG10P, V4L2_PIX_FMT_SGBRG10P };
	for (unsigned int i = 0; i < 4; i++) {
		uint32_t format, cfa;

		assert(quad_format(codes[i], &format, &cfa) == 0);
		assert(format == formats[i] && cfa == i);
	}
	uint32_t ignored;
	assert(quad_format(MEDIA_BUS_FMT_YUYV8_1_5X8, &ignored, &ignored) < 0);
}

static void check_layout_and_controls(struct cam *camera)
{
	assert(capture_layout(camera) == 0);
	assert(camera->uv_offset == 0);
	assert(camera->frame_span == (size_t)(camera->cfg->in_h - 1) * camera->bpl +
	       camera->cfg->in_w * 5 / 4);
	unsigned int bpl = camera->bpl, image = camera->sizeimage;
	camera->bpl = camera->cfg->in_w;
	assert(capture_layout(camera) < 0 && errno == EINVAL);
	camera->bpl = bpl;
	camera->sizeimage = camera->frame_span - 1;
	assert(capture_layout(camera) < 0 && errno == EINVAL);
	camera->sizeimage = image;
	assert(capture_layout(camera) == 0);

	for (unsigned int i = 2; i <= 4; i++) {
		struct v4l2_query_ext_ctrl query = { 0 };
		int value;

		assert(control_query(camera, i, &query) == 0);
		assert(query.id == named_controls[i].id && query.step == 1);
		control_cache(camera, i, 2048);
		assert(control_get(camera, i, &value) == 0 && value == 2048);
	}
	camera->dgain = DGAIN_UNITY;
	camera->rgain = camera->bgain = 1;
}

static void check_raw_frame(struct cam *camera)
{
	struct piano_quad *reference = NULL;
	struct piano_quad_params params;
	struct piano_quad_metrics metrics;
	size_t bytes = frame_size(camera->cfg);
	uint8_t *expected = malloc(bytes);
	assert(expected);
	quad_controls(camera, &params);
	fill_raw(dma, camera->cfg->in_w, camera->cfg->in_h, camera->bpl, 512);
	assert(piano_quad_create(camera->cfg->in_w, camera->cfg->in_h,
				 PIANO_QUAD_RGGB, &reference) == 0);
	assert(piano_quad_unpack(reference, dma, dma_bytes, camera->bpl, &params) == 0);
	assert(piano_quad_render(reference, expected, bytes, &metrics) == 0);
	piano_quad_destroy(reference);

	assert(quad_worker_create(camera->cfg->in_w, camera->cfg->in_h,
				 PIANO_QUAD_RGGB, &camera->quad_worker) == 0);
	cam_frame(camera);
	assert(qbuf_calls == 1 && start_calls == 1 && !published_frames);
	/* READY may already have arrived. Either state still rejects a second
	 * frame until main consumes it, without starting another conversion.
	 */
	cam_frame(camera);
	assert(qbuf_calls == 2 && start_calls == 1);
	struct pollfd event = { .fd = quad_worker_fd(camera->quad_worker), .events = POLLIN };
	assert(poll(&event, 1, 2000) == 1 && event.revents & POLLIN);
	cam_quad_result(camera);
	assert(published_frames == 1 && published_bytes == bytes);
	assert(!memcmp(expected, published, bytes));
	assert(published_timestamp.tv_sec == 500 && published_timestamp.tv_usec == 100000);
	assert((published_timestamp_flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
	       V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);

	used = camera->frame_span - 1;
	cam_frame(camera);
	assert(qbuf_calls == 3 && start_calls == 1);
	used = dma_bytes;
	dequeue_error = true;
	cam_frame(camera);
	assert(qbuf_calls == 4 && start_calls == 1);
	dequeue_error = false;
	assert(quad_worker_stop(camera->quad_worker) == 0);
	camera->quad_worker = NULL;
	free(expected);
}

static void check_scaling(void)
{
	struct cam camera = { .mode = cam_cfgs[0], .quad_raw = true };
	camera.cfg = &camera.mode;
	camera.mode.in_w = camera.mode.out_w = 64;
	camera.mode.in_h = camera.mode.out_h = 48;
	uint8_t full[64 * 48 * 3 / 2];
	memset(full, 0, sizeof(full));
	assert(quad_output(&camera, full) == full);

	/* Wide output removes equal top/bottom margins before scaling. Use
	 * different outside colours to make a wrong crop or UV origin visible.
	 */
	camera.mode.out_w = 32;
	camera.mode.out_h = 16;
	camera.stage = malloc(frame_size(camera.cfg));
	assert(camera.stage);
	for (unsigned int row = 0; row < 48; row++)
		memset(full + row * 64, row >= 8 && row < 40 ? 100 : 20, 64);
	for (unsigned int row = 0; row < 24; row++) {
		for (unsigned int x = 0; x < 64; x += 2) {
			bool middle = row >= 4 && row < 20;
			full[64 * 48 + row * 64 + x] = middle ? 80 : 200;
			full[64 * 48 + row * 64 + x + 1] = middle ? 160 : 20;
		}
	}
	const uint8_t *output = quad_output(&camera, full);
	assert(output == camera.stage);
	for (unsigned int i = 0; i < 32 * 16; i++)
		assert(output[i] == 100);
	for (unsigned int i = 32 * 16; i < 32 * 16 * 3 / 2; i += 2)
		assert(output[i] == 80 && output[i + 1] == 160);
	struct stats stats;
	frame_stats(&camera, output, output + 32 * 16, 32, &stats);
	assert(stats.mean == 100);
	free(camera.stage);

	/* Tall output removes left/right margins. The interleaved chroma crop
	 * must start on the same even luma column, not half that byte offset.
	 */
	camera.mode.out_w = 16;
	camera.mode.out_h = 32;
	camera.stage = malloc(frame_size(camera.cfg));
	assert(camera.stage);
	for (unsigned int row = 0; row < 48; row++) {
		memset(full + row * 64, 10, 64);
		memset(full + row * 64 + 20, 120, 24);
	}
	for (unsigned int row = 0; row < 24; row++) {
		for (unsigned int x = 0; x < 64; x += 2) {
			bool middle = x >= 20 && x < 44;
			full[64 * 48 + row * 64 + x] = middle ? 60 : 200;
			full[64 * 48 + row * 64 + x + 1] = middle ? 180 : 20;
		}
	}
	output = quad_output(&camera, full);
	assert(output == camera.stage);
	for (unsigned int i = 0; i < 16 * 32; i++)
		assert(output[i] == 120);
	for (unsigned int i = 16 * 32; i < 16 * 32 * 3 / 2; i += 2) {
		/* Noninteger UV bilinear filtering uses fixed-point weights; allow
		 * its observed two-code-value rounding, not the outside colour.
		 */
		assert(abs(output[i] - 60) <= 2 && abs(output[i + 1] - 180) <= 2);
	}
	free(camera.stage);
}

static void check_tfe_timestamp(void)
{
	struct cam camera = { .mode = cam_cfgs[0], .loop_fd = LOOPBACK_FD,
		.vid_fd = CAPTURE_FD, .sensor_fd = -1, .vfe_fd = -1, .vcm_fd = -1,
		.nbufs = 1, .ae_manual = true, .wb_manual = true };
	camera.cfg = &camera.mode;
	camera.mode.in_w = camera.mode.out_w = 8;
	camera.mode.in_h = camera.mode.out_h = 8;
	camera.bpl = 8;
	camera.sizeimage = camera.frame_span = 96;
	camera.uv_offset = 64;
	dma_bytes = used = 96;
	dma = malloc(dma_bytes);
	published = malloc(dma_bytes);
	assert(dma && published);
	memset(dma, 0x71, dma_bytes);
	camera.bufs[0] = (struct buf){ .ptr = dma, .len = dma_bytes };
	current = &camera;
	dequeue_calls = 0;
	cam_frame(&camera);
	assert(published_timestamp.tv_sec == 500 && published_timestamp.tv_usec == 100000);
	cam_frame(&camera);
	assert(published_timestamp.tv_usec == 116667);
	assert((published_timestamp_flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
	       V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);
	struct timespec before, after;
	assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
	assert(loopback_black(&camera) == 0);
	assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
	int64_t before_us = before.tv_sec * INT64_C(1000000) + before.tv_nsec / 1000;
	int64_t after_us = after.tv_sec * INT64_C(1000000) + after.tv_nsec / 1000;
	int64_t black_us = published_timestamp.tv_sec * INT64_C(1000000) + published_timestamp.tv_usec;
	assert(black_us >= before_us && black_us <= after_us);
	for (unsigned int i = 0; i < 64; i++)
		assert(published[i] == 0);
	for (unsigned int i = 64; i < 96; i++)
		assert(published[i] == 128);
	free(published);
	free(dma);
}

int main(void)
{
	check_modes();
	check_scaling();
	struct cam camera = { .mode = cam_cfgs[0], .loop_fd = LOOPBACK_FD,
		.vid_fd = CAPTURE_FD, .sensor_fd = -1, .vfe_fd = -1, .vcm_fd = -1,
		.quad_raw = true, .nbufs = 1, .ae_manual = true, .wb_manual = true };
	camera.cfg = &camera.mode;
	camera.mode.in_w = camera.mode.out_w = 64;
	camera.mode.in_h = camera.mode.out_h = 32;
	memset(camera.mode.ccm, 0, sizeof(camera.mode.ccm));
	camera.mode.ccm[0] = camera.mode.ccm[4] = camera.mode.ccm[8] = 1024;
	camera.bpl = camera.mode.in_w * 5 / 4 + 8;
	camera.sizeimage = camera.bpl * camera.mode.in_h;
	current = &camera;
	dma_bytes = used = camera.sizeimage;
	dma = malloc(dma_bytes);
	published = malloc(frame_size(camera.cfg));
	assert(dma && published);
	camera.bufs[0] = (struct buf){ .ptr = dma, .len = dma_bytes };
	check_layout_and_controls(&camera);
	check_raw_frame(&camera);
	free(published);
	free(dma);
	check_tfe_timestamp();
	puts("owner checks passed: seven modes, RAW10 layout/order, software controls, "
	     "QBUF-before-render, CPU ownership, busy drop, completion, invalid-frame requeue, "
	     "full bypass and actual libyuv centre-crop/scale with chroma");
	return 0;
}
