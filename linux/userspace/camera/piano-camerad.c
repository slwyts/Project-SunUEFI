// SPDX-License-Identifier: MIT
// Copyright (c) 2026 bluseliu50 and piano mainline contributors
/*
 * piano-camerad: runs the Xiaomi Pad 8 Pro cameras through the SM8750 TFE
 * hardware ISP (the CAMSS pixel path, NV12 out) and feeds the frames into
 * v4l2loopback devices, which applications and PipeWire see as plain
 * webcams.
 *
 * A camera only streams while a reader streams from its loopback device
 * (v4l2loopback client usage events).  The daemon closes the control loop
 * the ISP lacks: auto exposure on the sensor (exposure lines, then analogue
 * gain) plus the ISP digital gain, and gray-world auto white balance on the
 * ISP white balance gains, both from a sparse sample of the output frames,
 * and contrast-detect autofocus where the camera has a focus actuator.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <time.h>

#include <linux/media.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <libyuv/scale.h>

#include "quad-worker.h"
#include "loopback-output.h"
#include "photo/timing.h"

/* v4l2loopback private event: capture side streaming started/stopped */
#define V4L2_EVENT_PRI_CLIENT_USAGE (V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1)
struct v4l2_event_client_usage {
	uint32_t count;
};

/* CAMSS keeps two hardware addresses; requeue each completed buffer. */
#define NUM_BUFS	2
#define AE_TARGET	112.0	/* mean luma, gamma encoded */
#define AE_PERIOD	3	/* frames between control updates */
#define AE_STARTUP_UPDATES 10	/* bounded acquisition, then steady tracking */
#define DGAIN_UNITY	1024
#define DGAIN_MAX	(8 * DGAIN_UNITY)
#define WB_UNITY	1024
#define SAT_UNITY	256

/* rendering defaults, overridable from the environment (see apply_look) */
#define DEF_SATURATION	100	/* percent */
#define DEF_CONTRAST	96	/* S curve share of the tone curve, of 256 */

struct capture_mode {
	unsigned int width, height, fps;
};

static const struct capture_mode rear_modes[] = {
	{ 4080, 3060, 30 }, { 4080, 2296, 30 },
	{ 3840, 2160, 60 }, { 8160, 6120, 10 },
};

static const struct capture_mode front_modes[] = {
	{ 3264, 2448, 30 }, { 3264, 1836, 30 }, { 6528, 4896, 10 },
};

struct cam_cfg {
	const char *name;
	const char *label;	/* v4l2loopback card label */
	const char *sensor;	/* entity name prefix */
	const char *phy;
	const char *csid;
	const char *vfe;	/* PIX line subdev */
	const char *video;	/* PIX line video node */
	const char *vcm;	/* focus actuator entity prefix, or NULL */
	unsigned int in_w, in_h;	/* sensor mode */
	unsigned int out_w, out_h;	/* ISP output (MN downscaler) */
	unsigned int max_out_w, max_out_h;
	const struct capture_mode *modes;
	unsigned int num_modes;
	bool hflip, vflip;	/* sensor readout flips for an upright image */
	int again_unit;		/* analogue gain control value for 1x */
	double fps;
	int ccm[9];		/* camera RGB to sRGB, Q10, row-major */
};

/* qcom-camss PIX line control: colour correction matrix, 3x3 s32, Q10 */
#define V4L2_CID_CAMSS_CCM	(V4L2_CID_USER_BASE | 0x1ff0)

static const struct cam_cfg cam_cfgs[] = {
	{
		.name = "rear", .label = "Rear Camera",
		.sensor = "s5kjn1", .phy = "msm_csiphy1",
		.csid = "msm_csid0", .vfe = "msm_vfe0_pix",
		.video = "msm_vfe0_video3", .vcm = "dw9768",
		.in_w = 4080, .in_h = 3060, .out_w = 1920, .out_h = 1440,
		.max_out_w = 4080, .max_out_h = 3060,
		.modes = rear_modes, .num_modes = sizeof(rear_modes) / sizeof(*rear_modes),
		.vflip = true, .again_unit = 1, .fps = 30,
		/* mean of the stock tuning matrices */
		.ccm = { 1732, -667, -40, -230, 1337, -82, -93, -989, 2106 },
	},
	{
		.name = "front", .label = "Front Camera",
		.sensor = "ov32d40", .phy = "msm_csiphy4",
		.csid = "msm_csid1", .vfe = "msm_vfe1_pix",
		.video = "msm_vfe1_video3",
		.in_w = 3264, .in_h = 2448, .out_w = 1920, .out_h = 1440,
		.max_out_w = 3264, .max_out_h = 2448,
		.modes = front_modes, .num_modes = sizeof(front_modes) / sizeof(*front_modes),
		.again_unit = 256, .fps = 30,
		.ccm = { 1649, -375, -251, -245, 1525, -255, -169, -308, 1501 },
	},
};

#define NUM_CAMS (sizeof(cam_cfgs) / sizeof(cam_cfgs[0]))

struct buf {
	void *ptr;
	size_t len;
};

#define AF_HIST		64	/* sharpness samples kept per scan */

#define PHOTO_FLASH_CHANNELS 2
struct camera_flash {
	int fd, mode, source, timeout, intensity;
	struct v4l2_query_ext_ctrl timeout_range;
	bool saved, configured;
};

struct cam {
	struct cam_cfg mode;
	const struct cam_cfg *cfg;
	int loop_fd;
	struct loopback_output loop_output;
	bool streaming;
	int vid_fd, sensor_fd, vfe_fd, vcm_fd;
	struct buf bufs[NUM_BUFS];
	unsigned int bpl, sizeimage, nbufs;
	size_t uv_offset, frame_span;
	uint8_t *stage;		/* repacked frame when bpl != width */
	/* Only the full rear Quad CFA mode uses RDI and CPU conversion. */
	bool quad_raw;
	struct quad_worker *quad_worker;
	struct timeval quad_timestamp;
	uint32_t quad_timestamp_flags;
	unsigned long frames;
	/* sensor control ranges */
	int exp_min, exp_max, ag_min, ag_max;
	/* auto exposure / white balance state, kept across sessions */
	int exposure, again, dgain;
	double rgain, bgain;
	bool have_state;
	/* Ownership is independent for exposure, white balance and rear focus. */
	bool ae_manual, wb_manual, af_manual;
	/* exposure statistics are fresh for each stream */
	double ae_mean, ae_bright;
	unsigned int ae_updates, ae_stable;
	bool ae_acquiring, ae_ready;
	/* autofocus: lens position range, scan state */
	int af_min, af_max, af_pos;
	enum { AF_OFF, AF_WAIT, AF_SCAN, AF_FINE, AF_VERIFY, AF_LOCKED } af_state;
	int af_step, af_dir, af_origin, af_drops, af_best_pos, af_settle;
	int af_lost, af_n;
	bool af_reversed;
	double af_best, af_floor, af_lock;
	int af_fine[3];			/* fine pass positions */
	int af_hist_pos[AF_HIST];	/* samples of the current scan */
	double af_hist_val[AF_HIST];
	struct piano_photo_capture photo;
	struct camera_flash flash[PHOTO_FLASH_CHANNELS];
	unsigned int flash_count, loop_users;
	unsigned int photo_recover_frames;
	bool photo_owned_stream, saved_ae_manual, saved_wb_manual, saved_af_manual;
	uint32_t colorspace, ycbcr_enc, quantization;
	struct piano_photo_timing photo_timing;
	bool frame_sync_subscribed;
};

static struct cam cams[NUM_CAMS];
static volatile sig_atomic_t quit;
static int exit_status;
static double srgb_lin[256];
static int saturation = DEF_SATURATION * SAT_UNITY / 100;
static int contrast = DEF_CONTRAST;
static int af_settle_frames;

static const struct piano_photo_ops camera_photo_ops;
static int camera_photo_request(struct cam *c, int client_fd);
static void camera_photo_frame(struct cam *c, const struct v4l2_buffer *buffer,
			       const uint8_t *y, const uint8_t *uv, bool complete);

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void logmsg(const struct cam *c, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s%s", c ? c->cfg->name : "piano-camerad",
		c ? ": " : ": ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

/* ------------------------------------------------------------------ */
/* media controller                                                   */

struct topo {
	int fd;
	struct media_v2_topology t;
	struct media_v2_entity *ents;
	struct media_v2_interface *ifs;
	struct media_v2_pad *pads;
	struct media_v2_link *links;
};

static void topo_free(struct topo *tp)
{
	free(tp->ents);
	free(tp->ifs);
	free(tp->pads);
	free(tp->links);
	memset(tp, 0, sizeof(*tp));
}

static int topo_load(struct topo *tp, int fd)
{
	memset(tp, 0, sizeof(*tp));
	tp->fd = fd;
	if (xioctl(fd, MEDIA_IOC_G_TOPOLOGY, &tp->t) < 0)
		return -1;

	tp->ents = calloc(tp->t.num_entities, sizeof(*tp->ents));
	tp->ifs = calloc(tp->t.num_interfaces, sizeof(*tp->ifs));
	tp->pads = calloc(tp->t.num_pads, sizeof(*tp->pads));
	tp->links = calloc(tp->t.num_links, sizeof(*tp->links));
	tp->t.ptr_entities = (uintptr_t)tp->ents;
	tp->t.ptr_interfaces = (uintptr_t)tp->ifs;
	tp->t.ptr_pads = (uintptr_t)tp->pads;
	tp->t.ptr_links = (uintptr_t)tp->links;

	if (xioctl(fd, MEDIA_IOC_G_TOPOLOGY, &tp->t) < 0) {
		topo_free(tp);
		return -1;
	}

	return 0;
}

static const struct media_v2_entity *topo_entity(const struct topo *tp,
						 const char *prefix)
{
	size_t n = strlen(prefix);

	for (unsigned int i = 0; i < tp->t.num_entities; i++)
		if (!strncmp(tp->ents[i].name, prefix, n) &&
		    (tp->ents[i].name[n] == '\0' || tp->ents[i].name[n] == ' '))
			return &tp->ents[i];

	return NULL;
}

static const struct media_v2_pad *topo_pad(const struct topo *tp,
					   uint32_t id)
{
	for (unsigned int i = 0; i < tp->t.num_pads; i++)
		if (tp->pads[i].id == id)
			return &tp->pads[i];

	return NULL;
}

static int topo_open_devnode(const struct topo *tp, uint32_t entity_id)
{
	for (unsigned int i = 0; i < tp->t.num_links; i++) {
		const struct media_v2_link *l = &tp->links[i];
		char path[64], line[256], dev[128] = "";
		FILE *f;

		if ((l->flags & MEDIA_LNK_FL_LINK_TYPE) !=
		    MEDIA_LNK_FL_INTERFACE_LINK || l->sink_id != entity_id)
			continue;

		for (unsigned int j = 0; j < tp->t.num_interfaces; j++) {
			const struct media_v2_interface *in = &tp->ifs[j];

			if (in->id != l->source_id)
				continue;

			snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/uevent",
				 in->devnode.major, in->devnode.minor);
			f = fopen(path, "r");
			if (!f)
				return -1;
			while (fgets(line, sizeof(line), f))
				if (!strncmp(line, "DEVNAME=", 8)) {
					line[strcspn(line, "\n")] = '\0';
					snprintf(dev, sizeof(dev), "/dev/%s",
						 line + 8);
				}
			fclose(f);
			if (!dev[0])
				return -1;

			return open(dev, O_RDWR | O_CLOEXEC);
		}
	}

	errno = ENOENT;
	return -1;
}

static int setup_link(struct topo *tp, uint32_t src_ent, uint16_t src_pad,
		      uint32_t sink_ent, uint16_t sink_pad, bool enable)
{
	struct media_link_desc ld = { 0 };

	ld.source.entity = src_ent;
	ld.source.index = src_pad;
	ld.sink.entity = sink_ent;
	ld.sink.index = sink_pad;
	ld.flags = enable ? MEDIA_LNK_FL_ENABLED : 0;

	return xioctl(tp->fd, MEDIA_IOC_SETUP_LINK, &ld);
}

/*
 * Enable src:src_pad -> sink:sink_pad, disabling other enabled links on
 * either pad first (a CSID sink takes one PHY, a PHY feeds one CSID).
 */
static int route(struct topo *tp, const struct media_v2_entity *src,
		 uint16_t src_pad, const struct media_v2_entity *sink,
		 uint16_t sink_pad)
{
	for (unsigned int i = 0; i < tp->t.num_links; i++) {
		const struct media_v2_link *l = &tp->links[i];
		const struct media_v2_pad *ps, *pk;
		bool on_src, on_sink;

		if ((l->flags & MEDIA_LNK_FL_LINK_TYPE) !=
		    MEDIA_LNK_FL_DATA_LINK ||
		    !(l->flags & MEDIA_LNK_FL_ENABLED) ||
		    (l->flags & MEDIA_LNK_FL_IMMUTABLE))
			continue;

		ps = topo_pad(tp, l->source_id);
		pk = topo_pad(tp, l->sink_id);
		if (!ps || !pk)
			continue;

		on_src = ps->entity_id == src->id && ps->index == src_pad;
		on_sink = pk->entity_id == sink->id && pk->index == sink_pad;
		if (on_src && on_sink)
			return 0;
		if (on_src || on_sink)
			setup_link(tp, ps->entity_id, ps->index, pk->entity_id,
				   pk->index, false);
	}

	return setup_link(tp, src->id, src_pad, sink->id, sink_pad, true);
}

/* PIX and RDI are different CSID pads. Disable the old output before changing
 * paths; route() alone only resolves conflicts on the requested pad.
 * Keep other CSIDs, including an active front camera, untouched.
 */
