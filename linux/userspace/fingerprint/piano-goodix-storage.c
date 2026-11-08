// SPDX-License-Identifier: BSD-3-Clause
/* Restricted FS/GPFS services using Qualcomm's public packed messages.
 * CRegisterListenerCBO and IListenerCBO operations follow minkipc's IDLs.
 * Requests carry no TA identity. Only the explicit directory is accessible.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "MinkCom.h"
#include "fs_msg.h"
#include "gpfs_msg.h"
#include "piano-goodix-rpmb.h"
#include "piano-goodix-storage.h"

/* The factory TA uses legacy IDs 0..3 with the public GPFS message layout. */
enum { GPFS_READ, GPFS_WRITE, GPFS_REMOVE, GPFS_RENAME };
enum { RPMB_SERVICE_ID = 0x2000, RPMB_BUFFER_BYTES = 20040, LISTENER_COUNT = 3 };
enum storage_mode { STORAGE_EMPTY, STORAGE_PROFILE, STORAGE_ENROLL,
	STORAGE_RPMB_DENY, STORAGE_RPMB_UFS };

_Static_assert(sizeof(tz_file_open_req_t) == 264 &&
	       offsetof(tz_file_open_req_t, flags) == 260, "FS open request ABI");
_Static_assert(sizeof(tz_file_close_req_t) == 8 && sizeof(tz_file_open_rsp_t) == 8 &&
	       sizeof(tz_file_close_rsp_t) == 8 && sizeof(tz_file_get_errno_rsp_t) == 8,
	       "FS close/response ABI");

struct storage_listener {
	struct goodix_storage *storage;
	Object registration, memory;
	void *shared, *work;
	size_t size;
	uint32_t service;
};

struct goodix_storage {
	atomic_uint references;
	pthread_mutex_t mutex;
	pthread_cond_t released;
	struct storage_listener listeners[LISTENER_COUNT];
	struct { int fd, handle; } files[FOPEN_MAX];
	int next_handle, file_error;
	int directory;
	int profile;
	int stopped;
	struct goodix_rpmb *rpmb;
	int allow_rpmb;
	unsigned int rpmb_requests;
};

static int storage_filename(const char path[TZ_CM_MAX_NAME_LEN], const char **name)
{
	const char prefix[] = "/data/vendor_de/0/fpdata/";
	size_t length = strnlen(path, TZ_CM_MAX_NAME_LEN);

	if (length == TZ_CM_MAX_NAME_LEN ||
	    strncmp(path, prefix, sizeof(prefix) - 1))
		return EACCES;
	*name = path + sizeof(prefix) - 1;
	if (!**name || !strcmp(*name, ".") || !strcmp(*name, ".."))
		return EACCES;
	for (const unsigned char *p = (const unsigned char *)*name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
			return EACCES;
	return 0;
}

static void log_path(const char *path)
{
	fputc('"', stderr);
	for (size_t i = 0; i < TZ_CM_MAX_NAME_LEN && path[i]; i++) {
		unsigned char c = path[i];
		if (c >= 32 && c < 127 && c != '"' && c != '\\')
			fputc(c, stderr);
		else
			fprintf(stderr, "\\x%02x", c);
	}
	fputc('"', stderr);
}

static int storage_file(struct goodix_storage *storage, const char *name, int flags)
{
	struct stat st;
	int fd = openat(storage->directory, name,
			flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st)) {
		int error = errno;
		close(fd);
		return -error;
	}
	if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
	    (st.st_mode & 0077) ||
	    st.st_size < 0) {
		close(fd);
		return -EACCES;
	}
	return fd;
}

/* Files can span several GPFS messages. Copy in the public listener's 64 KiB
 * I/O window rather than allocating the entire file in the fingerprint process.
 */
static int storage_copy(int source, int destination, off_t size)
{
	uint8_t buffer[MAX_READ_SIZE];
	off_t position = 0;
	int error = 0;

	while (position < size) {
		size_t count = (uintmax_t)(size - position) < sizeof(buffer) ?
			(size_t)(size - position) : sizeof(buffer);
		ssize_t bytes = pread(source, buffer, count, position);
		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0) {
			error = bytes < 0 ? errno : EIO;
			break;
		}
		size_t done = 0;
		while (done < (size_t)bytes) {
			ssize_t written = pwrite(destination, buffer + done, bytes - done,
						 position + done);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0) {
				error = written < 0 ? errno : EIO;
				break;
			}
			done += written;
		}
		if (error)
			break;
		position += bytes;
	}
	explicit_bzero(buffer, sizeof(buffer));
	return error;
}

