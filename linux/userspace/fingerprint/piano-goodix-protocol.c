// SPDX-License-Identifier: BSD-3-Clause
/* Offsets are ELF virtual addresses in OS4.0.4.0.XPYCNXM /odm/lib64/libgf_hal.so
 * (SHA256 36a176de...) and the `gfpia` TA in
 * vendor/firmware/hyperos4-fingerprint/manifest.json
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "piano-goodix-protocol.h"

uint32_t goodix_get32(const uint8_t *buffer, size_t offset)
{
	uint32_t value;

	memcpy(&value, buffer + offset, sizeof(value));
	return value;
}

void goodix_put32(uint8_t *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

/* gf_hal_common_authenticate 0x21a18. The operation ID is the token
 * challenge; Linux does not use the token, so it stays zero
 */
void goodix_authenticate_request(uint8_t *request, uint32_t group)
{
	memset(request, 0, GOODIX_AUTHENTICATE_BYTES);
	goodix_put32(request, 0x6c, group);
	goodix_put32(request, 0x78, 0xffffffffU);
}

/* hal_milan_irq 0x41390; 0x2c5 is the token's big-endian authenticator type */
void goodix_irq_request(uint8_t *request)
{
	static const uint8_t fingerprint_type[4] = {0, 0, 0, 2};

	memset(request, 0, GOODIX_IRQ_BYTES);
	goodix_put32(request, 0x98, 1);
	goodix_put32(request, 0xa8, 0x200);
	memcpy(request + 0x2c5, fingerprint_type, sizeof(fingerprint_type));
}

/* irq_process 0x41bc8 writes the step as a 64-bit value */
void goodix_irq_capture_again(uint8_t *irq)
{
	goodix_put32(irq, 0x98, GOODIX_STEP_CAPTURE);
	goodix_put32(irq, 0x9c, 0);
}

/* hal_milan_authenticate_finish 0x428d8: operation and IRQ type are swapped */
void goodix_authenticate_finish_request(uint8_t *request, const uint8_t *irq)
{
	memset(request, 0, GOODIX_AUTHENTICATE_FINISH_BYTES);
	goodix_put32(request, 0x70, goodix_get32(irq, 0x70));
	goodix_put32(request, 0x74, goodix_get32(irq, 0x6c));
	goodix_put32(request, 0x78, goodix_get32(irq, 0x80));
	goodix_put32(request, 0x7c, goodix_get32(irq, 0x84));
}

/* gf_hal_common_set_active_group 0x2208c. The TA only copies the path
 * (gf_ta_set_active_group, gfpia 0x47810)
 */
int goodix_set_active_group_request(uint8_t *request, uint32_t group,
				    const char *path)
{
	size_t length = strlen(path);

	if (length >= GOODIX_GROUP_PATH_BYTES)
		return -ENAMETOOLONG;
	memset(request, 0, GOODIX_SET_ACTIVE_GROUP_BYTES);
	goodix_put32(request, 0x6c, group);
	memcpy(request + 0x70, path, length);
	return 0;
}

/* irq_image_enroll_or_authenticate 0x42308 */
enum goodix_result goodix_authenticate_result(uint32_t operation, int status)
{
	switch (status) {
	case 0:
		return operation == GOODIX_OPERATION_AUTHENTICATE_IMAGE ||
		       operation == GOODIX_OPERATION_AUTHENTICATE_FF ?
		       GOODIX_RESULT_MATCH : GOODIX_RESULT_NONE;
	case GOODIX_NOT_MATCH:
	case GOODIX_ALGO_FAKE:
		return GOODIX_RESULT_NO_MATCH;
	case GOODIX_MATCH_FAIL_AND_RETRY:
		return GOODIX_RESULT_RETRY;
	case GOODIX_ACQUIRED_PARTIAL:
	case GOODIX_SENSOR_BROKEN_CHECK_ALGO_ERROR:
		return GOODIX_RESULT_PARTIAL;
	case GOODIX_ACQUIRED_IMAGER_DIRTY:
	case GOODIX_ALGO_DIRTY_FINGER:
	case GOODIX_ALGO_COVER_BROKEN:
		return GOODIX_RESULT_DIRTY;
	case GOODIX_TOO_FAST:
		return GOODIX_RESULT_TOO_FAST;
	case GOODIX_TOO_SLOW:
		return GOODIX_RESULT_TOO_SLOW;
	case GOODIX_ALGO_PALM_DETECT:
		return GOODIX_RESULT_PALM;
	default:
		return GOODIX_RESULT_ERROR;
	}
}