static int csid_output(struct topo *tp, const struct media_v2_entity *csid,
		       unsigned int pad)
{
	for (unsigned int i = 0; i < tp->t.num_links; i++) {
		const struct media_v2_link *link = &tp->links[i];
		const struct media_v2_pad *src, *sink;

		if ((link->flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_DATA_LINK ||
		    !(link->flags & MEDIA_LNK_FL_ENABLED) ||
		    (link->flags & MEDIA_LNK_FL_IMMUTABLE))
			continue;
		src = topo_pad(tp, link->source_id);
		sink = topo_pad(tp, link->sink_id);
		if (!src || !sink || src->entity_id != csid->id || src->index == pad)
			continue;
		if (setup_link(tp, src->entity_id, src->index, sink->entity_id,
			       sink->index, false) < 0)
			return -1;
	}
	return 0;
}

static int find_media(void)
{
	char path[32];

	for (int i = 0; i < 16; i++) {
		struct media_device_info info;
		int fd;

		snprintf(path, sizeof(path), "/dev/media%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!xioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) &&
		    !strcmp(info.driver, "qcom-camss"))
			return fd;
		close(fd);
	}

	errno = ENODEV;
	return -1;
}

/* ------------------------------------------------------------------ */
/* V4L2 helpers                                                       */

static int subdev_fmt(int fd, unsigned int pad, unsigned int w,
		      unsigned int h, uint32_t *code)
{
	struct v4l2_subdev_format f = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = pad,
	};

	if (xioctl(fd, VIDIOC_SUBDEV_G_FMT, &f) < 0)
		return -1;
	f.format.width = w;
	f.format.height = h;
	if (*code)
		f.format.code = *code;
	if (xioctl(fd, VIDIOC_SUBDEV_S_FMT, &f) < 0)
		return -1;
	*code = f.format.code;

	return f.format.width == w && f.format.height == h ? 0 : -1;
}

/* Raw sensors select the nearest hardware mode. Propagate that returned size
 * through CAMSS instead of assuming that every kernel uses identical tables.
 */
static int sensor_format(struct cam *c, uint32_t *code)
{
	struct v4l2_subdev_format f = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
		.format = { .width = c->cfg->in_w, .height = c->cfg->in_h },
	};

	if (xioctl(c->sensor_fd, VIDIOC_SUBDEV_G_FMT, &f) < 0)
		return -1;
	f.format.width = c->cfg->in_w;
	f.format.height = c->cfg->in_h;
	if (xioctl(c->sensor_fd, VIDIOC_SUBDEV_S_FMT, &f) < 0)
		return -1;
	c->mode.in_w = f.format.width;
	c->mode.in_h = f.format.height;
	*code = f.format.code;
	return 0;
}

static int ctrl_set(int fd, uint32_t id, int val)
{
	struct v4l2_control c = { .id = id, .value = val };

	return xioctl(fd, VIDIOC_S_CTRL, &c);
}

static int ctrl_range(int fd, uint32_t id, int *min, int *max)
{
	struct v4l2_queryctrl q = { .id = id };

	if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0)
		return -1;
	*min = q.minimum;
	*max = q.maximum;

	return 0;
}

static int open_loopback(const char *label)
{
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *e;
	int fd = -1;

	if (!d)
		return -1;

	while ((e = readdir(d))) {
		char path[300], name[64] = "";
		FILE *f;

		if (strncmp(e->d_name, "video", 5))
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name",
			 e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, "\n")] = '\0';
		fclose(f);
		if (strcmp(name, label))
			continue;

		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		break;
	}
	closedir(d);

	return fd;
}

/* make udev (and so PipeWire) look at the device again */
static void loopback_uevent(int fd)
{
	struct stat st;
	char path[64];
	int ufd;

	if (fstat(fd, &st) < 0)
		return;
	snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/uevent",
		 major(st.st_rdev), minor(st.st_rdev));
	ufd = open(path, O_WRONLY | O_CLOEXEC);
	if (ufd < 0)
		return;
	if (write(ufd, "change", 6) < 0)
		;
	close(ufd);
}

static size_t frame_size(const struct cam_cfg *cfg)
{
	return (size_t)cfg->out_w * cfg->out_h * 3 / 2;
}

static int loopback_write_frame(struct cam *c, const uint8_t *frame,
				const struct timeval *timestamp, uint32_t flags)
{
	int result = loopback_output_publish(&c->loop_output, frame,
					     frame_size(c->cfg), timestamp, flags);

	if (result < 0) {
		errno = -result;
		return -1;
	}
	return 0;
}

static void loopback_close(struct cam *c)
{
	int result = loopback_output_clear(&c->loop_output);

	if (result < 0)
		logmsg(c, "loopback queue cleanup: %s", strerror(-result));
	if (c->loop_fd >= 0)
		close(c->loop_fd);
	c->loop_fd = -1;
}

static int loopback_black(struct cam *c)
{
	const struct cam_cfg *cfg = c->cfg;
	size_t size = frame_size(cfg), luma = (size_t)cfg->out_w * cfg->out_h;
	uint8_t *black = malloc(size);
	int ret;

	if (!black)
		return -1;
	memset(black, 0, luma);
	memset(black + luma, 128, size - luma);
	struct timespec now;
	struct timeval timestamp;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		free(black);
		return -1;
	}
	timestamp = (struct timeval){ .tv_sec = now.tv_sec, .tv_usec = now.tv_nsec / 1000 };
	ret = loopback_write_frame(c, black, &timestamp, V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);
	free(black);

	return ret;
}

static int loopback_format(struct cam *c, unsigned int width, unsigned int height)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	size_t size = (size_t)width * height * 3 / 2;

	f.fmt.pix.width = width;
	f.fmt.pix.height = height;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = width;
	f.fmt.pix.sizeimage = size;
	/* full range BT.601 YCbCr, sRGB transfer */
	f.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
	if (xioctl(c->loop_fd, VIDIOC_S_FMT, &f) < 0 ||
	    xioctl(c->loop_fd, VIDIOC_G_FMT, &f) < 0) {
		logmsg(c, "loopback format: %s", strerror(errno));
		return -1;
	}
	if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
	    f.fmt.pix.width != width || f.fmt.pix.height != height ||
	    f.fmt.pix.bytesperline != width || f.fmt.pix.sizeimage != size) {
		logmsg(c, "loopback did not accept tightly packed NV12 %ux%u", width,
		       height);
		return -1;
	}
	return 0;
}

static int loopback_rate(struct cam *c, unsigned int fps)
{
	struct v4l2_streamparm p = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.parm.output.timeperframe = { .numerator = 1, .denominator = fps },
	};

	if (xioctl(c->loop_fd, VIDIOC_S_PARM, &p) < 0)
		return -1;
	if ((uint64_t)p.parm.output.timeperframe.numerator * fps !=
	    p.parm.output.timeperframe.denominator) {
		errno = ERANGE;
		return -1;
	}
	return 0;
}

static int select_capture_mode(struct cam_cfg *cfg, unsigned int width,
			       unsigned int height, unsigned int fps)
{
	/* Prefer an exact sensor mode before choosing a source for scaled output. */
	for (unsigned int pass = 0; pass < 2; ++pass) {
		for (unsigned int i = 0; i < cfg->num_modes; ++i) {
			const struct capture_mode *mode = &cfg->modes[i];

			if (mode->fps != fps || width > mode->width || height > mode->height)
				continue;
			if (!pass && (width != mode->width || height != mode->height))
				continue;
			cfg->in_w = cfg->max_out_w = mode->width;
			cfg->in_h = cfg->max_out_h = mode->height;
			cfg->out_w = width;
			cfg->out_h = height;
			cfg->fps = fps;
			return 0;
		}
	}
	errno = EINVAL;
	return -1;
}