/* Publish a complete file only after its data is durable. All names are flat
 * components in the locked private directory; never truncate an unchecked inode.
 * Preserve the old file, then overwrite this message's range. pwrite zero-fills
 * any hole without allocating it in userspace.
 */
static int storage_replace(struct goodix_storage *storage, const char *name,
			   int source, off_t old_size, const void *data,
			   size_t count, off_t offset)
{
	char temporary[64];
	uint64_t random;
	size_t done = 0;
	int error = 0, fd = storage_file(storage, name, O_RDONLY);

	if (fd >= 0)
		close(fd);
	else if (fd != -ENOENT)
		return -fd;

	while (done < sizeof(random)) {
		ssize_t bytes = getrandom((uint8_t *)&random + done, sizeof(random) - done, 0);
		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return bytes < 0 ? errno : EIO;
		done += bytes;
	}
	snprintf(temporary, sizeof(temporary), ".piano-write-%016llx",
		 (unsigned long long)random);
	fd = openat(storage->directory, temporary,
		    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;

	error = storage_copy(source, fd, old_size);
	if (error)
		goto done;
	for (done = 0; done < count;) {
		ssize_t bytes = pwrite(fd, (const uint8_t *)data + done,
				       count - done, offset + done);
		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0) {
			error = bytes < 0 ? errno : EIO;
			goto done;
		}
		done += bytes;
	}
	if (fsync(fd)) {
		error = errno;
		goto done;
	}
	if (renameat(storage->directory, temporary, storage->directory, name)) {
		error = errno;
		goto done;
	}
	if (fsync(storage->directory))
		error = errno;
done:
	close(fd);
	if (error)
		unlinkat(storage->directory, temporary, 0);
	return error;
}

/* GPFS writes update a range, not an entire file. Retain the old suffix and
 * zero-fill a hole. A requested backup is committed before publishing new data.
 */
static int storage_write(struct goodix_storage *storage, const char *name,
			 const void *data, size_t count, off_t offset, int backup)
{
	struct stat st;
	off_t old_size = 0;
	int fd = storage_file(storage, name, O_RDONLY), error = 0;

	if (fd >= 0) {
		if (fstat(fd, &st)) {
			error = errno;
			close(fd);
			return error;
		}
		old_size = st.st_size;
	} else if (fd != -ENOENT) {
		return -fd;
	}
	if (backup) {
		char previous[TZ_CM_MAX_NAME_LEN + sizeof(".bak")];

		snprintf(previous, sizeof(previous), "%s.bak", name);
		error = storage_replace(storage, previous, fd, old_size, NULL, 0, 0);
	}
	if (!error)
		error = storage_replace(storage, name, fd, old_size, data, count, offset);
	if (fd >= 0)
		close(fd);
	return error;
}

static int storage_remove(struct goodix_storage *storage, const char *name)
{
	int fd = storage_file(storage, name, O_RDONLY);

	if (fd < 0)
		return -fd;
	close(fd);
	if (unlinkat(storage->directory, name, 0) || fsync(storage->directory))
		return errno;
	return 0;
}

static int storage_rename(struct goodix_storage *storage, const char *from, const char *to)
{
	int fd = storage_file(storage, from, O_RDONLY);

	if (fd < 0)
		return -fd;
	close(fd);
	fd = storage_file(storage, to, O_RDONLY);
	if (fd >= 0)
		close(fd);
	else if (fd != -ENOENT)
		return -fd;
	if (renameat(storage->directory, from, storage->directory, to) ||
	    fsync(storage->directory))
		return errno;
	return 0;
}

/* FTS checks existence with plain open/close before asking GPFS to unseal data.
 * Export private handles, never process file descriptors supplied by QTEE.
 */
