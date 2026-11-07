// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "photo/jpeg.h"

#define CONTROL_PATH "/run/piano-camerad/control.sock"

static int capture(const char *camera, const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX, .sun_path = CONTROL_PATH };
	struct piano_photo_result result;
	struct pollfd wait;
	char request[64];
	FILE *output = NULL;
	int fd = -1, image_fd = -1, output_fd = -1, ret = 0, ready;
	bool created = false;

	if (strcmp(camera, "rear")) { fprintf(stderr, "Flash capture is available only for the rear camera.\n"); return 2; }
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { ret = -errno; goto out; }
	int length = snprintf(request, sizeof(request), "capture %s flash", camera);
	if (send(fd, request, (size_t)length, MSG_NOSIGNAL) != length) { ret = -errno; goto out; }
	wait = (struct pollfd){ .fd = fd, .events = POLLIN };
	do { ready = poll(&wait, 1, 6000); } while (ready < 0 && errno == EINTR);
	if (ready <= 0) { ret = ready < 0 ? -errno : -ETIMEDOUT; goto out; }
	ret = piano_photo_receive(fd, &result, &image_fd);
	if (ret < 0) goto out;
	if (result.status) { ret = -(int)result.status; goto out; }
	if (!strcmp(path, "-")) output = stdout;
	else {
		/* The unprivileged client creates its own output; the root camera
		 * owner never receives or opens a user-selected filesystem path.
		 */
		output_fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
		if (output_fd < 0) { ret = -errno; goto out; }
		created = true;
		output = fdopen(output_fd, "wb");
		if (!output) { ret = -errno; close(output_fd); output_fd = -1; goto out; }
	}
	ret = piano_photo_jpeg(image_fd, &result, output);
	if (output != stdout) {
		if (fclose(output) != 0 && !ret) ret = -errno;
		output = NULL;
	}
	if (!ret) fprintf(stderr, "Captured %ux%u JPEG (physical frame %u).\n", result.width, result.height, result.sequence);
out:
	if (image_fd >= 0) close(image_fd);
	if (fd >= 0) close(fd);
	if (ret && created) unlink(path);
	if (ret == -EOPNOTSUPP)
		fprintf(stderr, "This camera mode does not support synchronized flash capture.\n");
	else if (ret) fprintf(stderr, "Photo capture: %s\n", strerror(-ret));
	return ret ? 1 : 0;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream, "Usage: %s caps|get rear|front\n"
			"       %s capture rear --flash --output FILE|-\n"
			"       %s set rear|front ae|awb|af auto|manual\n"
			"       %s set rear|front exposure|analog-gain|digital-gain|red-balance|blue-balance|focus INTEGER\n"
			"       %s set rear|front exposure-time-ns NANOSECONDS\n"
			"The camera must already be streaming. Exposure time is quantized to sensor lines; caps reports the active mode and live ranges. Other values use driver-native units.\n",
			program, program, program, program, program);
}

int main(int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX, .sun_path = CONTROL_PATH };
	char packet[256], reply[8192];
	size_t used = 0;
	int fd;
	ssize_t n;

	if (argc == 6 && !strcmp(argv[1], "capture") && !strcmp(argv[3], "--flash") &&
	    !strcmp(argv[4], "--output"))
		return capture(argv[2], argv[5]);

	if (argc == 2 && !strcmp(argv[1], "--help")) {
		usage(stdout, argv[0]);
		return 0;
	}

	if (argc != 3 && argc != 5) {
		usage(stderr, argv[0]);
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		if (!argv[i][0] || strpbrk(argv[i], " \t\r\n"))
			return 2;
		int bytes = snprintf(packet + used, sizeof(packet) - used, "%s%s", i == 1 ? "" : " ", argv[i]);
		if (bytes < 0 || (size_t)bytes >= sizeof(packet) - used)
			return 2;
		used += (size_t)bytes;
	}
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("camera control connect");
		if (fd >= 0)
			close(fd);
		return 1;
	}
	if (send(fd, packet, used, MSG_NOSIGNAL) != (ssize_t)used) {
		perror("camera control send");
		close(fd);
		return 1;
	}
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int ready;
	do { ready = poll(&pfd, 1, 3000); } while (ready < 0 && errno == EINTR);
	if (ready <= 0) {
		fprintf(stderr, "No control reply; query current state before retrying a write.\n");
		close(fd);
		return 1;
	}
	do { n = recv(fd, reply, sizeof(reply) - 1, MSG_TRUNC); } while (n < 0 && errno == EINTR);
	close(fd);
	if (n <= 0 || n >= (ssize_t)sizeof(reply) || memchr(reply, 0, (size_t)n)) {
		fprintf(stderr, "Invalid or missing control reply.\n");
		return 1;
	}
	reply[n] = 0;
	fputs(reply, stdout);
	return strstr(reply, "\"ok\":true") ? 0 : 1;
}