static int loopback_init(struct cam *c)
{
	const struct cam_cfg *cfg = c->cfg;
	struct v4l2_event_subscription sub = {
		.type = V4L2_EVENT_PRI_CLIENT_USAGE,
	};
	struct v4l2_event_subscription changes = { .type = V4L2_EVENT_SOURCE_CHANGE };

	c->loop_fd = open_loopback(cfg->label);
	if (c->loop_fd < 0) {
		logmsg(c, "no v4l2loopback device \"%s\"", cfg->label);
		return -1;
	}
	/* Before the first frame, register each sensor mode's size and interval.
	 * Readers then use ordinary V4L2 S_FMT/S_PARM, not a second camera entry.
	 */
	for (unsigned int i = 0; i < cfg->num_modes; ++i) {
		const struct capture_mode *mode = &cfg->modes[i];

		if (loopback_format(c, mode->width, mode->height) < 0 ||
		    loopback_rate(c, mode->fps) < 0 ||
		    xioctl(c->loop_fd, VIDIOC_SUBSCRIBE_EVENT, &changes) < 0)
			return -1;
	}
	if (loopback_format(c, cfg->out_w, cfg->out_h) < 0 ||
	    loopback_rate(c, (unsigned int)cfg->fps) < 0 ||
	    xioctl(c->loop_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
		return -1;

	int result = loopback_output_setup(&c->loop_output, c->loop_fd, frame_size(cfg));
	if (result < 0) {
		errno = -result;
		return -1;
	}

	/* one black frame: the device turns into a capture device */
	if (loopback_black(c) < 0)
		return -1;
	loopback_uevent(c->loop_fd);
	return 0;
}

/* ------------------------------------------------------------------ */
/* camera pipeline                                                    */

static void apply_exposure(struct cam *c)
{
	ctrl_set(c->sensor_fd, V4L2_CID_EXPOSURE, c->exposure);
	ctrl_set(c->sensor_fd, V4L2_CID_ANALOGUE_GAIN, c->again);
	if (!c->quad_raw)
		ctrl_set(c->vfe_fd, V4L2_CID_DIGITAL_GAIN, c->dgain);
}

static void apply_wb(struct cam *c)
{
	if (c->quad_raw)
		return;
	ctrl_set(c->vfe_fd, V4L2_CID_RED_BALANCE,
		 (int)lround(c->rgain * WB_UNITY));
	ctrl_set(c->vfe_fd, V4L2_CID_BLUE_BALANCE,
		 (int)lround(c->bgain * WB_UNITY));
}

static int cam_stop(struct cam *c)
{
	struct v4l2_requestbuffers rb = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	piano_photo_abort(&c->photo, EPIPE, true);
	if (c->vid_fd >= 0) {
		if (xioctl(c->vid_fd, VIDIOC_STREAMOFF, &type) < 0 && c->streaming) {
			logmsg(c, "STREAMOFF: %s; retaining capture buffers", strerror(errno));
			return -1;
		}
		for (int i = 0; i < NUM_BUFS; i++)
			if (c->bufs[i].ptr)
				munmap(c->bufs[i].ptr, c->bufs[i].len);
		xioctl(c->vid_fd, VIDIOC_REQBUFS, &rb);
		close(c->vid_fd);
	}
	if (c->sensor_fd >= 0)
		close(c->sensor_fd);
	if (c->vcm_fd >= 0)
		close(c->vcm_fd);
	if (c->vfe_fd >= 0)
		close(c->vfe_fd);
	free(c->stage);

	memset(c->bufs, 0, sizeof(c->bufs));
	c->stage = NULL;
	c->nbufs = c->bpl = c->sizeimage = 0;
	c->uv_offset = c->frame_span = 0;
	c->vid_fd = c->sensor_fd = c->vfe_fd = c->vcm_fd = -1;
	if (c->streaming)
		logmsg(c, "stopped after %lu frames", c->frames);
	c->streaming = false;
	c->photo_owned_stream = false;
	c->frame_sync_subscribed = false;
	piano_photo_timing_reset(&c->photo_timing);
	/* The worker owns a copied frame only. Release capture DMA first, then
	 * wait for its current CPU conversion before freeing the workspace.
	 */
	int result = quad_worker_stop(c->quad_worker);
	if (result < 0) {
		errno = -result;
		return -1;
	}
	c->quad_worker = NULL;
	c->quad_raw = false;
	return 0;
}

/* Full JN1 readout is Quad CFA: the four colours describe 2x2 same-colour
 * blocks. Use the order accepted after the sensor's actual flip controls;
 * the ordinary 2x2 Bayer demosaic of the TFE PIX line is unsuitable here.
 */
static int quad_format(uint32_t code, uint32_t *fourcc, uint32_t *cfa)
{
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		*fourcc = V4L2_PIX_FMT_SRGGB10P;
		*cfa = PIANO_QUAD_RGGB;
		return 0;
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		*fourcc = V4L2_PIX_FMT_SBGGR10P;
		*cfa = PIANO_QUAD_BGGR;
		return 0;
	case MEDIA_BUS_FMT_SGRBG10_1X10:
		*fourcc = V4L2_PIX_FMT_SGRBG10P;
		*cfa = PIANO_QUAD_GRBG;
		return 0;
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		*fourcc = V4L2_PIX_FMT_SGBRG10P;
		*cfa = PIANO_QUAD_GBRG;
		return 0;
	default:
		errno = EOPNOTSUPP;
		return -1;
	}
}

static int capture_layout(struct cam *c)
{
	const struct cam_cfg *cfg = c->cfg;

	if (c->quad_raw) {
		size_t row_bytes = (size_t)cfg->in_w * 5 / 4;

		/* G_FMT supplies the real DMA stride; RAW10 row padding is not
		 * part of the image and must not be guessed from the sensor width.
		 */
		c->uv_offset = 0;
		c->frame_span = (size_t)(cfg->in_h - 1) * c->bpl + row_bytes;
		if (c->bpl < row_bytes || c->sizeimage < c->frame_span) {
			errno = EINVAL;
			logmsg(c, "invalid RAW10 layout: stride %u sizeimage %u span %zu",
			       c->bpl, c->sizeimage, c->frame_span);
			return -1;
		}
		if (cfg->out_w != cfg->in_w || cfg->out_h != cfg->in_h) {
			c->stage = malloc(frame_size(cfg));
			if (!c->stage)
				return -1;
		}
		return 0;
	}

	c->uv_offset = (size_t)c->bpl * cfg->out_h;
	c->frame_span = c->uv_offset + (size_t)(cfg->out_h / 2 - 1) * c->bpl +
			cfg->out_w;
	if (c->bpl < cfg->out_w || c->sizeimage < c->frame_span) {
		errno = EINVAL;
		logmsg(c, "invalid NV12 layout: stride %u sizeimage %u span %zu", c->bpl,
		       c->sizeimage, c->frame_span);
		return -1;
	}
	if (c->bpl != cfg->out_w) {
		c->stage = malloc(frame_size(cfg));
		if (!c->stage)
			return -1;
	}
	return 0;
}

/* Scale the whole sensor image with a common aspect ratio, then crop in the
 * scaled YUV image. The TFE has at most 16x downscaling and a 16-pixel write
 * alignment. Very small outputs therefore crop within its minimum-size image.
 */
static int isp_geometry(const struct cam_cfg *cfg, struct v4l2_rect *compose,
			struct v4l2_rect *crop)
{
	uint64_t width = cfg->out_w, height = cfg->out_h;

	if (!width || !height || width > cfg->max_out_w || height > cfg->max_out_h ||
	    (width & 15) || (height & 1))
		return -1;
	if (width * cfg->in_h < height * cfg->in_w)
		width = (height * cfg->in_w + cfg->in_h - 1) / cfg->in_h;
	else
		height = (width * cfg->in_h + cfg->in_w - 1) / cfg->in_w;
	if (width * 16 < cfg->in_w) {
		width = (cfg->in_w + 15) / 16;
		height = (width * cfg->in_h + cfg->in_w - 1) / cfg->in_w;
	}
	if (height * 16 < cfg->in_h) {
		height = (cfg->in_h + 15) / 16;
		width = (height * cfg->in_w + cfg->in_h - 1) / cfg->in_h;
	}
	width = (width + 1) & ~UINT64_C(1);
	height = (height + 1) & ~UINT64_C(1);
	*compose = (struct v4l2_rect){ .width = width, .height = height };
	*crop = (struct v4l2_rect){
		.left = ((width - cfg->out_w) / 2) & ~UINT64_C(1),
		.top = ((height - cfg->out_h) / 2) & ~UINT64_C(1),
		.width = cfg->out_w, .height = cfg->out_h,
	};
	return 0;
}

static int sensor_frame_rate(struct cam *c)
{
	struct v4l2_control blank = { .id = V4L2_CID_HBLANK };
	struct v4l2_ext_control rate = { .id = V4L2_CID_PIXEL_RATE };
	struct v4l2_ext_controls controls = { .count = 1, .controls = &rate };
	struct v4l2_query_ext_ctrl vertical = { .id = V4L2_CID_VBLANK };
	uint64_t line, frame;

	/* OV32D40 uses fixed factory timing, expressed as a subdev interval.
	 * Its CSI transport rate cannot convert native FLL counts to output rows.
	 */
	if (!strcmp(c->cfg->sensor, "ov32d40")) {
		struct v4l2_subdev_frame_interval interval = { .pad = 0 };

		if (xioctl(c->sensor_fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &interval) < 0)
			return -1;
		if ((uint64_t)interval.interval.numerator * (unsigned int)c->cfg->fps !=
		    interval.interval.denominator) {
			errno = ERANGE;
			return -1;
		}
		return 0;
	}
	if (xioctl(c->sensor_fd, VIDIOC_G_CTRL, &blank) < 0 ||
	    xioctl(c->sensor_fd, VIDIOC_G_EXT_CTRLS, &controls) < 0 ||
	    xioctl(c->sensor_fd, VIDIOC_QUERY_EXT_CTRL, &vertical) < 0)
		return -1;
	if (blank.value < 0 || rate.value64 <= 0 || vertical.minimum < 0) {
		errno = ERANGE;
		return -1;
	}
	line = ((uint64_t)c->cfg->in_w + blank.value) * (unsigned int)c->cfg->fps;
	frame = ((uint64_t)rate.value64 + line - 1) / line;
	if (frame < c->cfg->in_h + (uint64_t)vertical.minimum ||
	    frame > c->cfg->in_h + (uint64_t)vertical.maximum) {
		errno = ERANGE;
		return -1;
	}
	return ctrl_set(c->sensor_fd, V4L2_CID_VBLANK, frame - c->cfg->in_h);
}

static int cam_start(struct cam *c)
{
	const struct cam_cfg *cfg = c->cfg;
	const struct media_v2_entity *sen, *phy, *csid, *vfe, *vid;
	struct topo tp;
	int mfd, phy_fd = -1, csid_fd = -1, ret = -1;
	uint32_t code = 0, yuv = MEDIA_BUS_FMT_YUYV8_1_5X8;
	uint32_t raw_fourcc = 0, quad_cfa = 0;
	bool raw = !strcmp(cfg->sensor, "s5kjn1") && cfg->in_w == 8160 &&
		   cfg->in_h == 6120;
	unsigned int csid_pad = raw ? 1 : 4;
	struct v4l2_subdev_selection sel = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
		.target = V4L2_SEL_TGT_COMPOSE,
	};
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	struct v4l2_requestbuffers rb = {
		.count = NUM_BUFS,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_rect compose, crop;

	if (c->quad_worker) {
		errno = EBUSY;
		return -1;
	}
	c->quad_raw = raw;
	c->vid_fd = c->sensor_fd = c->vfe_fd = c->vcm_fd = -1;

	mfd = find_media();
	if (mfd < 0) {
		logmsg(c, "no CAMSS media device");
		return -1;
	}
	if (topo_load(&tp, mfd) < 0) {
		logmsg(c, "media topology: %s", strerror(errno));
		close(mfd);
		return -1;
	}

	sen = topo_entity(&tp, cfg->sensor);
	phy = topo_entity(&tp, cfg->phy);
	csid = topo_entity(&tp, cfg->csid);
	vfe = topo_entity(&tp, raw ? "msm_vfe0_rdi0" : cfg->vfe);
	vid = topo_entity(&tp, raw ? "msm_vfe0_video0" : cfg->video);
	if (!sen || !phy || !csid || !vfe || !vid) {
		logmsg(c, "media entities missing");
		goto out;
	}

	c->sensor_fd = topo_open_devnode(&tp, sen->id);
	phy_fd = topo_open_devnode(&tp, phy->id);
	csid_fd = topo_open_devnode(&tp, csid->id);
	c->vfe_fd = topo_open_devnode(&tp, vfe->id);
	c->vid_fd = topo_open_devnode(&tp, vid->id);
	if (c->vid_fd >= 0)
		fcntl(c->vid_fd, F_SETFL, O_NONBLOCK);
	if (c->sensor_fd < 0 || phy_fd < 0 || csid_fd < 0 || c->vfe_fd < 0 ||
	    c->vid_fd < 0) {
		logmsg(c, "cannot open device nodes: %s", strerror(errno));
		goto out;
	}

	/* optional: without the actuator the lens stays where it is */
	c->af_state = AF_OFF;
	if (cfg->vcm) {
		const struct media_v2_entity *lens = topo_entity(&tp, cfg->vcm);

		if (lens)
			c->vcm_fd = topo_open_devnode(&tp, lens->id);
		if (c->vcm_fd >= 0 &&
		    ctrl_range(c->vcm_fd, V4L2_CID_FOCUS_ABSOLUTE,
			       &c->af_min, &c->af_max) == 0) {
			struct v4l2_control ctl = {
				.id = V4L2_CID_FOCUS_ABSOLUTE,
			};

			/* the climb starts where the lens rests */
			if (xioctl(c->vcm_fd, VIDIOC_G_CTRL, &ctl) == 0) {
				c->af_pos = ctl.value;
				c->af_settle = 0;
				c->af_state = AF_WAIT;
			} else {
				logmsg(c, "focus position: %s", strerror(errno));
			}
		}
	}

	if (route(&tp, phy, 1, csid, 0) < 0 ||
	    csid_output(&tp, csid, csid_pad) < 0 ||
	    route(&tp, csid, csid_pad, vfe, 0) < 0) {
		logmsg(c, "media links: %s", strerror(errno));
		goto out;
	}

	/* the flips change the Bayer order the sensor reports */
	ctrl_set(c->sensor_fd, V4L2_CID_HFLIP, cfg->hflip);
	ctrl_set(c->sensor_fd, V4L2_CID_VFLIP, cfg->vflip);

	if (sensor_format(c, &code) < 0 ||
	    sensor_frame_rate(c) < 0 ||
	    isp_geometry(cfg, &compose, &crop) < 0 ||
	    subdev_fmt(phy_fd, 0, cfg->in_w, cfg->in_h, &code) < 0 ||
	    subdev_fmt(csid_fd, 0, cfg->in_w, cfg->in_h, &code) < 0 ||
	    subdev_fmt(csid_fd, csid_pad, cfg->in_w, cfg->in_h, &code) < 0 ||
	    subdev_fmt(c->vfe_fd, 0, cfg->in_w, cfg->in_h, &code) < 0) {
		logmsg(c, "sensor side formats: %s", strerror(errno));
		goto out;
	}

	if (raw) {
		/* RDI retains every sensor sample. Its source format remains RAW10;
		 * no PIX compose, crop, colour control or Bayer ISP is involved.
		 */
		if (quad_format(code, &raw_fourcc, &quad_cfa) < 0 ||
		    subdev_fmt(c->vfe_fd, 1, cfg->in_w, cfg->in_h, &code) < 0)
			goto out;
	} else {
		sel.r = compose;
		if (xioctl(c->vfe_fd, VIDIOC_SUBDEV_S_SELECTION, &sel) < 0 ||
		    sel.r.width != compose.width || sel.r.height != compose.height) {
			logmsg(c, "ISP compose size: %s", strerror(errno));
			goto out;
		}
		sel.pad = 1;
		sel.target = V4L2_SEL_TGT_CROP;
		sel.r = crop;
		if (xioctl(c->vfe_fd, VIDIOC_SUBDEV_S_SELECTION, &sel) < 0 ||
		    sel.r.width != crop.width || sel.r.height != crop.height ||
		    sel.r.left != crop.left || sel.r.top != crop.top ||
		    subdev_fmt(c->vfe_fd, 1, cfg->out_w, cfg->out_h, &yuv) < 0) {
			logmsg(c, "ISP output size: %s", strerror(errno));
			goto out;
		}
	}

	f.fmt.pix_mp.width = raw ? cfg->in_w : cfg->out_w;
	f.fmt.pix_mp.height = raw ? cfg->in_h : cfg->out_h;
	f.fmt.pix_mp.pixelformat = raw ? raw_fourcc : V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	if (xioctl(c->vid_fd, VIDIOC_S_FMT, &f) < 0 ||
	    xioctl(c->vid_fd, VIDIOC_G_FMT, &f) < 0) {
		logmsg(c, "video format: %s", strerror(errno));
		goto out;
	}
	if (f.fmt.pix_mp.pixelformat != (raw ? raw_fourcc : V4L2_PIX_FMT_NV12) ||
	    f.fmt.pix_mp.num_planes != 1 ||
	    f.fmt.pix_mp.width != (raw ? cfg->in_w : cfg->out_w) ||
	    f.fmt.pix_mp.height != (raw ? cfg->in_h : cfg->out_h) ||
	    (cfg->out_w & 1) || (cfg->out_h & 1)) {
		logmsg(c, "capture did not accept single-plane %s %ux%u",
		       raw ? "RAW10" : "NV12", cfg->out_w, cfg->out_h);
		goto out;
	}
	c->colorspace = f.fmt.pix_mp.colorspace;
	c->ycbcr_enc = f.fmt.pix_mp.ycbcr_enc;
	c->quantization = f.fmt.pix_mp.quantization;
	c->bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	c->sizeimage = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	if (capture_layout(c) < 0)
		goto out;
	if (raw) {
		int result = quad_worker_create(cfg->in_w, cfg->in_h, quad_cfa,
						&c->quad_worker);
		if (result < 0) {
			errno = -result;
			logmsg(c, "Quad CFA converter: %s", strerror(errno));
			goto out;
		}
	}

	if (xioctl(c->vid_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2 ||
	    rb.count > NUM_BUFS) {
		logmsg(c, "REQBUFS: count %u, %s", rb.count, strerror(errno));
		goto out;
	}
	c->nbufs = rb.count;
	for (unsigned int i = 0; i < c->nbufs; i++) {
		struct v4l2_plane pl[VIDEO_MAX_PLANES] = { 0 };
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
			.length = VIDEO_MAX_PLANES,
			.m.planes = pl,
		};

		if (xioctl(c->vid_fd, VIDIOC_QUERYBUF, &b) < 0)
			goto out;
		if (b.length != 1 || pl[0].length < c->sizeimage) {
			logmsg(c, "invalid capture buffer %u: planes %u length %u", i,
			       b.length, pl[0].length);
			goto out;
		}
		c->bufs[i].len = pl[0].length;
		c->bufs[i].ptr = mmap(NULL, pl[0].length, PROT_READ,
				      MAP_SHARED, c->vid_fd,
				      pl[0].m.mem_offset);
		if (c->bufs[i].ptr == MAP_FAILED) {
			c->bufs[i].ptr = NULL;
			goto out;
		}
		if (xioctl(c->vid_fd, VIDIOC_QBUF, &b) < 0)
			goto out;
	}

	ctrl_range(c->sensor_fd, V4L2_CID_EXPOSURE, &c->exp_min, &c->exp_max);
	ctrl_range(c->sensor_fd, V4L2_CID_ANALOGUE_GAIN, &c->ag_min,
		   &c->ag_max);
	if (!c->have_state) {
		c->exposure = c->exp_max / 2;
		c->again = cfg->again_unit * 2;
		c->dgain = DGAIN_UNITY;
		c->rgain = 1.8;
		c->bgain = 1.6;
		c->have_state = true;
	}
	c->exposure = c->exposure < c->exp_min ? c->exp_min :
		      c->exposure > c->exp_max ? c->exp_max : c->exposure;
	c->again = c->again < c->ag_min ? c->ag_min :
		   c->again > c->ag_max ? c->ag_max : c->again;
	apply_exposure(c);
	apply_wb(c);
	if (!raw) {
		int32_t ccm[9];
		struct v4l2_ext_control ec = {
			.id = V4L2_CID_CAMSS_CCM,
			.size = sizeof(ccm),
			.p_s32 = ccm,
		};
		struct v4l2_ext_controls ecs = { .count = 1, .controls = &ec };

		/* S_EXT_CTRLS writes the accepted payload back to userspace. */
		memcpy(ccm, cfg->ccm, sizeof(ccm));
		if (xioctl(c->vfe_fd, VIDIOC_S_EXT_CTRLS, &ecs) < 0)
			logmsg(c, "colour matrix: %s", strerror(errno));
		ctrl_set(c->vfe_fd, V4L2_CID_SATURATION, saturation);
		ctrl_set(c->vfe_fd, V4L2_CID_CONTRAST, contrast);
	}

	/* Kernel and owner are installed together. FRAME_SYNC is physical SOF
	 * in CLOCK_MONOTONIC; the capture timestamp is physical EOF. Optional
	 * subscription failure disables flash without preventing preview.
	 */
	piano_photo_timing_reset(&c->photo_timing);
	c->frame_sync_subscribed = false;
	if (!raw && !strcmp(cfg->name, "rear")) {
		struct v4l2_event_subscription sub = { .type = V4L2_EVENT_FRAME_SYNC };

		c->frame_sync_subscribed = xioctl(c->vid_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0;
	}
	if (xioctl(c->vid_fd, VIDIOC_STREAMON, &type) < 0) {
		logmsg(c, "STREAMON: %s", strerror(errno));
		goto out;
	}

	c->streaming = true;
	c->frames = 0;
	c->ae_updates = c->ae_stable = 0;
	c->ae_acquiring = true;
	c->ae_ready = false;
	logmsg(c, "streaming %ux%u -> %ux%u NV12 (bayer 0x%04x)", cfg->in_w,
	       cfg->in_h, cfg->out_w, cfg->out_h, code);
	if (raw)
		logmsg(c, "full Quad CFA via RDI: RAW10 stride %u sizeimage %u; "
		       "software still conversion drops input frames while busy",
		       c->bpl, c->sizeimage);
	ret = 0;
out:
	if (phy_fd >= 0)
		close(phy_fd);
	if (csid_fd >= 0)
		close(csid_fd);
	topo_free(&tp);
	close(mfd);
	if (ret < 0)
		cam_stop(c);

	return ret;
}

/* ------------------------------------------------------------------ */
/* auto exposure / white balance                                      */

struct stats {
	double mean;		/* weighted mean luma */
	double bright;		/* fraction of near-white samples */
	double r, g, b;		/* linear sums over mid-tone samples */
	unsigned int n_rgb;
};

static void frame_stats(const struct cam *c, const uint8_t *y,
			const uint8_t *uv, unsigned int stride,
			struct stats *s)
{
	const struct cam_cfg *cfg = c->cfg;
	unsigned int w = cfg->out_w, h = cfg->out_h;
	double sum = 0, wsum = 0;
	unsigned int bright = 0, n = 0;

	memset(s, 0, sizeof(*s));

	unsigned int row_step = h < 32 ? 1 : h / 32;
	unsigned int col_step = w < 64 ? 1 : w / 64;
	for (unsigned int row = row_step; row < h; row += row_step) {
		for (unsigned int col = col_step; col < w; col += col_step) {
			unsigned int r = row & ~1u, cl = col & ~1u;
			int Y = y[r * stride + cl];
			int U = uv[(r / 2) * stride + cl] - 128;
			int V = uv[(r / 2) * stride + cl + 1] - 128;
			bool centre = row > h / 4 && row < 3 * h / 4 &&
				      col > w / 4 && col < 3 * w / 4;
			double wt = centre ? 2.0 : 1.0;

			sum += wt * Y;
			wsum += wt;
			n++;
			if (Y >= 250)
				bright++;

			if (Y > 24 && Y < 230) {
				int R = Y + (int)lround(1.402 * V);
				int G = Y - (int)lround(0.344 * U + 0.714 * V);
				int B = Y + (int)lround(1.772 * U);

				if (R < 0 || R > 254 || G < 0 || G > 254 ||
				    B < 0 || B > 254)
					continue;
				s->r += srgb_lin[R];
				s->g += srgb_lin[G];
				s->b += srgb_lin[B];
				s->n_rgb++;
			}
		}
	}

	s->mean = sum / wsum;
	s->bright = (double)bright / n;
}

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void control_update(struct cam *c, const struct stats *s)
{
	const struct cam_cfg *cfg = c->cfg;
	double k, total, rem, target, error, lo, hi;
	int exp, ag;
	bool acquire = c->ae_acquiring;
	int old_exp = c->exposure, old_ag = c->again, old_dg = c->dgain;

	/* Acquisition uses the current valid frame, not stale filtered luma. */
	if (!c->ae_updates || acquire) {
		c->ae_mean = s->mean;
		c->ae_bright = s->bright;
	} else {
		c->ae_mean += 0.25 * (s->mean - c->ae_mean);
		c->ae_bright += 0.25 * (s->bright - c->ae_bright);
	}
	/* A hard clipping cutoff alternated between gain-up and gain-down.
	 * Reduce the luma target continuously instead of reversing the step.
	 */
	target = fmax(AE_TARGET * 0.65, AE_TARGET / (1.0 + 2.0 * c->ae_bright));
	error = log(target / fmax(c->ae_mean, 1.0));
	if (!c->ae_manual && fabs(error) > 0.05) {
		/* Only startup may take a larger step. The existing highlight target
		 * remains, and the startup upper bound falls with clipped pixels.
		 */
		lo = acquire ? 0.75 : 0.94;
		hi = acquire ? clampd(1.5 / (1.0 + 4.0 * s->bright), 1.06, 1.5) : 1.06;
		k = clampd(pow(target / fmax(c->ae_mean, 1.0), 2.2 * 0.3), lo, hi);
		total = (double)c->exposure * c->again / cfg->again_unit *
			c->dgain / DGAIN_UNITY * k;

		exp = (int)fmin(total, c->exp_max);
		if (exp < c->exp_min)
			exp = c->exp_min;
		rem = total / exp;

		ag = (int)floor(rem * cfg->again_unit);
		ag = ag < c->ag_min ? c->ag_min : ag > c->ag_max ? c->ag_max : ag;
		rem /= (double)ag / cfg->again_unit;

		c->exposure = exp;
		c->again = ag;
		c->dgain = (int)clampd(lround(rem * DGAIN_UNITY), DGAIN_UNITY,
				       DGAIN_MAX);
		apply_exposure(c);
	}

	/* A bounded acquisition cannot declare readiness merely on timeout.
	 * A target match, or unchanged controls at their limits, must persist
	 * for two observations before the rear lens scan is allowed.
	 */
	if (fabs(error) <= 0.10 ||
	    (c->ae_mean > 1.0 && old_exp == c->exposure &&
	     old_ag == c->again && old_dg == c->dgain))
		c->ae_stable++;
	else
		c->ae_stable = 0;
	c->ae_ready = c->ae_stable >= 2;
	if (c->ae_updates < AE_STARTUP_UPDATES)
		c->ae_updates++;
	if (c->ae_ready || c->ae_updates >= AE_STARTUP_UPDATES)
		c->ae_acquiring = false;

	/* gray world white balance on mid tones */
	if (!c->wb_manual && s->n_rgb > 200 && s->r > 0 && s->b > 0) {
		double rr = s->g / s->r, bb = s->g / s->b;

		if (fabs(log(rr)) > 0.02 || fabs(log(bb)) > 0.02) {
			c->rgain = clampd(c->rgain * pow(rr, 0.4), 0.5, 4.0);
			c->bgain = clampd(c->bgain * pow(bb, 0.4), 0.5, 4.0);
			apply_wb(c);
		}
	}

	if (c->frames % (AE_PERIOD * 30) == 0)
		logmsg(c, "mean %.0f bright %.2f ae %.0f target %.0f exp %d again %d dgain %d wb r %.2f b %.2f",
		       s->mean, s->bright, c->ae_mean, target, c->exposure, c->again, c->dgain,
		       c->rgain, c->bgain);
}

/* ------------------------------------------------------------------ */
/* contrast-detect autofocus                                          */

/*
 * Hill climb in the manner of the stock fine search: start where the lens
 * is, step towards the larger part of the range, keep going while the
 * sharpness rises and stop once it has fallen twice below the peak (or
 * turn round once if the first direction only ever fell). The peak is
 * then refined by a parabola through the best sample and its neighbours,
 * so no second, finer sweep is needed.
 */
#define AF_START	15	/* frames for auto exposure to settle first */
#define AF_SETTLE	2	/* frames to drop after a lens move (default) */
#define AF_STEPS	16	/* climb step: the range in this many steps */
#define AF_DROP		0.85	/* below this share of the peak: past it */
#define AF_RISE		1.6	/* a peak rises this far above the scan floor */
#define AF_LOST		0.6	/* relock below this share of the lock sharpness */
#define AF_LOST_FRAMES	20

/* horizontal gradient energy of the centre third, per unit of luma */
static double af_sharpness(const struct cam *c, const uint8_t *y,
			   unsigned int stride)
{
	unsigned int w = c->cfg->out_w, h = c->cfg->out_h;
	uint64_t grad = 0, luma = 0;

	for (unsigned int r = h / 3; r < 2 * h / 3; r += 4) {
		const uint8_t *p = y + (size_t)r * stride;

		for (unsigned int x = w / 3; x < 2 * w / 3; x += 2) {
			int d = p[x + 2] - p[x];

			grad += (uint64_t)(d * d);
			luma += p[x];
		}
	}

	return luma ? (double)grad / luma : 0;
}

static int af_clamp(const struct cam *c, int pos)
{
	return pos < c->af_min ? c->af_min : pos > c->af_max ? c->af_max : pos;
}

static int af_move(struct cam *c, int pos)
{
	pos = af_clamp(c, pos);
	if (pos != c->af_pos) {
		if (ctrl_set(c->vcm_fd, V4L2_CID_FOCUS_ABSOLUTE, pos) < 0) {
			logmsg(c, "focus control %d: %s", pos, strerror(errno));
			c->af_state = AF_OFF;
			return -1;
		}
		c->af_pos = pos;
		c->af_settle = af_settle_frames;
	}
	return 0;
}

static void af_scan(struct cam *c)
{
	int mid = (c->af_min + c->af_max) / 2;

	c->af_step = (c->af_max - c->af_min) / AF_STEPS;
	if (c->af_step < 1)
		c->af_step = 1;
	c->af_dir = c->af_pos <= mid ? 1 : -1;
	c->af_origin = c->af_pos;
	c->af_reversed = false;
	c->af_drops = 0;
	c->af_n = 0;
	c->af_best = -1;
	c->af_floor = -1;
	c->af_state = AF_SCAN;
	/* the first sample is where the lens already is */
	af_move(c, c->af_pos);
}

/* vertex of the parabola through the best sample and its neighbours */
static int af_peak(const struct cam *c)
{
	int b = c->af_best_pos, l = -1, r = -1;
	double x0, x1 = b, x2, y0, y1 = c->af_best, y2, d;

	for (int i = 0; i < c->af_n; i++) {
		if (c->af_hist_pos[i] < b &&
		    (l < 0 || c->af_hist_pos[i] > c->af_hist_pos[l]))
			l = i;
		if (c->af_hist_pos[i] > b &&
		    (r < 0 || c->af_hist_pos[i] < c->af_hist_pos[r]))
			r = i;
	}
	if (l < 0 || r < 0)
		return b;
	x0 = c->af_hist_pos[l];
	y0 = c->af_hist_val[l];
	x2 = c->af_hist_pos[r];
	y2 = c->af_hist_val[r];
	d = (x0 - x1) * (x0 - x2) * (x1 - x2);
	if (d == 0)
		return b;
	{
		double A = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / d;
		double B = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) +
			    x0 * x0 * (y1 - y2)) / d;
		double v;

		if (A >= 0)	/* not a maximum */
			return b;
		v = -B / (2 * A);
		if (v < x0)
			v = x0;
		if (v > x2)
			v = x2;
		return (int)lround(v);
	}
}