static int storage_fs_dispatch(struct goodix_storage *storage, void *buffer, size_t size)
{
	tz_file_open_req_t request;
	tz_file_open_rsp_t response = {0};
	char pathname[TZ_CM_MAX_NAME_LEN] = {0};
	const char *name;
	int error = 0, handle = -1, flags = 0;

	if (size < sizeof(response))
		return -EMSGSIZE;
	memcpy(&response.cmd_id, buffer, sizeof(response.cmd_id));
	response.ret = -1;
	if (response.cmd_id == TZ_FS_MSG_CMD_FILE_OPEN) {
		if (size < sizeof(request))
			return -EMSGSIZE;
		memcpy(&request, buffer, sizeof(request));
		memcpy(pathname, request.pathname, sizeof(pathname));
		flags = request.flags;
		error = storage_filename(pathname, &name);
		if (!error && flags != O_RDONLY)
			error = EROFS;
		if (!error) {
			size_t slot;
			for (slot = 0; slot < FOPEN_MAX && storage->files[slot].handle; slot++)
				;
			if (slot == FOPEN_MAX || storage->next_handle == INT32_MAX) {
				error = EMFILE;
			} else {
				int fd = storage_file(storage, name, O_RDONLY);
				if (fd < 0) {
					error = -fd;
				} else {
					storage->files[slot].fd = fd;
					response.ret = ++storage->next_handle;
					storage->files[slot].handle = response.ret;
				}
			}
		}
	} else if (response.cmd_id == TZ_FS_MSG_CMD_FILE_CLOSE) {
		tz_file_close_req_t close_request;
		if (size < sizeof(close_request))
			return -EMSGSIZE;
		memcpy(&close_request, buffer, sizeof(close_request));
		handle = close_request.fd;
		error = EBADF;
		for (size_t slot = 0; slot < FOPEN_MAX; slot++) {
			if (!handle || storage->files[slot].handle != handle)
				continue;
			int result = close(storage->files[slot].fd);
			error = result ? errno : 0;
			storage->files[slot].handle = 0;
			response.ret = result;
			break;
		}
	} else if (response.cmd_id == TZ_FS_MSG_CMD_FILE_GET_ERRNO) {
		response.ret = storage->file_error;
	} else {
		storage->file_error = EOPNOTSUPP;
		fprintf(stderr, "FS command=%u unsupported\n", response.cmd_id);
		return -EOPNOTSUPP;
	}
	if (error)
		storage->file_error = error;
	fprintf(stderr, "FS command=%u path=", response.cmd_id);
	log_path(pathname);
	fprintf(stderr, " flags=%d handle=%d result=%d errno=%d\n",
		flags, handle, response.ret, error);
	memset(buffer, 0, size);
	memcpy(buffer, &response, sizeof(response));
	return 0;
}

