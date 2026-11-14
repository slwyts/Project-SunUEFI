// SPDX-License-Identifier: BSD-3-Clause
/* Exercise the patched root initialization on an ordinary temporary file.
 * The version callback is local; no device or secure application is opened.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/tee.h>
#include "qcomtee_object_private.h"

static struct tee_ioctl_version_data advertised;
static unsigned int calls, releases;
static int opened_fd, version_error;

static int version_call(int fd, unsigned long operation, ...)
{
	va_list args;

	assert(operation == TEE_IOC_VERSION);
	assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
	assert((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR);
	opened_fd = fd;
	calls++;
	if (version_error) {
		errno = version_error;
		return -1;
	}
	va_start(args, operation);
	struct tee_ioctl_version_data *version = va_arg(args, void *);
	va_end(args);
	assert(!version->impl_id && !version->impl_caps && !version->gen_caps);
	*version = advertised;
	return 0;
}

static void released(void *context)
{
	assert(context == &releases);
	releases++;
}

static void rejected(const char *path, uint32_t implementation, uint32_t capabilities)
{
	advertised = (struct tee_ioctl_version_data){
		.impl_id = implementation, .gen_caps = capabilities,
	};
	unsigned int before = calls;
	assert(qcomtee_object_root_init(path, version_call, released, &releases) == NULL);
	assert(errno == EOPNOTSUPP && calls == before + 1 && releases == 0);
	assert(fcntl(opened_fd, F_GETFD) == -1 && errno == EBADF);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	int file = open(argv[1], O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(file >= 0);
	close(file);

	rejected(argv[1], TEE_IMPL_ID_OPTEE, TEE_GEN_CAP_GP);
	rejected(argv[1], TEE_IMPL_ID_OPTEE, TEE_GEN_CAP_OBJREF);
	rejected(argv[1], TEE_IMPL_ID_QTEE, TEE_GEN_CAP_GP);
	rejected(argv[1], TEE_IMPL_ID_QTEE, 0);

	version_error = ENOTTY;
	assert(qcomtee_object_root_init(argv[1], version_call, released, &releases) == NULL);
	assert(errno == ENOTTY && releases == 0);
	assert(fcntl(opened_fd, F_GETFD) == -1 && errno == EBADF);
	version_error = 0;
	advertised = (struct tee_ioctl_version_data){
		.impl_id = TEE_IMPL_ID_QTEE, .gen_caps = TEE_GEN_CAP_OBJREF,
	};
	struct qcomtee_object *root = qcomtee_object_root_init(argv[1], version_call,
							    released, &releases);
	assert(root && root->object_type == QCOMTEE_OBJECT_TYPE_ROOT && root->root == root);
	assert(root->tee_object_id == TEE_OBJREF_NULL);
	assert(ROOT_OBJECT(root)->fd == opened_fd);
	qcomtee_object_refs_dec(root);
	assert(releases == 1);
	assert(fcntl(opened_fd, F_GETFD) == -1 && errno == EBADF);

	assert(!unlink(argv[1]));
	unsigned int before = calls;
	assert(qcomtee_object_root_init(argv[1], version_call, released, &releases) == NULL);
	assert(errno == ENOENT && calls == before && releases == 1);
	return 0;
}