/* one sharpness sample at the current lens position */
static void af_feed(struct cam *c, double sh)
{
	int next;

	switch (c->af_state) {
	case AF_LOCKED:
		c->af_lost = sh < AF_LOST * c->af_lock ? c->af_lost + 1 : 0;
		if (c->af_lost >= AF_LOST_FRAMES)
			af_scan(c);
		return;
	case AF_VERIFY:
		/* a parabola that missed: fall back to the best sample */
		if (sh < AF_DROP * c->af_best && c->af_pos != c->af_best_pos) {
			af_move(c, c->af_best_pos);
			return;
		}
		c->af_lock = sh;
		c->af_lost = 0;
		c->af_state = AF_LOCKED;
		logmsg(c, "focus %d after %d steps (sharpness %.2f)",
		       c->af_pos, c->af_n, sh);
		return;
	case AF_SCAN:
	case AF_FINE:
		break;
	default:
		return;
	}

	if (c->af_n < AF_HIST) {
		c->af_hist_pos[c->af_n] = c->af_pos;
		c->af_hist_val[c->af_n] = sh;
		c->af_n++;
	}
	if (sh > c->af_best) {
		c->af_best = sh;
		c->af_best_pos = c->af_pos;
		c->af_drops = 0;
	} else if (sh < AF_DROP * c->af_best) {
		c->af_drops++;
	}
	if (c->af_floor < 0 || sh < c->af_floor)
		c->af_floor = sh;

	if (c->af_state == AF_FINE) {
		/* three samples around the coarse estimate */
		for (int i = 0; i < 3; i++) {
			if (c->af_fine[i] < 0)
				continue;
			next = c->af_fine[i];
			c->af_fine[i] = -1;
			if (af_move(c, next) < 0)
				return;
			if (c->af_pos != next || c->af_settle)
				return;
			/* already there (clamped): sampled */
		}
		if (af_move(c, af_peak(c)) < 0)
			return;
		c->af_state = AF_VERIFY;
		return;
	}

	next = c->af_pos + c->af_dir * c->af_step;
	if (next == af_clamp(c, next) && c->af_n < AF_HIST &&
	    !(c->af_drops >= 2 && c->af_best > AF_RISE * c->af_floor)) {
		af_move(c, next);
		return;
	}
	/*
	 * Past a peak, or at the end of the range. Turn round once if the
	 * first direction never rose above the start (the peak lies the
	 * other way) or found nothing but a flat curve.
	 */
	if (!c->af_reversed && (c->af_best_pos == c->af_origin ||
				c->af_best <= AF_RISE * c->af_floor)) {
		next = c->af_origin - c->af_dir * c->af_step;
		c->af_dir = -c->af_dir;
		c->af_reversed = true;
		c->af_drops = 0;
		if (next == af_clamp(c, next)) {
			af_move(c, next);
			return;
		}
	}
	/* refine: a parabola, then a fine pass at a quarter step around it */
	next = af_peak(c);
	c->af_fine[0] = af_clamp(c, next - c->af_step / 4);
	c->af_fine[1] = af_clamp(c, next + c->af_step / 4);
	c->af_fine[2] = -1;
	c->af_state = AF_FINE;
	af_move(c, next);
}

