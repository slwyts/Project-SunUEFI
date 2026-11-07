/* Tests the actual candidate modules. Callbacks replace hardware I/O only;
 * no camera node, flash node, real image or production socket is opened.
 */
#define _GNU_SOURCE
#include "../photo/capture.h"
#include "../photo/jpeg.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <jpeglib.h>
#include <linux/videodev2.h>

struct fake_hardware {
	struct piano_photo_settings settings;
	bool ready;
	int freeze_count, start_count, stop_count, restore_count;
	int freeze_error, start_error, stop_error;
	uint32_t fault;
	uint64_t stop_ns;
};

static bool ready(void *p) { return ((struct fake_hardware *)p)->ready; }
static int freeze(void *p, struct piano_photo_settings *settings)
{
	struct fake_hardware *hw = p;

	hw->freeze_count++;
	*settings = hw->settings;
	return hw->freeze_error;
}
static int start(void *p, uint64_t pulse, uint64_t *a, uint64_t *b)
{
	struct fake_hardware *hw = p;

	hw->start_count++;
	assert(pulse >= hw->settings.pulse_min_ns && pulse <= hw->settings.pulse_max_ns);
	*a = 1000000000; *b = 1002000000;
	return hw->start_error;
}
static int stop(void *p, uint64_t *at, uint32_t *fault)
{
	struct fake_hardware *hw = p;

	hw->stop_count++; *at = hw->stop_ns; *fault = hw->fault;
	return hw->stop_error;
}
static void restore(void *p) { ((struct fake_hardware *)p)->restore_count++; }
static const struct piano_photo_ops ops = { ready, freeze, start, stop, restore };

static void setup(struct piano_photo_capture *capture, struct fake_hardware *hw, int sockets[2])
{
	*hw = (struct fake_hardware){
		.ready = true, .stop_ns = 1060000000,
		.settings = {
			.exposure_ns = 2696794, .readout_ns = 32224580, .frame_ns = 33330687,
			.line_ns_num = 10534351, .line_ns_den = 1000,
			.pulse_min_ns = 10000000, .pulse_step_ns = 10000000, .pulse_max_ns = 300000000,
			.flash_channels = 2, .exposure_lines = 256, .native_width = 4080, .native_height = 3060,
			.freeze_ns = 900000001,
		},
	};
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
	piano_photo_init(capture, &ops, hw);
	assert(piano_photo_begin(capture, sockets[0], true, 0) == 0);
}

static struct piano_photo_frame frame(uint32_t sequence)
{
	static uint8_t y[48], uv[24];
	uint64_t now = 900000000 + (uint64_t)(sequence - 1) * 33000000;

	memset(y, 128, sizeof(y)); memset(uv, 128, sizeof(uv));
	return (struct piano_photo_frame){
		.y = y, .uv = uv, .width = 8, .height = 4, .stride = 12,
		.colorspace = V4L2_COLORSPACE_REC709, .ycbcr_enc = V4L2_YCBCR_ENC_709,
		.quantization = V4L2_QUANTIZATION_LIM_RANGE, .sequence = sequence,
		.sof_ns = now - 34000000, .eof_ns = now - 1000000, .exposure_start_ns = now - 37000000,
		.exposure_end_ns = now - 1000000, .now_ns = now,
		.complete = true, .physical_timing = true,
	};
}

static void feed(struct piano_photo_capture *c, uint32_t sequence)
{ struct piano_photo_frame f = frame(sequence); piano_photo_frame(c, &f); }

static void illuminated(struct piano_photo_capture *c, struct fake_hardware *hw)
{
	feed(c, 1); assert(c->phase == PHOTO_SETTLE && hw->freeze_count == 1 && hw->start_count == 0);
	feed(c, 2); assert(hw->start_count == 0);
	assert(c->settle_left == 2); /* queued before the freeze completed */
	feed(c, 3); assert(hw->start_count == 0 && c->settle_left == 1);
	feed(c, 4); assert(c->phase == PHOTO_LIT && hw->start_count == 1 && c->pulse_ns == 90000000);
}

static void expect_error(int peer, int error)
{
	struct piano_photo_result result; int image_fd = -1;

	assert(piano_photo_receive(peer, &result, &image_fd) == 0);
	if (result.status != (uint32_t)error)
		fprintf(stderr, "expected errno %d, received %u\n", error, result.status);
	assert(result.status == (uint32_t)error && image_fd == -1);
	close(peer);
}

static void selected_frame(struct piano_photo_capture *c)
{
	struct piano_photo_frame f = frame(5);

	f.now_ns = 1060000000; f.sof_ns = 1013000000; f.eof_ns = 1055000000;
	f.exposure_start_ns = 1010000000; f.exposure_end_ns = f.eof_ns;
	piano_photo_frame(c, &f);
}