/* Operate on one bounded flat file; no path resolution or directory creation. */
static int storage_dispatch(struct goodix_storage *storage, void *buffer, size_t size)
{
	tz_gpfile_read_req_t read_request;
	tz_gpfile_read_rsp_t *response = buffer;
	char pathname[TZ_CM_MAX_NAME_LEN] = {0};
	const char *name = NULL;
	uint32_t command, count = 0, backup = 0;
	int32_t offset = 0;
	int error = EOPNOTSUPP, fd = -1;
	size_t done = 0;

	if (size < sizeof(tz_gpfile_write_rsp_t))
		return -EMSGSIZE;
	memcpy(&command, buffer, sizeof(command));
	if (command == TZ_GPFS_MSG_CMD_GPFS_VERSION) {
		tz_gpfile_version_rsp_t version = {command, GP_FS_VERSION, 0};
		memset(buffer, 0, size);
		memcpy(buffer, &version, sizeof(version));
		fprintf(stderr, "GPFS command=%u version=%u\n", command, GP_FS_VERSION);
		return 0;
	}
	if (command <= GPFS_RENAME) {
		if (size < sizeof(tz_gpfile_remove_req_t))
			return -EMSGSIZE;
		memcpy(pathname, (uint8_t *)buffer + sizeof(command), sizeof(pathname));
		error = storage_filename(pathname, &name);
		if (error)
			goto reply;
	}
	if (command == GPFS_READ || command == GPFS_WRITE) {
		if (size < sizeof(read_request))
			return -EMSGSIZE;
		memcpy(&read_request, buffer, sizeof(read_request));
		offset = read_request.offset;
		count = read_request.count;
		/* The public header's fixed array is not a file-size limit. Legacy
		 * Goodix uses the whole registered message and sends larger files
		 * in offset-based chunks. Check the actual tail capacity below.
		 */
		uintmax_t end = (uintmax_t)(uint32_t)offset + count;
		off_t position = (off_t)end;
		if (offset < 0 || position < 0 || (uintmax_t)position != end) {
			error = EINVAL;
			goto reply;
		}
		if (command == GPFS_WRITE) {
			tz_gpfile_write_req_t *request = buffer;
			if (size < offsetof(tz_gpfile_write_req_t, buf) ||
			    count > size - offsetof(tz_gpfile_write_req_t, buf))
				return -EMSGSIZE;
			backup = request->backup;
			if (backup && !storage->profile) {
				error = EOPNOTSUPP;
				goto reply;
			}
			error = storage_write(storage, name,
				(uint8_t *)buffer + offsetof(tz_gpfile_write_req_t, buf),
				count, offset, backup);
			done = error ? 0 : count;
			goto reply;
		}
		if (size < offsetof(tz_gpfile_read_rsp_t, buf) ||
		    count > size - offsetof(tz_gpfile_read_rsp_t, buf))
			return -EMSGSIZE;
		fd = storage_file(storage, name, O_RDONLY);
		if (fd < 0) {
			error = -fd;
			goto reply;
		}
		error = 0;
		while (done < count) {
			ssize_t bytes;
			bytes = pread(fd, (uint8_t *)buffer + offsetof(tz_gpfile_read_rsp_t, buf) + done,
				      count - done, (off_t)offset + done);
			if (bytes < 0 && errno == EINTR)
				continue;
			if (bytes < 0) {
				error = errno;
				break;
			}
			if (!bytes)
				break;
			done += bytes;
		}
		close(fd);
	} else if (command == GPFS_REMOVE || command == GPFS_RENAME) {
		if (!storage->profile) {
			/* The empty-store diagnostic never deletes or renames a file. */
			error = EROFS;
		} else if (command == GPFS_REMOVE) {
			error = storage_remove(storage, name);
		} else {
			char destination[TZ_CM_MAX_NAME_LEN];
			const char *to;

			if (size < sizeof(tz_gpfile_rename_req_t))
				return -EMSGSIZE;
			memcpy(destination, ((tz_gpfile_rename_req_t *)buffer)->to,
			       sizeof(destination));
			error = storage_filename(destination, &to);
			if (!error)
				error = storage_rename(storage, name, to);
		}
	}
reply:
	fprintf(stderr, "GPFS command=%u path=", command);
	log_path(pathname);
	fprintf(stderr, " offset=%d count=%u backup=%u result=%d transferred=%zu\n",
		offset, count, backup, error, error ? 0 : done);
	if (command != GPFS_READ || error)
		memset(buffer, 0, size);
	response->cmd_id = command;
	response->err = error;
	response->num_bytes_read = error ? 0 : done;
	return 0;
}

/* minkipc's librpmbservice/rpmb_service.h defines these packed u32 headers.
 * Copy only the command's fixed header, never the following RPMB frames. The
 * negative response prevents secure-world reads, writes and partition changes.
 */
static int storage_rpmb_deny_dispatch(void *buffer, size_t size)
{
	uint32_t request[6] = {0}, response[5] = {0};
	size_t request_bytes, response_bytes;

	if (size < sizeof(uint32_t))
		return -EMSGSIZE;
	memcpy(request, buffer, sizeof(uint32_t));
	switch (request[0]) {
	case 0x101:
		request_bytes = 8;
		response_bytes = 20;
		break;
	case 0x102:
	case 0x103:
		request_bytes = 24;
		response_bytes = 20;
		break;
	case 0x104:
		request_bytes = 12;
		response_bytes = 16;
		break;
	default:
		fprintf(stderr, "RPMB probe command=%#x unsupported; no I/O\n", request[0]);
		return -EOPNOTSUPP;
	}
	if (size < request_bytes || size < response_bytes)
		return -EMSGSIZE;
	memcpy(request, buffer, request_bytes);
	response[0] = request[0];
	if (request[0] == 0x101) {
		response[1] = request[1];
		response[2] = (uint32_t)-EPERM;
		fprintf(stderr, "RPMB probe command=%#x version=%#x denied; no I/O\n",
			request[0], request[1]);
	} else if (request[0] == 0x104) {
		response[1] = (uint32_t)-EPERM;
		response[3] = response_bytes;
		fprintf(stderr, "RPMB probe command=%#x version=%#x device=%u denied; no I/O\n",
			request[0], request[1], request[2]);
	} else {
		response[1] = (uint32_t)-EPERM;
		response[3] = response_bytes;
		response[4] = request[4];
		fprintf(stderr, "RPMB probe command=%#x version=%#x sectors=%u bytes=%u "
			"offset=%u batch=%u denied; no I/O\n", request[0], request[4],
			request[1], request[2], request[3], request[5]);
		if (request[3] >= request_bytes && request[3] <= size &&
		    request[2] >= 512 && request[2] <= size - request[3]) {
			const uint8_t *frame = (uint8_t *)buffer + request[3];
			/* These three public structural fields are the only frame
			 * bytes inspected. Data, MAC, nonce and counter remain opaque.
			 */
			fprintf(stderr, "RPMB probe frame type=%#x address=%u block_count=%u\n",
				(unsigned int)frame[510] << 8 | frame[511],
				(unsigned int)frame[504] << 8 | frame[505],
				(unsigned int)frame[506] << 8 | frame[507]);
		}
	}
	memcpy(buffer, response, response_bytes);
	return 0;
}