static void af_update(struct cam *c, const uint8_t *y, unsigned int stride)
{
	if (c->af_manual || c->af_state == AF_OFF)
		return;
	if (c->af_state == AF_WAIT) {
		if (c->frames >= AF_START && c->ae_ready)
			af_scan(c);
		return;
	}
	if (c->af_settle > 0) {
		c->af_settle--;
		return;
	}
	if (!c->ae_ready)
		return;
	af_feed(c, af_sharpness(c, y, stride));
}

static void quad_controls(const struct cam *c, struct piano_quad_params *params)
{
	piano_quad_params_default(params);
	/* JN1 factory RAW10 levels: white 1023, black 64 on all four channels. */
	params->black_level = 64.0f;
	params->digital_gain = (float)c->dgain / DGAIN_UNITY;
	params->white_balance[0] = c->rgain;
	params->white_balance[2] = c->bgain;
	for (unsigned int i = 0; i < 9; i++)
		params->ccm[i] = (float)c->cfg->ccm[i] / 1024;
	params->saturation = (float)saturation / SAT_UNITY;
	params->contrast = (float)contrast / 256;
}

/* Demosaic the full sensor first. A smaller client format then uses standard
 * NV12 scaling with an even, centred crop; the native 50MP path makes no copy
 * and never bins or enlarges a lower-resolution image.
 */
static const uint8_t *quad_output(struct cam *c, const uint8_t *frame)
{
	const struct cam_cfg *cfg = c->cfg;
	unsigned int width = cfg->in_w, height = cfg->in_h;
	unsigned int left, top;

	if (cfg->out_w == width && cfg->out_h == height)
		return frame;
	if ((uint64_t)cfg->out_w * height < (uint64_t)cfg->out_h * width)
		width = ((uint64_t)height * cfg->out_w / cfg->out_h) & ~1u;
	else
		height = ((uint64_t)width * cfg->out_h / cfg->out_w) & ~1u;
	left = ((cfg->in_w - width) / 2) & ~1u;
	top = ((cfg->in_h - height) / 2) & ~1u;
	if (NV12Scale(frame + (size_t)top * cfg->in_w + left, cfg->in_w,
		      frame + (size_t)cfg->in_w * cfg->in_h +
			      (size_t)(top / 2) * cfg->in_w + left, cfg->in_w,
		      width, height, c->stage, cfg->out_w,
		      c->stage + (size_t)cfg->out_w * cfg->out_h, cfg->out_w,
		      cfg->out_w, cfg->out_h, kFilterBox) < 0) {
		errno = EIO;
		return NULL;
	}
	return c->stage;
}

/* Only the copied CPU frame crosses the worker boundary. All camera controls,
 * statistics, autofocus and loopback writes stay in the existing main loop.
 */
static void cam_quad_result(struct cam *c)
{
	struct quad_worker_result frame;
	int result = quad_worker_take(c->quad_worker, &frame);

	if (result < 0) {
		if (result != -EAGAIN)
			logmsg(c, "Quad CFA completion: %s", strerror(-result));
		return;
	}
	if (frame.status < 0 ||
	    frame.bytes != (size_t)c->cfg->in_w * c->cfg->in_h * 3 / 2) {
		logmsg(c, "Quad CFA frame %u: %s", frame.sequence,
		       strerror(frame.status < 0 ? -frame.status : EIO));
	} else {
		const uint8_t *output = quad_output(c, frame.nv12);

		if (!output) {
			logmsg(c, "Quad CFA resize: %s", strerror(errno));
			goto release;
		}
		if (++c->frames % AE_PERIOD == 0) {
			struct stats stats;

			frame_stats(c, output,
				    output + (size_t)c->cfg->out_w * c->cfg->out_h,
				    c->cfg->out_w, &stats);
			control_update(c, &stats);
		}
		af_update(c, output, c->cfg->out_w);
		if (loopback_write_frame(c, output, &c->quad_timestamp, c->quad_timestamp_flags) < 0)
			logmsg(c, "loopback write: %s", strerror(errno));
		if (c->frames == 1)
			logmsg(c, "full Quad CFA frame %u: unpack %.1f ms render %.1f ms "
			       "working %.1f MiB", frame.sequence,
			       frame.metrics.unpack_ns / 1e6,
			       (frame.metrics.demosaic_ns + frame.metrics.color_ns) / 1e6,
			       frame.metrics.working_bytes / 1048576.0);
	}
release:
	result = quad_worker_release(c->quad_worker);
	if (result < 0)
		logmsg(c, "Quad CFA release: %s", strerror(-result));
}

static void cam_frame(struct cam *c)
{
	const struct cam_cfg *cfg = c->cfg;
	struct v4l2_plane pl[VIDEO_MAX_PLANES] = { 0 };
	struct v4l2_buffer b = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.length = VIDEO_MAX_PLANES,
		.m.planes = pl,
	};
	const uint8_t *y, *uv, *out;
	bool prepared = false;

	if (xioctl(c->vid_fd, VIDIOC_DQBUF, &b) < 0) {
		if (errno != EAGAIN) {
			logmsg(c, "DQBUF: %s", strerror(errno));
			piano_photo_abort(&c->photo, errno, true);
		}
		return;
	}

	if (!(b.flags & V4L2_BUF_FLAG_ERROR)) {
		const struct buf *mapped;
		size_t offset = pl[0].data_offset, used = pl[0].bytesused;

		if (b.index >= c->nbufs || b.length != 1) {
			logmsg(c, "invalid capture frame: index %u planes %u", b.index,
			       b.length);
			piano_photo_abort(&c->photo, EIO, true);
			goto requeue;
		}
		mapped = &c->bufs[b.index];
		if (!mapped->ptr || pl[0].length > mapped->len ||
		    pl[0].length < c->sizeimage || used > pl[0].length ||
		    offset > used || c->frame_span > used - offset) {
			logmsg(c, "incomplete capture frame: sequence %u index %u used %zu "
			       "offset %zu length %u mapped %zu span %zu", b.sequence,
			       b.index, used, offset, pl[0].length, mapped->len, c->frame_span);
			piano_photo_abort(&c->photo, EIO, true);
			goto requeue;
		}
		y = (const uint8_t *)mapped->ptr + offset;
		if (c->quad_raw) {
			struct piano_quad_params params;
			int result;

			quad_controls(c, &params);
			result = quad_worker_submit(c->quad_worker, y, used - offset,
						    c->bpl, &params, b.sequence);
			prepared = result == 0;
			if (prepared) {
				/* The one in-flight worker result retains this source frame's
				 * clock, even when rendering finishes seconds later.
				 */
				c->quad_timestamp = b.timestamp;
				c->quad_timestamp_flags = b.flags;
			}
			if (result < 0 && result != -EBUSY)
				logmsg(c, "RAW10 frame %u: %s", b.sequence, strerror(-result));
			goto requeue;
		}
		uv = y + c->uv_offset;
		bool recovering = c->photo_recover_frames > 0;
		bool held = piano_photo_blocks_3a(&c->photo) || recovering;

		camera_photo_frame(c, &b, y, uv, true);
		held = held || piano_photo_blocks_3a(&c->photo);
		if (recovering && !piano_photo_active(&c->photo)) c->photo_recover_frames--;
		if (++c->frames % AE_PERIOD == 0 && !held) {
			struct stats s;

			frame_stats(c, y, uv, c->bpl, &s);
			control_update(c, &s);
		}
		if (!held) af_update(c, y, c->bpl);

		out = y;
		if (c->stage) {
			uint8_t *d = c->stage;

			for (unsigned int r = 0; r < cfg->out_h * 3 / 2; r++)
				memcpy(d + (size_t)r * cfg->out_w,
				       y + (size_t)r * c->bpl, cfg->out_w);
			out = c->stage;
		}
		if (loopback_write_frame(c, out, &b.timestamp, b.flags) < 0)
			logmsg(c, "loopback write: %s", strerror(errno));
	}

	else piano_photo_abort(&c->photo, EIO, true);

requeue:
	if (xioctl(c->vid_fd, VIDIOC_QBUF, &b) < 0) {
		logmsg(c, "QBUF after frame %u: %s", b.sequence, strerror(errno));
		piano_photo_abort(&c->photo, errno, true);
		/* A prepared copy is never started before its DMA buffer is back
		 * with the driver. Stop rather than keep a partially queued stream.
		 */
		if (c->quad_raw)
			cam_stop(c);
		return;
	}
	if (prepared) {
		int result = quad_worker_start(c->quad_worker);

		if (result < 0) {
			logmsg(c, "Quad CFA start: %s", strerror(-result));
			cam_stop(c);
		}
	}
}

static int loopback_change_format(struct cam *c)
{
	struct v4l2_format format = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	struct v4l2_streamparm rate = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	struct cam_cfg mode = c->mode;
	struct v4l2_rect compose, crop;

	if (cam_stop(c) < 0 ||
	    xioctl(c->loop_fd, VIDIOC_G_FMT, &format) < 0 ||
	    xioctl(c->loop_fd, VIDIOC_G_PARM, &rate) < 0)
		return -1;
	unsigned int fps = rate.parm.output.timeperframe.denominator;

	if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
	    rate.parm.output.timeperframe.numerator != 1 ||
	    select_capture_mode(&mode, format.fmt.pix.width, format.fmt.pix.height, fps) < 0 ||
	    isp_geometry(&mode, &compose, &crop) < 0) {
		errno = EINVAL;
		return -1;
	}
	int result = loopback_output_clear(&c->loop_output);
	if (result < 0) {
		errno = -result;
		return -1;
	}
	if (loopback_format(c, mode.out_w, mode.out_h) < 0 ||
	    loopback_rate(c, fps) < 0)
		return -1;
	c->mode = mode;
	result = loopback_output_setup(&c->loop_output, c->loop_fd, frame_size(c->cfg));
	if (result < 0) {
		errno = -result;
		return -1;
	}
	/* Publishing this frame acknowledges the consumer's blocking S_FMT.
	 * Actual camera DMA still starts only with the existing client-usage event.
	 */
	if (loopback_black(c) < 0)
		return -1;
	logmsg(c, "output negotiated to %ux%u NV12 at %u fps", mode.out_w, mode.out_h, fps);
	return 0;
}

static void loopback_event(struct cam *c)
{
	struct v4l2_event ev;

	while (!xioctl(c->loop_fd, VIDIOC_DQEVENT, &ev)) {
		const struct v4l2_event_client_usage *u = (const void *)&ev.u;

		if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
			if (loopback_change_format(c) < 0) {
				logmsg(c, "output reconfiguration: %s", strerror(errno));
				/* A failed STREAMOFF must keep this process and its capture
				 * mappings alive. The consumer's negotiation times out;
				 * it cannot make us tear down uncertain DMA buffers.
				 */
				if (!c->streaming) {
					exit_status = 1;
					quit = 1;
				}
				return;
			}
			continue;
		}
		if (ev.type != V4L2_EVENT_PRI_CLIENT_USAGE)
			continue;
		c->loop_users = u->count;
		if (!u->count && c->streaming && piano_photo_active(&c->photo))
			c->photo_owned_stream = true;
		if (u->count && !c->streaming) {
			/* Capture STREAMON discards the negotiation frame. Only a
			 * real frame from this stream may satisfy its first dequeue.
			 */
			cam_start(c);
		}
		else if (!u->count && c->streaming && !piano_photo_active(&c->photo))
			cam_stop(c);
	}
}

/*
 * systemd readiness (Type=notify): the loopback devices are capture
 * devices from here on, so a session started afterwards finds them.
 */
static void notify_ready(void)
{
	const char *path = getenv("NOTIFY_SOCKET");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	static const char msg[] = "READY=1";
	socklen_t len;
	int fd;

	if (!path || (path[0] != '/' && path[0] != '@') ||
	    strlen(path) >= sizeof(sa.sun_path))
		return;
	strcpy(sa.sun_path, path);
	if (path[0] == '@')
		sa.sun_path[0] = '\0';
	len = offsetof(struct sockaddr_un, sun_path) + strlen(path);
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, len);
	close(fd);
}

/* Fixed native controls, serialized with 3A by the existing owner loop. */
#define CONTROL_PATH "/run/piano-camerad/control.sock"
#define CONTROL_CLIENTS 4
#define POLL_QUAD_RESULT (3 + CONTROL_CLIENTS)
#define POLL_PHOTO_CANCEL (POLL_QUAD_RESULT + 1)
static int control_fd = -1;
static int control_clients[CONTROL_CLIENTS] = { -1, -1, -1, -1 };