int main(void)
{
	struct piano_photo_capture c;
	struct fake_hardware hw;
	struct piano_photo_result result;
	int sockets[2], image_fd = -1, count = 0;

	setup(&c, &hw, sockets); piano_photo_abort(&c, ECANCELED, false); close(sockets[1]);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
	assert(piano_photo_begin(&c, sockets[0], false, 0) == -EOPNOTSUPP);
	assert(!piano_photo_active(&c) && hw.freeze_count == 0 && hw.start_count == 0);
	assert(fcntl(sockets[0], F_GETFD) >= 0); close(sockets[0]); close(sockets[1]); count++;

	setup(&c, &hw, sockets); hw.ready = false; feed(&c, 1);
	assert(c.phase == PHOTO_WAIT_READY && hw.freeze_count == 0);
	piano_photo_abort(&c, ECANCELED, true); expect_error(sockets[1], ECANCELED);
	assert(hw.stop_count == 0 && hw.restore_count == 0); count++;

	setup(&c, &hw, sockets); illuminated(&c, &hw);
	struct piano_photo_frame old = frame(5);
	old.now_ns = 1030000000; old.eof_ns = 1029000000;
	old.exposure_start_ns = 995000000; old.exposure_end_ns = old.eof_ns;
	piano_photo_frame(&c, &old);
	assert(c.phase == PHOTO_LIT && hw.stop_count == 0);
	struct piano_photo_frame selected = frame(6);
	selected.now_ns = 1060000000; selected.eof_ns = 1055000000;
	selected.exposure_start_ns = 1010000000; selected.exposure_end_ns = selected.eof_ns;
	piano_photo_frame(&c, &selected);
	assert(!piano_photo_active(&c) && hw.stop_count == 1 && hw.restore_count == 1);
	assert(piano_photo_receive(sockets[1], &result, &image_fd) == 0 && result.status == 0);
	assert(result.sequence == 6 && result.capture_stride == 12 && result.stride == 8 && result.data_bytes == 48);
	assert(result.exposure_lines == 256 && result.native_width == 4080);
	assert((fcntl(image_fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
	uint8_t bytes[48]; assert(read(image_fd, bytes, sizeof(bytes)) == sizeof(bytes));
	for (unsigned int i = 0; i < sizeof(bytes); i++) assert(bytes[i] == 128);
	assert(pwrite(image_fd, "x", 1, 0) < 0 && ftruncate(image_fd, 0) < 0); count++;

	FILE *jpeg = tmpfile(); assert(jpeg);
	assert(piano_photo_jpeg(image_fd, &result, jpeg) == 0); rewind(jpeg);
	struct jpeg_decompress_struct decoder = {0}; struct jpeg_error_mgr jpeg_error;
	decoder.err = jpeg_std_error(&jpeg_error); jpeg_create_decompress(&decoder);
	jpeg_stdio_src(&decoder, jpeg); assert(jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK);
	jpeg_start_decompress(&decoder); assert(decoder.output_width == 8 && decoder.output_height == 4);
	uint8_t row[24]; JSAMPROW rows[1] = { row };
	while (decoder.output_scanline < decoder.output_height) {
		assert(jpeg_read_scanlines(&decoder, rows, 1) == 1);
		for (unsigned int i = 0; i < sizeof(row); i++) assert(row[i] >= 126 && row[i] <= 134);
	}
	jpeg_finish_decompress(&decoder); jpeg_destroy_decompress(&decoder);
	fclose(jpeg); close(image_fd); close(sockets[1]); count++;

	setup(&c, &hw, sockets); feed(&c, 1); struct piano_photo_frame bad = frame(2); bad.complete = false;
	piano_photo_frame(&c, &bad); expect_error(sockets[1], EIO);
	assert(hw.stop_count == 1 && hw.restore_count == 1 && hw.start_count == 0); count++;

	setup(&c, &hw, sockets); feed(&c, 1); feed(&c, 3); expect_error(sockets[1], EIO);
	assert(hw.stop_count == 1 && hw.restore_count == 1); count++;

	setup(&c, &hw, sockets); hw.settings.exposure_ns = 99000000;
	hw.settings.readout_ns = 95113082; hw.settings.frame_ns = 99480916;
	feed(&c, 1); expect_error(sockets[1], ERANGE);
	assert(hw.start_count == 0 && hw.stop_count == 1 && hw.restore_count == 1); count++;

	setup(&c, &hw, sockets); hw.start_error = -EIO;
	feed(&c, 1); feed(&c, 2); feed(&c, 3); feed(&c, 4); expect_error(sockets[1], EIO);
	assert(hw.start_count == 1 && hw.stop_count == 1 && hw.restore_count == 1); count++;

	setup(&c, &hw, sockets); illuminated(&c, &hw); hw.stop_error = -EIO;
	selected_frame(&c); expect_error(sockets[1], EIO); assert(hw.restore_count == 1); count++;

	setup(&c, &hw, sockets); illuminated(&c, &hw); hw.fault = 4;
	selected_frame(&c); expect_error(sockets[1], EIO); assert(hw.restore_count == 1); count++;

	setup(&c, &hw, sockets); illuminated(&c, &hw); hw.stop_ns = 1040000000;
	selected_frame(&c); expect_error(sockets[1], ETIME); count++;

	setup(&c, &hw, sockets); piano_photo_tick(&c, 5000000000); expect_error(sockets[1], ETIMEDOUT);
	assert(hw.stop_count == 0 && hw.restore_count == 0); count++;

	setup(&c, &hw, sockets); illuminated(&c, &hw);
	piano_photo_tick(&c, c.light_end_ns); expect_error(sockets[1], ETIMEDOUT);
	assert(hw.stop_count == 1 && hw.restore_count == 1); count++;

	setup(&c, &hw, sockets); illuminated(&c, &hw); close(sockets[1]);
	piano_photo_abort(&c, ECANCELED, false);
	assert(!piano_photo_active(&c) && hw.stop_count == 1 && hw.restore_count == 1); count++;

	setup(&c, &hw, sockets); bad = frame(1); bad.physical_timing = false;
	piano_photo_frame(&c, &bad); expect_error(sockets[1], EOPNOTSUPP); assert(hw.freeze_count == 0); count++;

	setup(&c, &hw, sockets); bad = frame(1); bad.eof_ns = bad.now_ns + 1;
	piano_photo_frame(&c, &bad); expect_error(sockets[1], EPROTO); assert(hw.freeze_count == 0); count++;

	printf("%d real capture/protocol/JPEG checks passed\n", count);
	return 0;
}