const char *goodix_error_name(int status)
{
	static const struct { int code; const char *name; } names[] = {
		{ GOODIX_NOT_MATCH, "GF_ERROR_NOT_MATCH" },
		{ 1007, "GF_ERROR_CANCELED" },
		{ 1008, "GF_ERROR_TIMEOUT" },
		{ GOODIX_ACQUIRED_PARTIAL, "GF_ERROR_ACQUIRED_PARTIAL" },
		{ GOODIX_ACQUIRED_IMAGER_DIRTY, "GF_ERROR_ACQUIRED_IMAGER_DIRTY" },
		{ 1033, "GF_ERROR_OPEN_SECURE_OBJECT_FAILED" },
		{ 1034, "GF_ERROR_READ_SECURE_OBJECT_FAILED" },
		{ 1035, "GF_ERROR_WRITE_SECURE_OBJECT_FAILED" },
		{ 1037, "GF_ERROR_SECURE_OBJECT_NOT_EXIST" },
		{ 1047, "GF_ERROR_FINGER_NOT_EXIST" },
		{ GOODIX_TA_DEAD, "GF_ERROR_TA_DEAD" },
		{ GOODIX_INVALID_HAT_VERSION, "GF_ERROR_INVALID_HAT_VERSION" },
		{ GOODIX_INVALID_CHALLENGE, "GF_ERROR_INVALID_CHALLENGE" },
		{ GOODIX_UNTRUSTED_ENROLL, "GF_ERROR_UNTRUSTED_ENROLL" },
		{ GOODIX_MATCH_FAIL_AND_RETRY, "GF_ERROR_MATCH_FAIL_AND_RETRY" },
		{ 1065, "GF_ERROR_FINGER_NOT_LEAVE" },
		{ 1088, "GF_ERROR_NO_GROUP_ID" },
		{ GOODIX_TOO_FAST, "GF_ERROR_TOO_FAST" },
		{ GOODIX_ALGO_DIRTY_FINGER, "GF_ERROR_ALGO_DIRTY_FINGER" },
		{ GOODIX_ALGO_PALM_DETECT, "GF_ERROR_ALGO_PALM_DETECT" },
		{ GOODIX_TOO_SLOW, "GF_ERROR_TOO_SLOW" },
		{ 1118, "GF_ERROR_OVER_CURRENT" },
		{ GOODIX_ALGO_FAKE, "GF_ERROR_ALGO_FAKE" },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (names[i].code == status)
			return names[i].name;
	return "GF_ERROR";
}

/* The TA names every object "/data/vendor_de/0/fpdata/<name>"
 * (gf_tee_open_object, gfpia 0x50450). Field layouts follow minkipc
 * listeners/libfsservice/gpfs/gpfs_msg.h. Only a bare name under that
 * prefix is served, so Android storage is never touched
 */
enum {
	FILE_READ = 0,
	FILE_WRITE = 1,
	FILE_REMOVE = 2,
	FILE_RENAME = 3,
	FILE_VERSION = 12,
	FILE_PATH = 4,
	FILE_PATH_BYTES = 256,
	FILE_DATA_LIMIT = 500 * 1024,
	FILE_READ_OFFSET = FILE_PATH + FILE_PATH_BYTES,
	FILE_READ_COUNT = FILE_READ_OFFSET + 4,
	FILE_READ_DATA = 12,
	FILE_WRITE_BACKUP = FILE_READ_COUNT + 4,
	FILE_WRITE_DATA = FILE_WRITE_BACKUP + 4,
	FILE_RENAME_TO = FILE_PATH + FILE_PATH_BYTES,
	FILE_OBJECT_NAME_LIMIT = 96,
	FILE_SERVICE_VERSION = 2,
	FILE_NO_COMMAND = -1,
};

int goodix_file_object_name(const char *path, size_t capacity, const char **name)
{
	size_t prefix = strlen(GOODIX_TA_OBJECT_PREFIX);
	size_t length = strnlen(path, capacity);

	if (length == capacity || length <= prefix ||
	    strncmp(path, GOODIX_TA_OBJECT_PREFIX, prefix) ||
	    length - prefix > FILE_OBJECT_NAME_LIMIT || path[prefix] == '.')
		return -EACCES;
	for (size_t i = prefix; i < length; i++) {
		char c = path[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return -EACCES;
	}
	*name = path + prefix;
	return 0;
}

static int file_open(int directory, const char *name, int flags)
{
	int fd = openat(directory, name, flags | O_NOFOLLOW | O_CLOEXEC, 0600);
	struct stat st;

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		return -EINVAL;
	}
	return fd;
}

static int file_read(int directory, const char *name, int32_t offset,
		     uint8_t *data, size_t count, uint32_t *done)
{
	int fd;
	size_t total = 0;

	*done = 0;
	if (offset < 0)
		return EINVAL;
	fd = file_open(directory, name, O_RDONLY);
	if (fd < 0)
		return -fd;
	while (total < count) {
		ssize_t bytes = pread(fd, data + total, count - total, offset + total);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes < 0) {
			int error = errno;

			close(fd);
			return error;
		}
		if (!bytes)
			break;
		total += bytes;
	}
	close(fd);
	*done = total;
	return 0;
}

static int file_copy(int source, int directory, const char *name)
{
	uint8_t chunk[4096];
	int target = file_open(directory, name, O_WRONLY | O_CREAT | O_TRUNC);
	off_t offset = 0;
	int error = 0;

	if (target < 0)
		return -target;
	for (;;) {
		ssize_t bytes = pread(source, chunk, sizeof(chunk), offset);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0) {
			error = bytes < 0 ? errno : 0;
			break;
		}
		for (ssize_t written = 0; written < bytes;) {
			ssize_t step = write(target, chunk + written, bytes - written);

			if (step < 0 && errno == EINTR)
				continue;
			if (step <= 0) {
				error = step < 0 ? errno : EIO;
				goto done;
			}
			written += step;
		}
		offset += bytes;
	}