enum control_domain { DOMAIN_AE, DOMAIN_WB, DOMAIN_AF };
struct named_control {
	const char *name, *unit;
	uint32_t id;
	enum control_domain domain;
};
static const struct named_control named_controls[] = {
	{ "exposure", "sensor-lines", V4L2_CID_EXPOSURE, DOMAIN_AE },
	{ "analog-gain", "driver-native", V4L2_CID_ANALOGUE_GAIN, DOMAIN_AE },
	{ "digital-gain", "q10", V4L2_CID_DIGITAL_GAIN, DOMAIN_AE },
	{ "red-balance", "q10", V4L2_CID_RED_BALANCE, DOMAIN_WB },
	{ "blue-balance", "q10", V4L2_CID_BLUE_BALANCE, DOMAIN_WB },
	{ "focus", "driver-native-dac", V4L2_CID_FOCUS_ABSOLUTE, DOMAIN_AF },
};
#define CONTROL_COUNT (sizeof(named_controls) / sizeof(named_controls[0]))

static int control_node(const struct cam *c, unsigned int i)
{
	if (i < 2)
		return c->sensor_fd;
	return i == 5 ? c->vcm_fd : c->vfe_fd;
}

static bool control_software(const struct cam *c, unsigned int i)
{
	return c->quad_raw && i >= 2 && i <= 4;
}

static int control_query(struct cam *c, unsigned int i, struct v4l2_query_ext_ctrl *q)
{
	if (!control_software(c, i))
		return xioctl(control_node(c, i), VIDIOC_QUERY_EXT_CTRL, q);
	/* Keep the existing Q10 control protocol. These are the ranges actually
	 * accepted by the software converter, not nonexistent RDI ISP controls.
	 */
	*q = (struct v4l2_query_ext_ctrl){
		.id = named_controls[i].id, .type = V4L2_CTRL_TYPE_INTEGER,
		.minimum = i == 2 ? DGAIN_UNITY : 1, .maximum = 16 * 1024 - 1,
		.step = 1, .default_value = 1024,
	};
	return 0;
}

static int control_get(struct cam *c, unsigned int i, int *value)
{
	struct v4l2_control ctl = { .id = named_controls[i].id };
	int fd = control_node(c, i);

	if (control_software(c, i)) {
		*value = i == 2 ? c->dgain :
			(int)lround((i == 3 ? c->rgain : c->bgain) * WB_UNITY);
		return 0;
	}
	if (fd < 0) {
		errno = EOPNOTSUPP;
		return -1;
	}
	if (xioctl(fd, VIDIOC_G_CTRL, &ctl) < 0)
		return -1;
	*value = ctl.value;
	return 0;
}

static void control_cache(struct cam *c, unsigned int i, int value)
{
	switch (i) {
	case 0: c->exposure = value; break;
	case 1: c->again = value; break;
	case 2: c->dgain = value; break;
	case 3: c->rgain = (double)value / WB_UNITY; break;
	case 4: c->bgain = (double)value / WB_UNITY; break;
	case 5: c->af_pos = value; break;
	}
}

static int control_refresh(struct cam *c, enum control_domain domain)
{
	int values[CONTROL_COUNT];

	for (unsigned int i = 0; i < CONTROL_COUNT; i++)
		if (named_controls[i].domain == domain && control_get(c, i, &values[i]) < 0)
			return -1;
	for (unsigned int i = 0; i < CONTROL_COUNT; i++)
		if (named_controls[i].domain == domain)
			control_cache(c, i, values[i]);
	return 0;
}

static void control_error(FILE *out, const char *stage, int error, bool attempted)
{
	fprintf(out, "{\"version\":1,\"ok\":false,\"stage\":\"%s\",\"errno\":%d,"
		"\"write_attempted\":%s}\n", stage, error, attempted ? "true" : "false");
}

/* S5KJN1 publishes VT timing through standard pixel/blanking controls.
 * OV32D40 keeps native integration counts and fixed factory frame intervals;
 * its line period follows the same nominal FLL/interval relation as CamX.
 */
struct exposure_timing { uint64_t line_ns_num, line_ns_den; };

static int exposure_timing(struct cam *c, const struct v4l2_query_ext_ctrl *q,
			  struct exposure_timing *timing)
{
	struct v4l2_subdev_format f = { .which = V4L2_SUBDEV_FORMAT_ACTIVE, .pad = 0 };

	if (q->type != V4L2_CTRL_TYPE_INTEGER || !q->step) {
		errno = EOPNOTSUPP;
		return -1;
	}
	if (!strcmp(c->cfg->sensor, "s5kjn1")) {
		struct v4l2_control blank = { .id = V4L2_CID_HBLANK };
		struct v4l2_ext_control rate = { .id = V4L2_CID_PIXEL_RATE };
		struct v4l2_ext_controls controls = {
			.count = 1, .controls = &rate,
		};

		if (xioctl(c->sensor_fd, VIDIOC_SUBDEV_G_FMT, &f) < 0 ||
		    xioctl(c->sensor_fd, VIDIOC_G_CTRL, &blank) < 0 ||
		    xioctl(c->sensor_fd, VIDIOC_G_EXT_CTRLS, &controls) < 0)
			return -1;
		if (blank.value < 0 || rate.value64 <= 0) {
			errno = EOPNOTSUPP;
			return -1;
		}
		*timing = (struct exposure_timing){
			((uint64_t)f.format.width + blank.value) * UINT64_C(1000000000),
			(uint64_t)rate.value64,
		};
		return 0;
	}
	if (!strcmp(c->cfg->sensor, "ov32d40")) {
		struct v4l2_subdev_frame_interval interval = { .pad = 0 };

		if (xioctl(c->sensor_fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &interval) < 0)
			return -1;
		/* The driver fixes native FLL and keeps twelve integration periods
		 * between maximum exposure and frame end in each factory mode.
		 */
		*timing = (struct exposure_timing){
			(uint64_t)interval.interval.numerator * UINT64_C(1000000000),
			((uint64_t)q->maximum + 12) * interval.interval.denominator,
		};
		return 0;
	}
	errno = EOPNOTSUPP;
	return -1;
}

static uint64_t exposure_ns(const struct exposure_timing *timing, uint64_t lines)
{
	return (lines * timing->line_ns_num + timing->line_ns_den / 2) / timing->line_ns_den;
}

static int exposure_lines(const char *arg, const struct v4l2_query_ext_ctrl *q,
			  const struct exposure_timing *timing, uint64_t *ns, long *lines)
{
	char *end;
	uint64_t minimum = (uint64_t)q->minimum;
	uint64_t steps = (q->maximum - q->minimum) / q->step;
	uint64_t maximum = minimum + steps * q->step;
	uint64_t quantum = q->step * timing->line_ns_num;
	long long input;

	errno = 0;
	input = strtoll(arg, &end, 10);
	if (errno || !arg[0] || *end || input <= 0 ||
	    (uint64_t)input < exposure_ns(timing, minimum) ||
	    (uint64_t)input > exposure_ns(timing, maximum)) {
		errno = ERANGE;
		return -1;
	}
	*ns = (uint64_t)input;
	uint64_t scaled = *ns * timing->line_ns_den;
	uint64_t origin = minimum * timing->line_ns_num;
	uint64_t delta = scaled > origin ? scaled - origin : 0;
	uint64_t selected = (delta + quantum / 2) / quantum;
	if (selected > steps)
		selected = steps;
	*lines = (long)(minimum + selected * q->step);
	return 0;
}

static void control_report_exposure_time(FILE *out, struct cam *c)
{
	struct v4l2_query_ext_ctrl q = { .id = V4L2_CID_EXPOSURE };
	struct exposure_timing timing;
	int actual;

	fputs(",{\"name\":\"exposure-time-ns\",\"unit\":\"ns\"", out);
	if (xioctl(c->sensor_fd, VIDIOC_QUERY_EXT_CTRL, &q) < 0 ||
	    exposure_timing(c, &q, &timing) < 0) {
		fprintf(out, ",\"supported\":false,\"timing_errno\":%d}", errno);
		return;
	}
	uint64_t last = q.minimum + (q.maximum - q.minimum) / q.step * q.step;
	fprintf(out, ",\"supported\":true,\"min\":%llu,\"max\":%llu,\"default\":%llu,"
		"\"line_ns_num\":%llu,\"line_ns_den\":%llu,\"line_step\":%llu,\"flags\":%u",
		(unsigned long long)exposure_ns(&timing, (uint64_t)q.minimum),
		(unsigned long long)exposure_ns(&timing, last),
		(unsigned long long)exposure_ns(&timing, (uint64_t)q.default_value),
		(unsigned long long)timing.line_ns_num, (unsigned long long)timing.line_ns_den,
		(unsigned long long)q.step, q.flags);
	if (control_get(c, 0, &actual) < 0)
		fprintf(out, ",\"get_errno\":%d", errno);
	else if (actual < q.minimum || actual > q.maximum)
		fprintf(out, ",\"get_errno\":%d,\"lines\":%d", ERANGE, actual);
	else
		fprintf(out, ",\"lines\":%d,\"value\":%llu", actual,
			(unsigned long long)exposure_ns(&timing, (uint64_t)actual));
	fputc('}', out);
}

static void control_report(FILE *out, struct cam *c, bool values)
{
	struct v4l2_query_ext_ctrl query[CONTROL_COUNT];
	int current[CONTROL_COUNT] = {0}, query_error[CONTROL_COUNT] = {0};
	int get_error[CONTROL_COUNT] = {0};
	bool complete = true;

	for (unsigned int i = 0; i < CONTROL_COUNT; i++) {
		int fd = control_node(c, i);

		query[i] = (struct v4l2_query_ext_ctrl){ .id = named_controls[i].id };
		if (fd < 0)
			continue;
		if (control_query(c, i, &query[i]) < 0) {
			query_error[i] = errno;
			complete = false;
		} else if (values && control_get(c, i, &current[i]) < 0) {
			get_error[i] = errno;
			complete = false;
		}
	}
	fprintf(out, "{\"version\":1,\"ok\":%s,\"camera\":\"%s\",\"active\":true,"
		"\"ae\":\"%s\",\"awb\":\"%s\",\"af\":\"%s\",\"controls\":[",
		complete ? "true" : "false", c->cfg->name, c->ae_manual ? "manual" : "auto",
		c->wb_manual ? "manual" : "auto",
		c->vcm_fd < 0 ? "unsupported" : c->af_manual ? "manual" :
		c->af_state == AF_OFF ? "error" : "auto");
	for (unsigned int i = 0; i < CONTROL_COUNT; i++) {
		const struct named_control *n = &named_controls[i];
		const struct v4l2_query_ext_ctrl *q = &query[i];

		fprintf(out, "%s{\"name\":\"%s\",\"unit\":\"%s\"", i ? "," : "", n->name, n->unit);
		if (control_node(c, i) < 0) {
			fputs(",\"supported\":false}", out);
			continue;
		}
		if (query_error[i]) {
			fprintf(out, ",\"query_errno\":%d}", query_error[i]);
			continue;
		}
		fprintf(out, ",\"supported\":true,\"type\":%u,\"min\":%lld,\"max\":%lld,"
			"\"step\":%llu,\"default\":%lld,\"flags\":%u",
			q->type, (long long)q->minimum, (long long)q->maximum,
			(unsigned long long)q->step, (long long)q->default_value, q->flags);
		if (values) {
			if (get_error[i])
				fprintf(out, ",\"get_errno\":%d", get_error[i]);
			else
				fprintf(out, ",\"value\":%d", current[i]);
		}
		fputc('}', out);
	}
	control_report_exposure_time(out, c);
	fputs("]}\n", out);
}