static void storage_close_files(struct goodix_storage *storage)
{
	for (size_t i = 0; i < FOPEN_MAX; i++) {
		if (!storage->files[i].handle)
			continue;
		close(storage->files[i].fd);
		storage->files[i].fd = -1;
		storage->files[i].handle = 0;
	}
	if (storage->directory >= 0) {
		close(storage->directory);
		storage->directory = -1;
	}
}

static int32_t storage_invoke(ObjectCxt context, ObjectOp operation,
			      ObjectArg *args, ObjectCounts counts)
{
	struct storage_listener *listener = (struct storage_listener *)context;
	struct goodix_storage *storage = listener->storage;
	operation = ObjectOp_methodID(operation);
	if (operation == Object_OP_retain) {
		atomic_fetch_add(&storage->references, 1);
		return Object_OK;
	}
	if (operation == Object_OP_release) {
		/* Keep the predicate and notification under the same mutex: close
		 * must not free storage before the releasing worker finishes using it.
		 */
		pthread_mutex_lock(&storage->mutex);
		unsigned int remaining = atomic_fetch_sub(&storage->references, 1) - 1;
		if (remaining == 1)
			pthread_cond_signal(&storage->released);
		pthread_mutex_unlock(&storage->mutex);
		if (!remaining) {
			for (size_t i = 0; i < LISTENER_COUNT; i++) {
				if (storage->listeners[i].shared)
					explicit_bzero(storage->listeners[i].shared, storage->listeners[i].size);
				Object_RELEASE_IF(storage->listeners[i].memory);
				if (storage->listeners[i].work)
					explicit_bzero(storage->listeners[i].work, storage->listeners[i].size);
				free(storage->listeners[i].work);
			}
			storage_close_files(storage);
			goodix_rpmb_close(storage->rpmb);
			pthread_cond_destroy(&storage->released);
			pthread_mutex_destroy(&storage->mutex);
			free(storage);
		}
		return Object_OK;
	}
	if (operation == 1 && counts == ObjectCounts_pack(0, 0, 0, 0)) {
		pthread_mutex_lock(&storage->mutex);
		pthread_mutex_unlock(&storage->mutex);
		return Object_OK;
	}
	/* IListenerCBO.request(out offsets, out is64, out smo1..4). */
	if (operation || counts != ObjectCounts_pack(0, 2, 0, 4) ||
	    args[1].b.size != sizeof(uint32_t))
		return Object_ERROR_INVALID;
	args[0].b.size = 0;
	memset(args[1].b.ptr, 0, sizeof(uint32_t));
	for (size_t i = 2; i < 6; i++)
		args[i].o = Object_NULL;
	if (pthread_mutex_trylock(&storage->mutex))
		return Object_ERROR_BUSY;
	int result = -ECANCELED;
	if (!storage->stopped) {
		if (listener->service == RPMB_SERVICE_ID) {
			storage->rpmb_requests++;
			if (storage->rpmb && storage->allow_rpmb) {
				/* Secure world can change shared memory independently.
				 * Bind frame validation and SG_IO to this one HLOS copy.
				 */
				memcpy(listener->work, listener->shared, listener->size);
				result = goodix_rpmb_dispatch(storage->rpmb, listener->work, listener->size);
				memcpy(listener->shared, listener->work, listener->size);
				explicit_bzero(listener->work, listener->size);
			} else {
				result = storage_rpmb_deny_dispatch(listener->shared, listener->size);
			}
		} else {
			memcpy(listener->work, listener->shared, listener->size);
			result = listener->service == FILE_SERVICE_ID ?
				storage_fs_dispatch(storage, listener->work, listener->size) :
				storage_dispatch(storage, listener->work, listener->size);
			memcpy(listener->shared, listener->work, listener->size);
		}
	}
	pthread_mutex_unlock(&storage->mutex);
	return result ? Object_ERROR : Object_OK;
}