done:
	if (!error && fsync(target))
		error = errno;
	close(target);
	return error;
}

static int file_write(int directory, const char *name, int32_t offset,
		      const uint8_t *data, size_t count, int backup,
		      uint32_t *done)
{
	int fd, error = 0;
	size_t total = 0;
	struct stat st;

	*done = 0;
	if (offset < 0)
		return EINVAL;
	fd = file_open(directory, name, O_RDWR | O_CREAT | O_SYNC);
	if (fd < 0)
		return -fd;
	/* Same backup rule as the stock listener */
	if (backup && !fstat(fd, &st) && st.st_size > 0) {
		char copy[FILE_OBJECT_NAME_LIMIT + 8];

		memcpy(copy, name, strlen(name));
		memcpy(copy + strlen(name), ".bak", 5);
		error = file_copy(fd, directory, copy);
	}
	while (!error && total < count) {
		ssize_t bytes = pwrite(fd, data + total, count - total, offset + total);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0) {
			error = bytes < 0 ? errno : EIO;
			break;
		}
		total += bytes;
	}
	close(fd);
	if (!error)
		*done = total;
	return error;
}

static int file_remove(int directory, const char *name)
{
	struct stat st;

	if (fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW))
		return errno;
	if (!S_ISREG(st.st_mode))
		return EINVAL;
	return unlinkat(directory, name, 0) ? errno : 0;
}

static void count_refusal(struct goodix_file_counts *counts, uint32_t command,
			  const uint8_t *path)
{
	if (!counts)
		return;
	counts->refused++;
	counts->refused_command = command;
	memcpy(counts->refused_path, path, FILE_PATH_BYTES);
	counts->refused_path[FILE_PATH_BYTES] = 0;
}

int goodix_file_service(int directory, uint8_t *buffer, size_t size,
			struct goodix_file_counts *counts)
{
	uint32_t command, count = 0, backup;
	int32_t offset, error;
	const char *name = NULL, *target = NULL;

	if (size < GOODIX_FILE_SERVICE_BYTES)
		return -EMSGSIZE;
	command = goodix_get32(buffer, 0);
	switch (command) {
	case FILE_VERSION:
		goodix_put32(buffer, 4, FILE_SERVICE_VERSION);
		goodix_put32(buffer, 8, 0);
		return 0;
	case FILE_READ:
	case FILE_WRITE:
	case FILE_REMOVE:
	case FILE_RENAME:
		break;
	default:
		/* The data/persist variants 4..11 are refused */
		count_refusal(counts, command, buffer + FILE_PATH);
		goodix_put32(buffer, 4, (uint32_t)(command >= 4 && command <= 11 ?
						    EACCES : FILE_NO_COMMAND));
		goodix_put32(buffer, 8, 0);
		return 0;
	}
	/* Request and response share the buffer: read every field first */
	error = -goodix_file_object_name((const char *)buffer + FILE_PATH,
					 FILE_PATH_BYTES, &name);
	if (error)
		count_refusal(counts, command, buffer + FILE_PATH);
	if (!error && command == FILE_RENAME) {
		error = -goodix_file_object_name((const char *)buffer + FILE_RENAME_TO,
						 FILE_PATH_BYTES, &target);
		if (error)
			count_refusal(counts, command, buffer + FILE_RENAME_TO);
	}
	memcpy(&offset, buffer + FILE_READ_OFFSET, sizeof(offset));
	count = goodix_get32(buffer, FILE_READ_COUNT);
	backup = goodix_get32(buffer, FILE_WRITE_BACKUP);
	if (count > FILE_DATA_LIMIT)
		count = FILE_DATA_LIMIT;
	if (!error) {
		char object[FILE_OBJECT_NAME_LIMIT + 1], renamed[FILE_OBJECT_NAME_LIMIT + 1];

		memcpy(object, name, strlen(name) + 1);
		if (target)
			memcpy(renamed, target, strlen(target) + 1);
		if (counts) {
			unsigned int *served[] = { &counts->reads, &counts->writes,
						   &counts->removes, &counts->renames };

			(*served[command])++;
		}
		switch (command) {
		case FILE_READ:
			error = file_read(directory, object, offset,
					  buffer + FILE_READ_DATA, count, &count);
			break;
		case FILE_WRITE:
			error = file_write(directory, object, offset,
					   buffer + FILE_WRITE_DATA, count, backup != 0,
					   &count);
			break;
		case FILE_REMOVE:
			error = file_remove(directory, object);
			count = 0;
			break;
		case FILE_RENAME:
			error = renameat(directory, object, directory, renamed) ? errno : 0;
			count = 0;
			break;
		}
	}
	if (error)
		count = 0;
	goodix_put32(buffer, 4, (uint32_t)error);
	if (command == FILE_READ || command == FILE_WRITE)
		goodix_put32(buffer, 8, count);
	return 0;
}