static void control_request(FILE *out, const char *packet)
{
	char verb[16], camera[16], name[32], arg[32], extra[2];
	struct cam *c = NULL;
	int count = sscanf(packet, "%15s %15s %31s %31s %1s", verb, camera, name, arg, extra);

	if (count < 2 || count > 4) {
		control_error(out, "syntax", EINVAL, false);
		return;
	}
	for (unsigned int i = 0; i < NUM_CAMS; i++)
		if (!strcmp(camera, cams[i].cfg->name))
			c = &cams[i];
	if (!c) {
		control_error(out, "camera", EINVAL, false);
		return;
	}
	if (!c->streaming) {
		control_error(out, "inactive", ENODEV, false);
		return;
	}
	if (!strcmp(verb, "set") && piano_photo_active(&c->photo)) {
		control_error(out, "capture", EBUSY, false);
		return;
	}
	if ((!strcmp(verb, "caps") || !strcmp(verb, "get")) && count == 2) {
		control_report(out, c, !strcmp(verb, "get"));
		return;
	}
	if (strcmp(verb, "set") || count != 4) {
		control_error(out, "syntax", EINVAL, false);
		return;
	}
	if (!strcmp(name, "ae") || !strcmp(name, "awb") || !strcmp(name, "af")) {
		enum control_domain domain = !strcmp(name, "ae") ? DOMAIN_AE :
			!strcmp(name, "awb") ? DOMAIN_WB : DOMAIN_AF;
		bool manual = !strcmp(arg, "manual");

		if (!manual && strcmp(arg, "auto")) {
			control_error(out, "mode", EINVAL, false);
			return;
		}
		/* Confirm driver state before the software loop changes owner. */
		if (control_refresh(c, domain) < 0) {
			control_error(out, "get-before-mode", errno, false);
			return;
		}
		if (domain == DOMAIN_AE) {
			c->ae_manual = manual;
			c->ae_updates = c->ae_stable = 0;
			c->ae_acquiring = !manual;
			c->ae_ready = false;
		} else if (domain == DOMAIN_WB) {
			c->wb_manual = manual;
		} else {
			c->af_manual = manual;
			c->af_settle = 0;
			c->af_state = manual ? AF_OFF : AF_WAIT;
		}
		fprintf(out, "{\"version\":1,\"ok\":true,\"camera\":\"%s\","
			"\"domain\":\"%s\",\"mode\":\"%s\",\"write_attempted\":false}\n",
			camera, name, arg);
		return;
	}
	for (unsigned int i = 0; i < CONTROL_COUNT; i++) {
		const struct named_control *n = &named_controls[i];
		struct v4l2_query_ext_ctrl q = { .id = n->id };
		int fd = control_node(c, i), actual;
		char *end;
		long requested;
		uint64_t requested_ns = 0;
		struct exposure_timing timing;
		bool time_units = i == 0 && !strcmp(name, "exposure-time-ns");
		bool manual = n->domain == DOMAIN_AE ? c->ae_manual :
			n->domain == DOMAIN_WB ? c->wb_manual : c->af_manual;

		if (strcmp(name, n->name) && !time_units)
			continue;
		if (fd < 0) {
			control_error(out, "unsupported", EOPNOTSUPP, false);
			return;
		}
		if (!manual) {
			control_error(out, "auto-owner", EBUSY, false);
			return;
		}
		if (control_query(c, i, &q) < 0) {
			control_error(out, "query", errno, false);
			return;
		}
		if (time_units) {
			if (exposure_timing(c, &q, &timing) < 0) {
				control_error(out, "timing", errno, false);
				return;
			}
			if (exposure_lines(arg, &q, &timing, &requested_ns, &requested) < 0) {
				control_error(out, "value", errno, false);
				return;
			}
		} else {
			errno = 0;
			requested = strtol(arg, &end, 10);
			if (errno || !arg[0] || *end || requested < INT_MIN || requested > INT_MAX) {
				control_error(out, "value", ERANGE, false);
				return;
			}
		}
		if (q.type != V4L2_CTRL_TYPE_INTEGER || q.minimum < INT_MIN || q.maximum > INT_MAX ||
		    q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_GRABBED) ||
		    requested < q.minimum || requested > q.maximum || !q.step ||
		    (uint64_t)(requested - q.minimum) % q.step) {
			control_error(out, "range-or-flags", ERANGE, false);
			return;
		}
		if (!control_software(c, i) && ctrl_set(fd, n->id, (int)requested) < 0) {
			control_error(out, "set", errno, true);
			return;
		}
		/* Cache the accepted request if readback itself fails; never report it as a readback. */
		control_cache(c, i, (int)requested);
		if (n->domain == DOMAIN_AE) {
			c->ae_stable = 0;
			c->ae_ready = false;
		}
		if (control_get(c, i, &actual) < 0) {
			control_error(out, "readback", errno, true);
			return;
		}
		if (time_units) {
			if (actual < q.minimum || actual > q.maximum ||
			    (uint64_t)(actual - q.minimum) % q.step) {
				fprintf(out, "{\"version\":1,\"ok\":false,\"stage\":\"readback-range\","
					"\"lines\":%d,\"errno\":%d,\"write_attempted\":true}\n", actual, ERANGE);
				return;
			}
			control_cache(c, i, actual);
			fprintf(out, "{\"version\":1,\"ok\":%s,\"camera\":\"%s\","
				"\"control\":\"%s\",\"unit\":\"ns\",\"requested\":%llu,"
				"\"requested_lines\":%ld,\"lines\":%d,\"value\":%llu,"
				"\"errno\":%d,\"write_attempted\":true}\n",
				actual == requested ? "true" : "false", camera, name,
				(unsigned long long)requested_ns, requested, actual,
				(unsigned long long)exposure_ns(&timing, (uint64_t)actual),
				actual == requested ? 0 : EIO);
			return;
		}
		control_cache(c, i, actual);
		fprintf(out, "{\"version\":1,\"ok\":%s,\"camera\":\"%s\",\"control\":\"%s\","
			"\"requested\":%ld,\"value\":%d,\"errno\":%d,\"write_attempted\":true}\n",
			actual == requested ? "true" : "false", camera, name, requested, actual,
			actual == requested ? 0 : EIO);
		return;
	}
	control_error(out, "control", EINVAL, false);
}

/* Drain physical events before dequeueing the associated capture buffer. */
static void camera_frame_events(struct cam *c)
{
	struct v4l2_event event;

	if (!c->frame_sync_subscribed) return;
	while (xioctl(c->vid_fd, VIDIOC_DQEVENT, &event) == 0) {
		if (event.type == V4L2_EVENT_FRAME_SYNC &&
		    piano_photo_timing_event(&c->photo_timing, &event) < 0)
			piano_photo_abort(&c->photo, EPIPE, true);
	}
	if (errno != EAGAIN && errno != ENOENT)
		piano_photo_abort(&c->photo, errno, true);
}

static int photo_physical_times(struct cam *c, const struct v4l2_buffer *buffer,
				struct piano_photo_frame *frame)
{
	const struct piano_photo_settings *settings = &c->photo.settings;
	uint64_t lead = 0;

	if (!c->frame_sync_subscribed || c->quad_raw) return -EOPNOTSUPP;
	if (c->photo.frozen) {
		uint64_t line = (settings->line_ns_num + settings->line_ns_den - 1) / settings->line_ns_den;

		/* Original sensor exposure model: ADC offset 1ms plus one line.
		 * End uses actual physical EOF, including the measured extra span.
		 */
		lead = settings->exposure_ns + UINT64_C(1000000) + line;
	}
	return piano_photo_timing_frame(&c->photo_timing, buffer, lead, frame);
}

static int flash_get(int fd, uint32_t id, int *value)
{
	struct v4l2_control control = { .id = id };

	if (xioctl(fd, VIDIOC_G_CTRL, &control) < 0) return -errno;
	*value = control.value;
	return 0;
}

static int camera_flash_stop(void *context, uint64_t *call_ns, uint32_t *fault)
{
	struct cam *c = context;
	int first_error = 0;

	*call_ns = monotonic_ns();
	*fault = 0;
	for (unsigned int i = 0; i < c->flash_count; i++) {
		struct camera_flash *flash = &c->flash[i];
		int value = 0, ret;

		if (flash->fd < 0) continue;
		if (flash->configured) {
			if (ctrl_set(flash->fd, V4L2_CID_FLASH_STROBE_STOP, 0) < 0 && !first_error)
				first_error = errno;
			ret = flash_get(flash->fd, V4L2_CID_FLASH_FAULT, &value);
			if (ret < 0 && !first_error) first_error = -ret;
			else if (ret == 0) *fault |= (uint32_t)value;
			if (ctrl_set(flash->fd, V4L2_CID_FLASH_LED_MODE, V4L2_FLASH_LED_MODE_NONE) < 0 && !first_error)
				first_error = errno;
			if (flash->saved) {
				if (ctrl_set(flash->fd, V4L2_CID_FLASH_TIMEOUT, flash->timeout) < 0 && !first_error)
					first_error = errno;
				if (ctrl_set(flash->fd, V4L2_CID_FLASH_INTENSITY, flash->intensity) < 0 && !first_error)
					first_error = errno;
				if (ctrl_set(flash->fd, V4L2_CID_FLASH_STROBE_SOURCE, flash->source) < 0 && !first_error)
					first_error = errno;
			}
		}
		close(flash->fd);
		*flash = (struct camera_flash){ .fd = -1 };
	}
	c->flash_count = 0;
	return -first_error;
}

static int camera_flash_open(struct cam *c)
{
	struct topo topology;
	const struct media_v2_entity *sensor;
	int media_fd = find_media(), error = ENODEV;

	if (media_fd < 0) return -errno;
	if (topo_load(&topology, media_fd) < 0) { close(media_fd); return -errno; }
	sensor = topo_entity(&topology, c->cfg->sensor);
	if (!sensor) goto out;
	for (unsigned int i = 0; i < topology.t.num_links; i++) {
		const struct media_v2_link *link = &topology.links[i];

		if ((link->flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_ANCILLARY_LINK ||
		    link->source_id != sensor->id) continue;
		for (unsigned int j = 0; j < topology.t.num_entities; j++) {
			const struct media_v2_entity *entity = &topology.ents[j];
			struct camera_flash *flash;
			int value = 0, ret;

			if (entity->id != link->sink_id || entity->function != MEDIA_ENT_F_FLASH) continue;
			if (c->flash_count == PHOTO_FLASH_CHANNELS) { error = E2BIG; goto out; }
			flash = &c->flash[c->flash_count++];
			*flash = (struct camera_flash){ .fd = topo_open_devnode(&topology, entity->id) };
			if (flash->fd < 0) { error = errno; goto out; }
#define READ_FLASH(id, field) do { \
	ret = flash_get(flash->fd, id, &flash->field); \
	if (ret < 0) { error = -ret; goto out; } \
} while (0)
			READ_FLASH(V4L2_CID_FLASH_LED_MODE, mode);
			if (flash->mode != V4L2_FLASH_LED_MODE_NONE) { error = EBUSY; goto out; }
			READ_FLASH(V4L2_CID_FLASH_STROBE_SOURCE, source);
			READ_FLASH(V4L2_CID_FLASH_TIMEOUT, timeout);
			READ_FLASH(V4L2_CID_FLASH_INTENSITY, intensity);
#undef READ_FLASH
			ret = flash_get(flash->fd, V4L2_CID_FLASH_FAULT, &value);
			if (ret < 0) { error = -ret; goto out; }
			if (value) { error = EIO; goto out; }
			flash->timeout_range = (struct v4l2_query_ext_ctrl){ .id = V4L2_CID_FLASH_TIMEOUT };
			if (xioctl(flash->fd, VIDIOC_QUERY_EXT_CTRL, &flash->timeout_range) < 0) {
				error = errno; goto out;
			}
			flash->saved = true;
		}
	}
	if (c->flash_count) error = 0;
out:
	topo_free(&topology);
	close(media_fd);
	if (error) {
		uint64_t stop_ns; uint32_t fault;

		(void)camera_flash_stop(c, &stop_ns, &fault);
	}
	return -error;
}

static bool camera_photo_ready(void *context)
{
	const struct cam *c = context;

	return !c->photo_recover_frames && (c->ae_manual || c->ae_ready);
}

static int camera_photo_freeze(void *context, struct piano_photo_settings *settings)
{
	struct cam *c = context;
	struct v4l2_query_ext_ctrl exposure = { .id = V4L2_CID_EXPOSURE };
	struct v4l2_subdev_format format = { .which = V4L2_SUBDEV_FORMAT_ACTIVE, .pad = 0 };
	struct exposure_timing timing;
	struct v4l2_control blank = { .id = V4L2_CID_VBLANK };
	int ret;

	if (!c->streaming || c->quad_raw || !c->photo_timing.readout_ns || strcmp(c->cfg->sensor, "s5kjn1")) return -EOPNOTSUPP;
	if (control_refresh(c, DOMAIN_AE) < 0 || control_refresh(c, DOMAIN_WB) < 0 ||
	    (c->vcm_fd >= 0 && control_refresh(c, DOMAIN_AF) < 0) ||
	    xioctl(c->sensor_fd, VIDIOC_QUERY_EXT_CTRL, &exposure) < 0 ||
	    exposure_timing(c, &exposure, &timing) < 0 ||
	    xioctl(c->sensor_fd, VIDIOC_SUBDEV_G_FMT, &format) < 0 ||
	    xioctl(c->sensor_fd, VIDIOC_G_CTRL, &blank) < 0)
		return -errno;
	if (c->exposure <= 0 || blank.value < 0 || !format.format.height) return -ERANGE;
	ret = camera_flash_open(c);
	if (ret < 0) return ret;
	memset(settings, 0, sizeof(*settings));
	settings->line_ns_num = timing.line_ns_num; settings->line_ns_den = timing.line_ns_den;
	settings->exposure_ns = ((uint64_t)c->exposure * timing.line_ns_num + timing.line_ns_den - 1) / timing.line_ns_den;
	/* Observed physical SOF -> EOF, not a height/line-time estimate.
	 * Acceptance still checks the selected frame's actual endpoints. */
	settings->readout_ns = c->photo_timing.readout_ns;
	settings->frame_ns = exposure_ns(&timing, (uint64_t)format.format.height + blank.value);
	settings->exposure_lines = (uint32_t)c->exposure;
	settings->native_width = format.format.width; settings->native_height = format.format.height;
	settings->flash_channels = c->flash_count;
	for (unsigned int i = 0; i < c->flash_count; i++) {
		const struct v4l2_query_ext_ctrl *range = &c->flash[i].timeout_range;

		if (range->type != V4L2_CTRL_TYPE_INTEGER || range->minimum <= 0 ||
		    range->maximum < range->minimum || !range->step || range->maximum > INT_MAX) {
			ret = -EOPNOTSUPP; goto fail;
		}
		if (i && (settings->pulse_min_ns != (uint64_t)range->minimum * 1000 ||
			  settings->pulse_max_ns != (uint64_t)range->maximum * 1000 ||
			  settings->pulse_step_ns != range->step * 1000)) {
			ret = -EOPNOTSUPP; goto fail;
		}
		settings->pulse_min_ns = (uint64_t)range->minimum * 1000;
		settings->pulse_max_ns = (uint64_t)range->maximum * 1000;
		settings->pulse_step_ns = range->step * 1000;
	}
	c->saved_ae_manual = c->ae_manual; c->saved_wb_manual = c->wb_manual;
	c->saved_af_manual = c->af_manual;
	c->ae_manual = c->wb_manual = c->af_manual = true;
	settings->freeze_ns = monotonic_ns();
	return 0;
fail:
	{
		uint64_t stop_ns; uint32_t fault;

		(void)camera_flash_stop(c, &stop_ns, &fault);
	}
	return ret;
}