static int storage_directory(const char *directory)
{
	char *path, *component, *next = NULL;
	int fd, result, first = 1;

	if (!directory || directory[0] != '/')
		return -EACCES;
	path = strdup(directory);
	if (!path)
		return -ENOMEM;
	fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		free(path);
		return -errno;
	}
	for (component = strtok_r(path, "/", &next); component;
	     component = strtok_r(NULL, "/", &next)) {
		if (!strcmp(component, ".") || !strcmp(component, "..") ||
		    (first && (!strcmp(component, "data") || !strcmp(component, "persist")))) {
			result = -EACCES;
			goto failed;
		}
		first = 0;
		int child = openat(fd, component,
				   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (child < 0) {
			result = -errno;
			goto failed;
		}
		close(fd);
		fd = child;
	}
	free(path);
	return fd;
failed:
	close(fd);
	free(path);
	return result;
}

/* A directory lock covers the complete listener lifetime, including backups.
 * A profile may reopen its files; the original diagnostic requires a fresh one.
 */
static int storage_claim_directory(int fd, int profile)
{
	struct stat st;
	DIR *entries;
	int error = 0, scan;

	if (fstat(fd, &st))
		return -errno;
	if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700)
		return -EACCES;
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		int error = errno == EWOULDBLOCK ? EBUSY : errno;
		fprintf(stderr, "Fingerprint storage unavailable: stage=fpdata-lock error=%d\n", error);
		return -error;
	}
	if (profile)
		return 0;
	scan = dup(fd);
	if (scan < 0)
		return -errno;
	entries = fdopendir(scan);
	if (!entries) {
		error = errno;
		close(scan);
		return -error;
	}
	errno = 0;
	for (struct dirent *entry; (entry = readdir(entries));) {
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
			error = ENOTEMPTY;
			break;
		}
	}
	if (!error && errno)
		error = errno;
	closedir(entries);
	return -error;
}

static int storage_open(const char *directory, enum storage_mode mode,
			struct goodix_storage **output)
{
	struct goodix_storage *storage = NULL;
	Object root = Object_NULL, environment = Object_NULL;
	int fd = -1, result = -EINVAL;
	uint32_t uid = 87;
	int profile = mode == STORAGE_PROFILE || mode == STORAGE_ENROLL;
	int only_rpmb = mode == STORAGE_RPMB_DENY || mode == STORAGE_RPMB_UFS;
	int rpmb_backend = mode == STORAGE_ENROLL || mode == STORAGE_RPMB_UFS;

