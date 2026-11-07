/* Exercise the actual CLI with a socketpair peer. The peer sends only a
 * generated neutral NV12 frame; no production socket or camera is used.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include "../photo/protocol.h"

static int client_socket;
int __wrap_socket(int domain, int type, int protocol)
{ assert(domain == AF_UNIX && (type & SOCK_SEQPACKET) && protocol == 0); return dup(client_socket); }
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t size)
{
	const struct sockaddr_un *local = (const struct sockaddr_un *)address;
	(void)fd; (void)size;
	assert(!strcmp(local->sun_path, "/run/piano-camerad/control.sock"));
	return 0;
}
/* The test sandbox disallows sendto(), which libc send() uses. Use the
 * equivalent connected-socket sendmsg() without opening a network endpoint.
 */
ssize_t __wrap_send(int fd, const void *data, size_t bytes, int flags)
{
	struct iovec iov = { .iov_base = (void *)data, .iov_len = bytes };
	struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1 };

	return sendmsg(fd, &message, flags);
}

#define main ctl_entry_for_check
#include "../piano-camera-ctl.c"
#undef main

struct peer {
	int fd, image_fd;
	struct piano_photo_result result;
};
static void *reply(void *data)
{
	struct peer *peer = data;
	char request[64] = {0};
	ssize_t bytes = recv(peer->fd, request, sizeof(request) - 1, 0);

	assert(bytes == strlen("capture rear flash") && !strcmp(request, "capture rear flash"));
	assert(piano_photo_send(peer->fd, &peer->result, peer->image_fd) == 0);
	close(peer->fd);
	return NULL;
}
static int run(const char *path, int error)
{
	uint8_t y[32], uv[16];
	int sockets[2]; pthread_t thread;
	struct peer peer = {
		.image_fd = -1,
		.result = {
			.magic = PIANO_PHOTO_MAGIC, .version = PIANO_PHOTO_VERSION, .status = (uint32_t)error,
			.flags = PIANO_PHOTO_PHYSICAL_EOF | PIANO_PHOTO_FLASH_COMPLETE,
			.width = 8, .height = 4, .stride = 8, .capture_stride = 8,
			.pixelformat = V4L2_PIX_FMT_NV12, .colorspace = V4L2_COLORSPACE_REC709,
			.ycbcr_enc = V4L2_YCBCR_ENC_709, .quantization = V4L2_QUANTIZATION_LIM_RANGE,
			.sequence = 5, .flash_channels = 2, .exposure_lines = 256,
			.native_width = 4080, .native_height = 3060, .data_bytes = 48,
			.eof_ns = 1055000000, .exposure_start_ns = 1010000000, .exposure_end_ns = 1044921374,
			.light_start_ns = 1007000000, .light_end_ns = 1060000000,
			.strobe_call_ns = 1000000000, .strobe_return_ns = 1002000000,
			.timeout_ns = 90000000, .stop_call_ns = 1060000000,
			.exposure_ns = 2696794, .readout_ns = 32224580,
			.line_ns_num = 10534351, .line_ns_den = 1000, .frame_ns = 33330687,
			.freeze_ns = 900000001, .sof_ns = 1013707328,
		},
	};

	memset(y, 128, sizeof(y)); memset(uv, 128, sizeof(uv));
	if (!error) { peer.image_fd = piano_photo_image(y, uv, 8, 4, 8); assert(peer.image_fd >= 0); }
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
	client_socket = sockets[0]; peer.fd = sockets[1];
	assert(pthread_create(&thread, NULL, reply, &peer) == 0);
	char *arguments[] = { "piano-camera-ctl", "capture", "rear", "--flash", "--output", (char *)path };
	int result = ctl_entry_for_check(6, arguments);
	assert(pthread_join(thread, NULL) == 0);
	close(client_socket); if (peer.image_fd >= 0) close(peer.image_fd);
	return result;
}

int main(void)
{
	char path[] = "/tmp/piano-photo-cli-XXXXXX";
	int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);

	assert(run(path, 0) == 0);
	FILE *image = fopen(path, "rb"); assert(image);
	assert(fgetc(image) == 0xff && fgetc(image) == 0xd8);
	assert(fseek(image, -2, SEEK_END) == 0 && fgetc(image) == 0xff && fgetc(image) == 0xd9);
	fclose(image); unlink(path);

	assert(run(path, EOPNOTSUPP) == 1);
	assert(access(path, F_OK) < 0 && errno == ENOENT);

	image = fopen(path, "wb"); assert(image); assert(fwrite("KEEP", 1, 4, image) == 4); fclose(image);
	assert(run(path, 0) == 1);
	image = fopen(path, "rb"); assert(image); char saved[5] = {0};
	assert(fread(saved, 1, 4, image) == 4 && !strcmp(saved, "KEEP"));
	fclose(image); unlink(path);
	puts("3 actual CLI/JPEG/output-file checks passed without a production socket");
	return 0;
}
