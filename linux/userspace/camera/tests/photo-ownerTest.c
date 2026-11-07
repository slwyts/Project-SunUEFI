/* Actual owner entry/STREAMOFF cancellation; hardware entry points abort the
 * test if reached. Only socketpairs and synthetic frame metadata are used.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>

static int forbidden_calls;
int __wrap_open(const char *path, int flags, ...)
{ (void)path; (void)flags; forbidden_calls++; errno = EPERM; return -1; }
int __wrap_ioctl(int fd, unsigned long command, ...)
{ (void)fd; (void)command; forbidden_calls++; errno = EPERM; return -1; }

#define main camerad_entry_for_check
#include "../piano-camerad.c"
#undef main

static unsigned int stopped, restored;
static bool ready(void *p) { (void)p; return true; }
static int freeze(void *p, struct piano_photo_settings *s)
{
	(void)p;
	*s = (struct piano_photo_settings){
		.exposure_ns = 2000000, .readout_ns = 32000000, .frame_ns = 33000000,
		.line_ns_num = 10000, .line_ns_den = 1,
		.pulse_min_ns = 10000000, .pulse_max_ns = 300000000, .pulse_step_ns = 10000000,
		.flash_channels = 2, .exposure_lines = 200, .native_width = 4080, .native_height = 3060,
		.freeze_ns = 801000001,
	};
	return 0;
}
static int start(void *p, uint64_t pulse, uint64_t *a, uint64_t *b)
{ (void)p; (void)pulse; (void)a; (void)b; assert(!"unexpected lamp start"); return -EIO; }
static int stop(void *p, uint64_t *at, uint32_t *fault)
{ (void)p; *at = 900000000; *fault = 0; stopped++; return 0; }
static void restore(void *p) { (void)p; restored++; }

static void check_raw_levels(struct cam *camera)
{
	struct piano_quad_params params;
	struct piano_quad *converter = NULL;
	uint8_t raw[80], output[96];

	camera->dgain = DGAIN_UNITY;
	camera->rgain = camera->bgain = 1;
	quad_controls(camera, &params);
	assert(params.black_level == 64.0f && params.digital_gain == 1.0f);
	params.gamma = 1;
	params.contrast = 0;
	memset(params.ccm, 0, sizeof(params.ccm));
	params.ccm[0] = params.ccm[4] = params.ccm[8] = 1;
	assert(piano_quad_create(8, 8, PIANO_QUAD_BGGR, &converter) == 0);
	for (unsigned int level = 0; level < 2; level++) {
		/* Real packed RAW10: pedestal64 must map to black, white1023
		 * must still map to white with no extra normalization gain.
		 */
		for (unsigned int group = 0; group < sizeof(raw); group += 5) {
			memset(raw + group, level ? 255 : 16, 4);
			raw[group + 4] = level ? 255 : 0;
		}
		assert(piano_quad_unpack(converter, raw, sizeof(raw), 10, &params) == 0);
		assert(piano_quad_render(converter, output, sizeof(output), NULL) == 0);
		for (unsigned int pixel = 0; pixel < 64; pixel++)
			assert(output[pixel] == (level ? 255 : 0));
		for (unsigned int pixel = 64; pixel < sizeof(output); pixel++)
			assert(output[pixel] == 128);
	}
	piano_quad_destroy(converter);
}

int main(void)
{
	struct cam c = { .mode = cam_cfgs[0], .vid_fd = -1, .sensor_fd = -1, .vfe_fd = -1, .vcm_fd = -1 };
	struct piano_photo_result result;
	int sockets[2], image_fd;
	static const struct piano_photo_ops ops = { ready, freeze, start, stop, restore };

	c.cfg = &c.mode;
	check_raw_levels(&c);
	piano_photo_init(&c.photo, &camera_photo_ops, &c);
	c.mode.in_w = 8160;
	assert(camera_photo_request(&c, -1) == -EOPNOTSUPP);
	c.mode.in_w = 4080;
	c.streaming = true;
	assert(camera_photo_request(&c, -1) == -EOPNOTSUPP);
	assert(c.streaming);
	c.streaming = false;
	assert(forbidden_calls == 0 && !c.streaming && !piano_photo_active(&c.photo));

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
	piano_photo_init(&c.photo, &ops, &c);
	assert(piano_photo_begin(&c.photo, sockets[0], true, 0) == 0);
	struct piano_photo_frame frame = {
		.sequence = 1, .complete = true, .physical_timing = true,
		.sof_ns = 768000000, .eof_ns = 800000000, .exposure_start_ns = 760000000, .exposure_end_ns = 799000000,
		.now_ns = 801000000,
	};
	piano_photo_frame(&c.photo, &frame);
	assert(c.photo.phase == PHOTO_SETTLE);
	c.streaming = true;
	assert(cam_stop(&c) == 0);
	assert(forbidden_calls == 0 && stopped == 1 && restored == 1 && !c.streaming);
	assert(piano_photo_receive(sockets[1], &result, &image_fd) == 0);
	assert(result.status == EPIPE && image_fd == -1);
	close(sockets[1]);
	puts("actual owner RAW10 black/white levels, full-RDI flash refusal, preview preservation and STREAMOFF cancellation passed");
	return 0;
}