	*output = NULL;
	if (!only_rpmb) {
		fd = storage_directory(directory);
		if (fd < 0)
			return fd;
		result = storage_claim_directory(fd, profile);
		if (result)
			goto done;
	}
	storage = calloc(1, sizeof(*storage));
	if (!storage) {
		result = -ENOMEM;
		goto done;
	}
	atomic_init(&storage->references, 1);
	int error = pthread_mutex_init(&storage->mutex, NULL);
	if (!error) {
		error = pthread_cond_init(&storage->released, NULL);
		if (error)
			pthread_mutex_destroy(&storage->mutex);
	}
	if (error) {
		free(storage);
		storage = NULL;
		result = -error;
		goto done;
	}
	storage->directory = fd;
	storage->profile = profile;
	fd = -1;
	for (size_t i = 0; i < LISTENER_COUNT; i++)
		storage->listeners[i].storage = storage;
	if (rpmb_backend) {
		result = goodix_rpmb_open(&storage->rpmb);
		if (result)
			goto done;
	}
	if (MinkCom_getRootEnvObject(&root) ||
	    MinkCom_getClientEnvObject(root, &environment)) {
		result = -EIO;
		goto done;
	}
	/* Enrollment owns all three listeners under one lock and lifetime. Other
	 * profile operations only register FS/GPFS; the isolated diagnostic only
	 * registers RPMB. A conflicting existing listener is never replaced.
	 */
	for (size_t i = only_rpmb ? 2 : 0;
	     i < (only_rpmb || rpmb_backend ? 3U : 2U); i++) {
		struct storage_listener *listener = &storage->listeners[i];
		size_t bytes = i == 2 ? RPMB_BUFFER_BYTES :
			(i ? FILE_SERVICE_BUF_LEN : GPFILE_SERVICE_BUF_LEN);
		listener->service = i == 2 ? RPMB_SERVICE_ID :
			(i ? FILE_SERVICE_ID : GPFILE_SERVICE_ID);
		Object callback = {storage_invoke, (ObjectCxt)listener};
		ObjectArg open[] = {{.bi = {&uid, sizeof(uid)}}, {.o = Object_NULL}};
		if (Object_invoke(environment, 0, open, ObjectCounts_pack(1, 0, 0, 1))) {
			result = -EIO;
			goto done;
		}
		listener->registration = open[1].o;
		if (MinkCom_getMemoryObject(root, bytes, &listener->memory) ||
		    MinkCom_getMemoryObjectInfo(listener->memory, &listener->shared,
					&listener->size) || listener->size < bytes) {
			result = -ENOMEM;
			goto done;
		}
		if (i != 2 || storage->rpmb) {
			listener->work = calloc(1, listener->size);
			if (!listener->work) {
				result = -ENOMEM;
				goto done;
			}
		}
		ObjectArg register_args[] = {
			{.bi = {&listener->service, sizeof(listener->service)}},
			{.o = callback}, {.o = listener->memory},
		};
		int status = Object_invoke(listener->registration, 0, register_args,
					   ObjectCounts_pack(1, 0, 2, 0));
		if (status) {
			fprintf(stderr, "Storage service %u registration failed: %d\n",
				listener->service, status);
			result = -EBUSY;
			goto done;
		}
	}
	*output = storage;
	storage = NULL;
	result = 0;
done:
	if (fd >= 0)
		close(fd);
	Object_RELEASE_IF(environment);
	Object_RELEASE_IF(root);
	goodix_storage_close(storage);
	return result;
}

int goodix_storage_open(const char *directory, struct goodix_storage **output)
{
	return storage_open(directory, STORAGE_EMPTY, output);
}

int goodix_storage_open_profile(const char *directory, int gatekeeper,
			       struct goodix_storage **output)
{
	return storage_open(directory, gatekeeper ? STORAGE_ENROLL : STORAGE_PROFILE, output);
}

int goodix_storage_open_rpmb_probe(struct goodix_storage **output)
{
	return storage_open(NULL, STORAGE_RPMB_DENY, output);
}

int goodix_storage_open_rpmb_ufs(struct goodix_storage **output)
{
	return storage_open(NULL, STORAGE_RPMB_UFS, output);
}

void goodix_storage_allow_rpmb(struct goodix_storage *storage, int allow)
{
	pthread_mutex_lock(&storage->mutex);
	storage->allow_rpmb = !!allow;
	pthread_mutex_unlock(&storage->mutex);
}

unsigned int goodix_storage_rpmb_probe_requests(struct goodix_storage *storage)
{
	pthread_mutex_lock(&storage->mutex);
	unsigned int requests = storage->rpmb_requests;
	pthread_mutex_unlock(&storage->mutex);
	return requests;
}

void goodix_storage_close(struct goodix_storage *storage)
{
	if (!storage)
		return;
	pthread_mutex_lock(&storage->mutex);
	storage->stopped = 1;
	storage->allow_rpmb = 0;
	/* Every FS/GPFS/RPMB request holds this mutex. Drain the current request
	 * and return the directory lock before the caller unlocks the sensor.
	 * Late CBO references keep shared buffers alive, but stopped requests
	 * cannot use the closed files or start another protected-storage call.
	 */
	storage_close_files(storage);
	pthread_mutex_unlock(&storage->mutex);
	for (size_t i = 0; i < LISTENER_COUNT; i++)
		Object_RELEASE_IF(storage->listeners[i].registration);

	/* RegisterListenerCBO has no unregister operation. Its remote callback
	 * release arrives on the still-running Mink workers after deregistration.
	 * Each memory object retains that root until final destruction below.
	 */
	pthread_mutex_lock(&storage->mutex);
	while (atomic_load(&storage->references) != 1)
		pthread_cond_wait(&storage->released, &storage->mutex);
	pthread_mutex_unlock(&storage->mutex);
	Object callback = {storage_invoke, (ObjectCxt)&storage->listeners[0]};
	Object_release(callback);
}