static int camera_flash_start(void *context, uint64_t pulse_ns, uint64_t *call_ns, uint64_t *return_ns)
{
	struct cam *c = context;

	for (unsigned int i = 0; i < c->flash_count; i++) {
		struct camera_flash *flash = &c->flash[i];

		/* A failed control call may still have changed hardware. */
		flash->configured = true;
		if (ctrl_set(flash->fd, V4L2_CID_FLASH_TIMEOUT, (int)(pulse_ns / 1000)) < 0 ||
		    ctrl_set(flash->fd, V4L2_CID_FLASH_STROBE_SOURCE, V4L2_FLASH_STROBE_SOURCE_SOFTWARE) < 0)
			return -errno;
		/* Mark before the call: a failed transport may still have changed
		 * hardware, so cleanup must try STOP and NONE on this channel.
		 */
		if (ctrl_set(flash->fd, V4L2_CID_FLASH_LED_MODE, V4L2_FLASH_LED_MODE_FLASH) < 0)
			return -errno;
	}
	*call_ns = monotonic_ns();
	for (unsigned int i = 0; i < c->flash_count; i++)
		if (ctrl_set(c->flash[i].fd, V4L2_CID_FLASH_STROBE, 0) < 0) return -errno;
	*return_ns = monotonic_ns();
	return 0;
}

static void camera_photo_restore(void *context)
{
	struct cam *c = context;

	c->ae_manual = c->saved_ae_manual;
	c->wb_manual = c->saved_wb_manual;
	c->af_manual = c->saved_af_manual;
	/* Do not feed the light pulse or the two in-flight frames into the
	 * restored ambient-light AE/AWB/AF loops.
	 */
	c->photo_recover_frames = 2;
}

static const struct piano_photo_ops camera_photo_ops = {
	.ready = camera_photo_ready, .freeze = camera_photo_freeze,
	.start_flash = camera_flash_start, .stop_flash = camera_flash_stop,
	.restore = camera_photo_restore,
};

static int camera_photo_request(struct cam *c, int client_fd)
{
	int ret;

	if (strcmp(c->cfg->name, "rear") || c->cfg->in_w == 8160) return -EOPNOTSUPP;
	if (piano_photo_active(&c->photo)) return -EBUSY;
	if (!c->streaming) {
		if (cam_start(c) < 0) return -errno;
		c->photo_owned_stream = true;
	}
	ret = piano_photo_begin(&c->photo, client_fd, c->frame_sync_subscribed, monotonic_ns());
	if (ret < 0 && c->photo_owned_stream && !c->loop_users) cam_stop(c);
	return ret;
}

static void camera_photo_frame(struct cam *c, const struct v4l2_buffer *buffer,
			       const uint8_t *y, const uint8_t *uv, bool complete)
{
	struct piano_photo_frame frame = {
		.y = y, .uv = uv, .width = c->cfg->out_w, .height = c->cfg->out_h,
		.stride = c->bpl, .colorspace = c->colorspace, .ycbcr_enc = c->ycbcr_enc,
		.quantization = c->quantization, .sequence = buffer->sequence,
		.now_ns = monotonic_ns(), .complete = complete,
	};

	frame.physical_timing = photo_physical_times(c, buffer, &frame) == 0;
	if (piano_photo_active(&c->photo)) piano_photo_frame(&c->photo, &frame);
}

static int local_video_gid(gid_t *gid)
{
	FILE *file = fopen("/etc/group", "re");
	struct group *group;
	int found = 0;

	if (!file)
		return -1;
	while ((group = fgetgrent(file))) {
		if (!strcmp(group->gr_name, "video")) {
			*gid = group->gr_gid;
			found = 1;
			break;
		}
	}
	fclose(file);
	return found ? 0 : -1;
}

static int control_open(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX, .sun_path = CONTROL_PATH };
	gid_t video_gid;
	struct stat st;
	mode_t old;
	int fd, error;

	if (geteuid() != 0 || local_video_gid(&video_gid)) {
		errno = EPERM;
		return -1;
	}
	if (mkdir("/run/piano-camerad", 0750) < 0 && errno != EEXIST)
		return -1;
	if (lstat("/run/piano-camerad", &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0) {
		errno = EPERM;
		return -1;
	}
	if (chown("/run/piano-camerad", 0, video_gid) < 0 || chmod("/run/piano-camerad", 0750) < 0)
		return -1;
	if (!lstat(CONTROL_PATH, &st)) {
		if (!S_ISSOCK(st.st_mode) || st.st_uid != 0) {
			errno = EEXIST;
			return -1;
		}
		if (unlink(CONTROL_PATH) < 0)
			return -1;
	} else if (errno != ENOENT) {
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	old = umask(0077);
	int bound = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	umask(old);
	if (bound < 0)
		goto fail;
	if (chown(CONTROL_PATH, 0, video_gid) < 0 || chmod(CONTROL_PATH, 0660) < 0 ||
	    listen(fd, CONTROL_CLIENTS) < 0) {
		error = errno;
		unlink(CONTROL_PATH);
		errno = error;
		goto fail;
	}
	return fd;
fail:
	error = errno;
	close(fd);
	errno = error;
	return -1;
}

static void control_accept(void)
{
	int fd = accept4(control_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

	if (fd < 0)
		return;
	for (unsigned int i = 0; i < CONTROL_CLIENTS; i++)
		if (control_clients[i] < 0) {
			control_clients[i] = fd;
			return;
		}
	close(fd);
}

static void control_receive(unsigned int i)
{
	char packet[256], *reply = NULL;
	size_t bytes = 0;
	int fd = control_clients[i];
	ssize_t n = recv(fd, packet, sizeof(packet) - 1, MSG_TRUNC);

	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n > 0 && n < (ssize_t)sizeof(packet) && !memchr(packet, 0, (size_t)n)) {
		packet[n] = 0;
		if (!strncmp(packet, "capture ", 8)) {
			char camera[16], mode[16], extra[2];
			int ret = -EINVAL;

			if (sscanf(packet, "capture %15s %15s %1s", camera, mode, extra) == 2 &&
			    !strcmp(mode, "flash")) {
				for (unsigned int k = 0; k < NUM_CAMS; k++)
					if (!strcmp(camera, cams[k].cfg->name)) ret = camera_photo_request(&cams[k], fd);
			}
			control_clients[i] = -1;
			if (ret < 0) {
				struct piano_photo_result result = {
					.magic = PIANO_PHOTO_MAGIC, .version = PIANO_PHOTO_VERSION,
					.status = (uint32_t)-ret,
				};

				(void)piano_photo_send(fd, &result, -1);
				close(fd);
			}
			return;
		}
	}
	if (n > 0) {
		FILE *out = open_memstream(&reply, &bytes);

		if (out) {
			if (n >= (ssize_t)sizeof(packet) || memchr(packet, 0, (size_t)n))
				control_error(out, "packet", EMSGSIZE, false);
			else {
				packet[n] = 0;
				control_request(out, packet);
			}
			fclose(out);
			if (send(fd, reply, bytes, MSG_NOSIGNAL) < 0)
				logmsg(NULL, "control reply: %s", strerror(errno));
			free(reply);
		}
	}
	close(fd);
	control_clients[i] = -1;
}

static void control_close(void)
{
	for (unsigned int i = 0; i < CONTROL_CLIENTS; i++)
		if (control_clients[i] >= 0)
			close(control_clients[i]);
	if (control_fd >= 0) {
		close(control_fd);
		unlink(CONTROL_PATH);
	}
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts("piano-camerad: Linux Piano runtime helper; --help performs no device access.");
		return 0;
	}
	if (argc != 1) {
		fputs("Use --help or no arguments.\n", stderr);
		return 2;
	}

	struct sigaction sa = { .sa_handler = on_signal };
	unsigned int ncams = 0;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	setvbuf(stderr, NULL, _IOLBF, 0);

	if (getenv("PIANO_CAMERA_SATURATION"))
		saturation = atoi(getenv("PIANO_CAMERA_SATURATION")) *
			     SAT_UNITY / 100;
	if (getenv("PIANO_CAMERA_CONTRAST"))
		contrast = atoi(getenv("PIANO_CAMERA_CONTRAST"));
	af_settle_frames = AF_SETTLE;
	if (getenv("PIANO_CAMERA_AF_SETTLE"))
		af_settle_frames = atoi(getenv("PIANO_CAMERA_AF_SETTLE"));

	for (int i = 0; i < 256; i++) {
		double v = i / 255.0;

		srgb_lin[i] = v <= 0.04045 ? v / 12.92 :
			      pow((v + 0.055) / 1.055, 2.4);
	}

	for (unsigned int i = 0; i < NUM_CAMS; i++) {
		cams[i].mode = cam_cfgs[i];
		cams[i].cfg = &cams[i].mode;
		piano_photo_init(&cams[i].photo, &camera_photo_ops, &cams[i]);
		for (unsigned int j = 0; j < PHOTO_FLASH_CHANNELS; j++) cams[i].flash[j].fd = -1;
		cams[i].vid_fd = cams[i].sensor_fd = cams[i].vfe_fd =
			cams[i].vcm_fd = -1;
		if (loopback_init(&cams[i]) == 0)
			ncams++;
		else if (cams[i].loop_fd >= 0)
			loopback_close(&cams[i]);
	}
	if (!ncams) {
		logmsg(NULL, "no camera outputs");
		return 1;
	}
	control_fd = control_open();
	if (control_fd < 0) {
		logmsg(NULL, "control socket: %s", strerror(errno));
		for (unsigned int i = 0; i < NUM_CAMS; i++) {
			cam_stop(&cams[i]);
			loopback_close(&cams[i]);
		}
		return 1;
	}
	logmsg(NULL, "ready, %u camera(s)", ncams);
	notify_ready();

	while (!quit) {
		struct pollfd pfd[4 * NUM_CAMS + 1 + CONTROL_CLIENTS];
		struct cam *owner[4 * NUM_CAMS + 1 + CONTROL_CLIENTS];
		unsigned int kind[4 * NUM_CAMS + 1 + CONTROL_CLIENTS];
		unsigned int n = 0;
		int timeout = 1000;

		for (unsigned int i = 0; i < NUM_CAMS; i++) {
			struct cam *c = &cams[i];
			int photo_timeout = piano_photo_poll_timeout(&c->photo, monotonic_ns());

			if (photo_timeout < timeout) timeout = photo_timeout;
			if (piano_photo_active(&c->photo)) {
				pfd[n] = (struct pollfd){ .fd = c->photo.client_fd, .events = POLLIN };
				owner[n] = c;
				kind[n++] = POLL_PHOTO_CANCEL;
			}
			if (c->loop_fd < 0)
				continue;
			pfd[n] = (struct pollfd){ .fd = c->loop_fd,
						  .events = POLLPRI };
			owner[n] = c;
			kind[n++] = 0;
			if (c->streaming) {
				pfd[n] = (struct pollfd){ .fd = c->vid_fd,
							  .events = POLLIN | POLLPRI };
				owner[n] = c;
				kind[n++] = 1;
				if (c->quad_worker) {
					pfd[n] = (struct pollfd){ .fd = quad_worker_fd(c->quad_worker),
								  .events = POLLIN };
					owner[n] = c;
					kind[n++] = POLL_QUAD_RESULT;
				}
			}
		}

		pfd[n] = (struct pollfd){ .fd = control_fd, .events = POLLIN };
		owner[n] = NULL;
		kind[n++] = 2;
		for (unsigned int i = 0; i < CONTROL_CLIENTS; i++) {
			if (control_clients[i] < 0)
				continue;
			pfd[n] = (struct pollfd){ .fd = control_clients[i], .events = POLLIN };
			owner[n] = NULL;
			kind[n++] = 3 + i;
		}

		if (poll(pfd, n, timeout) < 0) {
			if (errno == EINTR)
				continue;
			logmsg(NULL, "poll: %s", strerror(errno));
			break;
		}

		for (unsigned int i = 0; i < n; i++) {
			if (!pfd[i].revents)
				continue;
			if (kind[i] == 2) {
				if (pfd[i].revents & POLLIN)
					control_accept();
			} else if (kind[i] == POLL_PHOTO_CANCEL) {
				struct cam *c = owner[i];

				if (piano_photo_active(&c->photo) && c->photo.client_fd == pfd[i].fd)
					piano_photo_abort(&c->photo, ECANCELED, false);
			} else if (kind[i] == POLL_QUAD_RESULT) {
				/* A source-change event earlier in this poll batch may have
				 * closed the old stream. Never consume its stale result fd.
				 */
				if (owner[i]->streaming && owner[i]->quad_worker &&
				    pfd[i].fd == quad_worker_fd(owner[i]->quad_worker) &&
				    (pfd[i].revents & POLLIN))
					cam_quad_result(owner[i]);
			} else if (kind[i] >= 3) {
				control_receive(kind[i] - 3);
			} else if (kind[i] == 1) {
				struct cam *c = owner[i];

				if (!c->streaming || c->vid_fd != pfd[i].fd) continue;
				if (pfd[i].revents & (POLLHUP | POLLERR)) piano_photo_abort(&c->photo, EIO, true);
				/* A physical SOF may be queued even when poll only reported
				 * readable buffers. Drain first in either case. */
				camera_frame_events(c);
				if (pfd[i].revents & POLLIN) cam_frame(c);
			} else if (pfd[i].revents & POLLPRI) {
				loopback_event(owner[i]);
			}
		}
		for (unsigned int i = 0; i < NUM_CAMS; i++) {
			struct cam *c = &cams[i];

			piano_photo_tick(&c->photo, monotonic_ns());
			/* cam_frame has requeued the current DMA buffer by this point. */
			if (!piano_photo_active(&c->photo) && c->photo_owned_stream && !c->loop_users)
				cam_stop(c);
		}
	}

	for (unsigned int i = 0; i < NUM_CAMS; i++) {
		cam_stop(&cams[i]);
		loopback_close(&cams[i]);
	}
	control_close();
	logmsg(NULL, "exit");

	return exit_status;
}
