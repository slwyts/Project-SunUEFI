// SPDX-License-Identifier: BSD-3-Clause
/* Run the real FS/GPFS callback against private temporary files. The linker
 * drops QTEE listener registration; no TEE, UIO or Android path is opened.
 * The RPMB test injects a shared-memory change at allocation and records the
 * real backend's ioctl arguments instead of accessing a hardware device.
 */
#include "../../linux/userspace/fingerprint/piano-goodix-storage.c"
#include "../../linux/userspace/fingerprint/piano-goodix-rpmb.c"

#include <assert.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/resource.h>

#define PREFIX "/data/vendor_de/0/fpdata/"
#define BUFFER_BYTES GPFILE_SERVICE_BUF_LEN

static uint8_t buffer[BUFFER_BYTES], work[BUFFER_BYTES];
static int mutate_rpmb_shared, rpmb_ioctls;
static int rpmb_failure_stage = -1, rpmb_short_transfer;
static atomic_int pause_fsync;
static sem_t callback_in_fsync, callback_continue, close_started, close_finished;
static sem_t registrations_released;

int __real_fsync(int fd);

int __wrap_fsync(int fd)
{
	if (atomic_exchange(&pause_fsync, 0)) {
		assert(!sem_post(&callback_in_fsync));
		int result;
		do {
			result = sem_wait(&callback_continue);
		} while (result && errno == EINTR);
		assert(!result);
	}
	return __real_fsync(fd);
}

static void private_frame_fields(uint8_t *frame)
{
	const char marker[] = "PIANO_PRIVATE_FRAME_MATERIAL";
	uint32_t counter = htobe32(987654321);

	memcpy(frame + 196, marker, sizeof(marker)); /* MAC */
	memcpy(frame + 228, marker, sizeof(marker)); /* data */
	memcpy(frame + 484, marker, 16); /* nonce */
	memcpy(frame + FRAME_COUNTER, &counter, sizeof(counter));
}

void *__real_malloc(size_t size);

void *__wrap_malloc(size_t size)
{
	void *allocation = __real_malloc(size);
	if (mutate_rpmb_shared && size == 2 * FRAME_BYTES) {
		/* dispatch validated before allocating its input copy. Change
		 * untrusted shared memory exactly between those two operations.
		 */
		buffer[24 + FRAME_TYPE + 1] = 1;
		mutate_rpmb_shared = 0;
	}
	return allocation;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	va_start(arguments, request);
	struct sg_io_v4 *io = va_arg(arguments, struct sg_io_v4 *);
	va_end(arguments);
	assert(fd == 123 && request == SG_IO);
	const uint8_t *cdb = (const uint8_t *)(uintptr_t)io->request;
	assert(io->request_len == 12 && cdb[1] == 0xec && cdb[2] == 0 && cdb[3] == 1);
	if (rpmb_ioctls == 0) {
		assert(cdb[0] == 0xb5 && io->dout_xfer_len == 2 * FRAME_BYTES);
		const uint8_t *frames = (const uint8_t *)(uintptr_t)io->dout_xferp;
		/* The attempted PROGRAM_KEY substitution must never reach SG_IO. */
		assert(frame_word(frames, FRAME_TYPE) == AUTH_WRITE);
		assert(frame_word(frames + FRAME_BYTES, FRAME_TYPE) == AUTH_WRITE);
	} else if (rpmb_ioctls == 1) {
		assert(cdb[0] == 0xb5 && io->dout_xfer_len == FRAME_BYTES);
		const uint8_t *frame = (const uint8_t *)(uintptr_t)io->dout_xferp;
		assert(frame_word(frame, FRAME_TYPE) == RESULT_READ);
	} else {
		assert(rpmb_ioctls == 2 && cdb[0] == 0xa2 && io->din_xfer_len == FRAME_BYTES);
		uint8_t *frame = (uint8_t *)(uintptr_t)io->din_xferp;
		memset(frame, 0, FRAME_BYTES);
		private_frame_fields(frame);
		frame[FRAME_TYPE] = AUTH_WRITE;
	}
	int stage = rpmb_ioctls;
	rpmb_ioctls++;
	if (stage == rpmb_failure_stage) {
		if (rpmb_short_transfer) {
			if (io->dout_xfer_len)
				io->dout_resid = 1;
			else
				io->din_resid = 1;
			return 0;
		}
		errno = EIO;
		return -1;
	}
	return 0;
}

static void path(char output[TZ_CM_MAX_NAME_LEN], const char *name)
{
	assert(snprintf(output, TZ_CM_MAX_NAME_LEN, PREFIX "%s", name) < TZ_CM_MAX_NAME_LEN);
}

static int dispatch(struct goodix_storage *storage)
{
	uint32_t wide = UINT32_MAX;
	struct storage_listener listener = {
		.storage = storage, .service = GPFILE_SERVICE_ID,
		.shared = buffer, .work = work, .size = sizeof(buffer),
	};
	ObjectArg args[6] = { [1].b = { &wide, sizeof(wide) } };

	assert(storage_invoke(&listener, 0, args, ObjectCounts_pack(0, 2, 0, 4)) == Object_OK);
	assert(wide == 0 && args[0].b.size == 0);
	return ((tz_gpfile_read_rsp_t *)buffer)->err;
}

static int write_range(struct goodix_storage *storage, const char *name,
		       const void *contents, uint32_t count, int32_t offset, uint32_t backup)
{
	tz_gpfile_write_req_t *request = (void *)buffer;

	memset(buffer, 0, sizeof(buffer));
	request->cmd_id = (tz_gpfs_msg_cmd_type)GPFS_WRITE;
	path(request->pathname, name);
	request->count = count;
	request->offset = offset;
	request->backup = backup;
	if (count <= sizeof(buffer) - offsetof(tz_gpfile_write_req_t, buf))
		memcpy(buffer + offsetof(tz_gpfile_write_req_t, buf), contents, count);
	int error = dispatch(storage);

	assert(((tz_gpfile_write_rsp_t *)buffer)->num_bytes_written == (error ? 0 : count));
	return error;
}

static int read_range(struct goodix_storage *storage, const char *name,
		      uint32_t count, int32_t offset)
{
	tz_gpfile_read_req_t *request = (void *)buffer;

	memset(buffer, 0, sizeof(buffer));
	request->cmd_id = (tz_gpfs_msg_cmd_type)GPFS_READ;
	path(request->pathname, name);
	request->count = count;
	request->offset = offset;
	return dispatch(storage);
}

static int change_name(struct goodix_storage *storage, const char *from, const char *to)
{
	tz_gpfile_rename_req_t *request = (void *)buffer;

	memset(buffer, 0, sizeof(buffer));
	request->cmd_id = (tz_gpfs_msg_cmd_type)(to ? GPFS_RENAME : GPFS_REMOVE);
	path(request->from, from);
	if (to)
		path(request->to, to);
	return dispatch(storage);
}

static void expect_file(int directory, const char *name, const void *contents, size_t count)
{
	struct stat st;
	uint8_t data[32];
	int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	assert(fd >= 0 && count <= sizeof(data));
	assert(fstat(fd, &st) == 0 && st.st_size == (off_t)count);
	assert((st.st_mode & 0777) == 0600 && st.st_nlink == 1);
	assert(read(fd, data, sizeof(data)) == (ssize_t)count);
	assert(!memcmp(data, contents, count));
	close(fd);
}

static void writes(struct goodix_storage *storage, const char *directory)
{
	assert(write_range(storage, "template", "abcdef", 6, 0, 0) == 0);
	int old = openat(storage->directory, "template", O_RDONLY);
	assert(old >= 0);
	assert(write_range(storage, "template", "XY", 2, 2, 1) == 0);
	expect_file(storage->directory, "template", "abXYef", 6);
	expect_file(storage->directory, "template.bak", "abcdef", 6);
	char previous[6];
	assert(read(old, previous, sizeof(previous)) == 6 && !memcmp(previous, "abcdef", 6));
	close(old);
	assert(write_range(storage, "template", "Z", 1, 8, 1) == 0);
	expect_file(storage->directory, "template", "abXYef\0\0Z", 9);
	expect_file(storage->directory, "template.bak", "abXYef", 6);
	assert(write_range(storage, "template", "", 0, 500000, 0) == 0);
	expect_file(storage->directory, "template", "abXYef\0\0Z", 9);
	assert(read_range(storage, "template", 32, 0) == 0);
	tz_gpfile_read_rsp_t *reply = (void *)buffer;
	assert(reply->num_bytes_read == 9 && !memcmp(reply->buf, "abXYef\0\0Z", 9));
	assert(write_range(storage, "new", "new", 3, 0, 1) == 0);
	expect_file(storage->directory, "new.bak", "", 0);

	close(storage->directory);
	storage->directory = storage_directory(directory);
	assert(storage->directory >= 0 && storage_claim_directory(storage->directory, 1) == 0);
	expect_file(storage->directory, "template", "abXYef\0\0Z", 9);
}

static void names(struct goodix_storage *storage)
{
	assert(write_range(storage, "first", "first", 5, 0, 0) == 0);
	assert(write_range(storage, "second", "second", 6, 0, 0) == 0);
	assert(change_name(storage, "first", "second") == 0);
	expect_file(storage->directory, "second", "first", 5);
	assert(faccessat(storage->directory, "first", F_OK, 0) == -1 && errno == ENOENT);
	assert(change_name(storage, "second", NULL) == 0);
	assert(change_name(storage, "second", NULL) == ENOENT);
	assert(write_range(storage, "calibration", "cal", 3, 0, 0) == 0);
	storage->profile = 0;
	assert(write_range(storage, "calibration", "x", 1, 0, 1) == EOPNOTSUPP);
	assert(change_name(storage, "calibration", "other") == EROFS);
	assert(change_name(storage, "calibration", NULL) == EROFS);
	expect_file(storage->directory, "calibration", "cal", 3);
}

static void large_files(struct goodix_storage *storage, const char *directory)
{
	const uint32_t write_capacity = sizeof(buffer) - offsetof(tz_gpfile_write_req_t, buf);
	const uint32_t read_capacity = sizeof(buffer) - offsetof(tz_gpfile_read_rsp_t, buf);
	uint8_t *contents = malloc(write_capacity);
	assert(contents);
	for (uint32_t i = 0; i < write_capacity; i++)
		contents[i] = (uint8_t)(i * 31 + 7);

	/* The first larger-than-500-KiB write and the real full-message size
	 * are both valid. These are synthetic bytes, never a sensor template.
	 */
	assert(write_capacity == 515824);
	assert(write_range(storage, "large", contents, TZ_GPFS_FILE_SIZE + 1, 0, 0) == 0);
	assert(read_range(storage, "large", read_capacity, 0) == 0);
	assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == TZ_GPFS_FILE_SIZE + 1);
	assert(!memcmp(buffer + offsetof(tz_gpfile_read_rsp_t, buf), contents,
		       TZ_GPFS_FILE_SIZE + 1));
	assert(write_range(storage, "large", contents, write_capacity, 0, 1) == 0);
	assert(read_range(storage, "large.bak", read_capacity, 0) == 0);
	assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == TZ_GPFS_FILE_SIZE + 1);

	/* A second message appends beyond the entire listener buffer. Preserve
	 * the first message and commit its complete backup before the new range.
	 */
	assert(write_range(storage, "large", contents, 4096, write_capacity, 1) == 0);
	assert(read_range(storage, "large", write_capacity, 0) == 0);
	assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == write_capacity);
	assert(!memcmp(buffer + offsetof(tz_gpfile_read_rsp_t, buf), contents, write_capacity));
	assert(read_range(storage, "large", 8192, write_capacity) == 0);
	assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == 4096);
	assert(!memcmp(buffer + offsetof(tz_gpfile_read_rsp_t, buf), contents, 4096));
	assert(read_range(storage, "large.bak", read_capacity, 0) == 0);
	assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == write_capacity);
	assert(!memcmp(buffer + offsetof(tz_gpfile_read_rsp_t, buf), contents, write_capacity));
	assert(read_range(storage, "large", read_capacity, 0) == 0);
	assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == read_capacity);
	assert(!memcmp(buffer + offsetof(tz_gpfile_read_rsp_t, buf), contents, write_capacity));
	assert(!memcmp(buffer + offsetof(tz_gpfile_read_rsp_t, buf) + write_capacity,
		       contents, read_capacity - write_capacity));

	/* Both the old main file and the new backup now exceed one listener
	 * message. A small range update must retain the later message's data.
	 */
	assert(write_range(storage, "large", "Z", 1, TZ_GPFS_FILE_SIZE + 17, 1) == 0);
	assert(read_range(storage, "large", 1, TZ_GPFS_FILE_SIZE + 17) == 0);
	assert(buffer[offsetof(tz_gpfile_read_rsp_t, buf)] == 'Z');
	assert(read_range(storage, "large.bak", 8192, write_capacity) == 0);
	assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == 4096);
	assert(!memcmp(buffer + offsetof(tz_gpfile_read_rsp_t, buf), contents, 4096));

	/* Exceeding the actual shared-memory tail by one byte must still fail
	 * before any I/O. Keep both read and write message boundaries distinct.
	 */
	memset(buffer, 0, sizeof(buffer));
	tz_gpfile_write_req_t *write = (void *)buffer;
	write->cmd_id = (tz_gpfs_msg_cmd_type)GPFS_WRITE;
	path(write->pathname, "large");
	write->count = write_capacity + 1;
	assert(storage_dispatch(storage, buffer, sizeof(buffer)) == -EMSGSIZE);
	memset(buffer, 0, sizeof(buffer));
	tz_gpfile_read_req_t *read = (void *)buffer;
	read->cmd_id = (tz_gpfs_msg_cmd_type)GPFS_READ;
	path(read->pathname, "large");
	read->count = read_capacity + 1;
	assert(storage_dispatch(storage, buffer, sizeof(buffer)) == -EMSGSIZE);

	close(storage->directory);
	storage->directory = storage_directory(directory);
	assert(storage->directory >= 0 && storage_claim_directory(storage->directory, 1) == 0);
	int fd = storage_file(storage, "large", O_RDONLY);
	struct stat st;
	assert(fd >= 0 && !fstat(fd, &st) && st.st_size == (off_t)write_capacity + 4096);
	close(fd);
	assert(read_range(storage, "large", 8192, write_capacity) == 0);
	assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == 4096);
	assert(!memcmp(buffer + offsetof(tz_gpfile_read_rsp_t, buf), contents, 4096));

	/* Exercise the signed protocol offset edge with a sparse file. The
	 * transfer remains one byte and must not allocate the hole in HLOS.
	 */
	if (sizeof(off_t) > sizeof(int32_t)) {
		assert(write_range(storage, "offset-edge", "X", 1, INT32_MAX, 0) == 0);
		assert(read_range(storage, "offset-edge", 1, INT32_MAX) == 0);
		assert(((tz_gpfile_read_rsp_t *)buffer)->num_bytes_read == 1);
		assert(buffer[offsetof(tz_gpfile_read_rsp_t, buf)] == 'X');
	} else {
		assert(write_range(storage, "offset-edge", "X", 1, INT32_MAX, 0) == EINVAL);
	}
	free(contents);
}

static void boundaries(struct goodix_storage *storage)
{
	assert(write_range(storage, "valid", "safe", 4, 0, 0) == 0);
	assert(write_range(storage, "../outside", "x", 1, 0, 0) == EACCES);
	assert(write_range(storage, "nested/file", "x", 1, 0, 0) == EACCES);
	assert(write_range(storage, "valid", "x", 1, -1, 0) == EINVAL);
	assert(change_name(storage, "valid", "../outside") == EACCES);

	assert(symlinkat("valid", storage->directory, "symlink") == 0);
	assert(write_range(storage, "symlink", "x", 1, 0, 0) != 0);
	assert(change_name(storage, "symlink", NULL) != 0);
	assert(change_name(storage, "valid", "symlink") != 0);
	assert(linkat(storage->directory, "valid", storage->directory, "hardlink", 0) == 0);
	assert(write_range(storage, "valid", "x", 1, 0, 0) == EACCES);
	assert(change_name(storage, "hardlink", NULL) == EACCES);
	assert(unlinkat(storage->directory, "hardlink", 0) == 0);
	assert(symlinkat("valid", storage->directory, "valid.bak") == 0);
	assert(write_range(storage, "valid", "x", 1, 0, 1) != 0);
	expect_file(storage->directory, "valid", "safe", 4);
	assert(mkdirat(storage->directory, "directory", 0700) == 0);
	assert(read_range(storage, "directory", 1, 0) == EACCES);
	assert(mkfifoat(storage->directory, "fifo", 0600) == 0);
	assert(read_range(storage, "fifo", 1, 0) == EACCES);
	memset(buffer, 0, sizeof(buffer));
	((tz_gpfile_write_req_t *)buffer)->cmd_id = (tz_gpfs_msg_cmd_type)GPFS_WRITE;
	path(((tz_gpfile_write_req_t *)buffer)->pathname, "valid");
	((tz_gpfile_write_req_t *)buffer)->count = 1;
	assert(storage_dispatch(storage, buffer, offsetof(tz_gpfile_write_req_t, buf)) == -EMSGSIZE);
	assert(storage_dispatch(storage, buffer, 3) == -EMSGSIZE);
}

static void fs_handles(struct goodix_storage *storage, const char *directory)
{
	assert(write_range(storage, "file", "data", 4, 0, 0) == 0);
	tz_file_open_req_t *open_request = (void *)buffer;
	memset(buffer, 0, sizeof(buffer));
	open_request->cmd_id = TZ_FS_MSG_CMD_FILE_OPEN;
	path(open_request->pathname, "file");
	assert(storage_fs_dispatch(storage, buffer, sizeof(buffer)) == 0);
	int handle = ((tz_file_open_rsp_t *)buffer)->ret;
	assert(handle > 0);
	int fd = storage->files[0].fd;
	assert(fd != handle);
	tz_file_close_req_t close_request = { TZ_FS_MSG_CMD_FILE_CLOSE, fd };
	memcpy(buffer, &close_request, sizeof(close_request));
	assert(storage_fs_dispatch(storage, buffer, sizeof(buffer)) == 0);
	assert(((tz_file_close_rsp_t *)buffer)->ret == -1 && fcntl(fd, F_GETFD) >= 0);
	uint32_t get_errno = TZ_FS_MSG_CMD_FILE_GET_ERRNO;
	memcpy(buffer, &get_errno, sizeof(get_errno));
	assert(storage_fs_dispatch(storage, buffer, sizeof(buffer)) == 0);
	assert(((tz_file_get_errno_rsp_t *)buffer)->ret == EBADF);
	close_request.fd = handle;
	memcpy(buffer, &close_request, sizeof(close_request));
	assert(storage_fs_dispatch(storage, buffer, sizeof(buffer)) == 0);
	assert(((tz_file_close_rsp_t *)buffer)->ret == 0 && fcntl(fd, F_GETFD) == -1);

	int second = storage_directory(directory);
	assert(second >= 0 && storage_claim_directory(second, 1) == -EBUSY);
	close(second);
	close(storage->directory);
	storage->directory = storage_directory(directory);
	assert(storage_claim_directory(storage->directory, 0) == -ENOTEMPTY);
	close(storage->directory);
	storage->directory = storage_directory(directory);
	assert(storage_claim_directory(storage->directory, 1) == 0);
	assert(storage_directory("/data/vendor_de/0/fpdata") == -EACCES);
	assert(storage_directory("/persist") == -EACCES);
}

static void write_failures(struct goodix_storage *storage)
{
	struct rlimit original, limited;

	assert(write_range(storage, "template", "abcdef", 6, 0, 0) == 0);
	assert(write_range(storage, "template.bak", "previous", 8, 0, 0) == 0);
	assert(getrlimit(RLIMIT_FSIZE, &original) == 0);
	limited = original;
	limited.rlim_cur = 3;
	assert(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
	assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
	/* A real filesystem write failure in backup staging leaves both old
	 * versions intact. No full disk or synthetic I/O implementation is used.
	 */
	assert(write_range(storage, "template", "x", 1, 0, 1) == EFBIG);
	expect_file(storage->directory, "template", "abcdef", 6);
	expect_file(storage->directory, "template.bak", "previous", 8);
	assert(setrlimit(RLIMIT_FSIZE, &original) == 0);
	assert(write_range(storage, "small", "a", 1, 0, 0) == 0);
	assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
	/* If backup succeeds but new-file staging fails, the old main file and
	 * its committed backup still contain the complete previous version.
	 */
	assert(write_range(storage, "small", "larger", 6, 0, 1) == EFBIG);
	expect_file(storage->directory, "small", "a", 1);
	expect_file(storage->directory, "small.bak", "a", 1);
	assert(setrlimit(RLIMIT_FSIZE, &original) == 0);
}

static void rpmb_denial(struct goodix_storage *storage)
{
	struct storage_listener listener = {
		.storage = storage, .service = RPMB_SERVICE_ID,
		.shared = buffer, .size = RPMB_BUFFER_BYTES,
	};
	uint32_t wide = UINT32_MAX;
	ObjectArg args[6] = { [1].b = { &wide, sizeof(wide) } };
	const uint32_t commands[] = {0x101, 0x102, 0x103, 0x104};

	/* No work buffer is supplied: RPMB dispatch must not copy any frames.
	 * Keep bytes beyond the largest response intact, including data payload.
	 */
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
		uint32_t request[] = {commands[i], 2, 1024, 40, 0x200, 1};
		memset(buffer, 0xa5, RPMB_BUFFER_BYTES);
		memcpy(buffer, request, sizeof(request));
		assert(storage_invoke(&listener, 0, args,
				      ObjectCounts_pack(0, 2, 0, 4)) == Object_OK);
		uint32_t response[5];
		memcpy(response, buffer, sizeof(response));
		assert(response[0] == commands[i]);
		if (commands[i] == 0x101) {
			assert(response[1] == 2 && (int32_t)response[2] == -EPERM);
			assert(response[3] == 0 && response[4] == 0);
		} else if (commands[i] == 0x104) {
			assert((int32_t)response[1] == -EPERM && response[2] == 0);
			assert(response[3] == 16 && response[4] == request[4]);
		} else {
			assert((int32_t)response[1] == -EPERM && response[2] == 0);
			assert(response[3] == 20 && response[4] == 0x200);
		}
		for (size_t j = 24; j < RPMB_BUFFER_BYTES; ++j)
			assert(buffer[j] == 0xa5);
		assert(wide == 0 && args[0].b.size == 0);
	}
	assert(goodix_storage_rpmb_probe_requests(storage) == 4);
	uint32_t unknown = 0x105;
	memcpy(buffer, &unknown, sizeof(unknown));
	assert(storage_invoke(&listener, 0, args,
			      ObjectCounts_pack(0, 2, 0, 4)) == Object_ERROR);
	assert(!memcmp(buffer, &unknown, sizeof(unknown)));
	assert(storage_rpmb_deny_dispatch(buffer, 3) == -EMSGSIZE);
	uint32_t init = 0x101;
	memcpy(buffer, &init, sizeof(init));
	assert(storage_rpmb_deny_dispatch(buffer, 8) == -EMSGSIZE);
	assert(!memcmp(buffer, &init, sizeof(init)));
	assert(storage_claim_directory(storage->directory, 0) == 0);
}

static void rpmb_write_request(void)
{
	uint32_t request[] = {0x103, 2, 2 * FRAME_BYTES, 24, 0, 2};

	memset(buffer, 0, RPMB_BUFFER_BYTES);
	memcpy(buffer, request, sizeof(request));
	for (size_t i = 0; i < 2; ++i) {
		uint8_t *frame = buffer + 24 + i * FRAME_BYTES;
		frame[FRAME_TYPE + 1] = AUTH_WRITE;
		frame[FRAME_COUNT + 1] = 2;
		private_frame_fields(frame);
	}
}

static void rpmb_snapshot(struct goodix_storage *storage)
{
	struct goodix_rpmb backend = { .fd = 123, .sectors = 32768, .max_frames = 39 };
	struct storage_listener listener = {
		.storage = storage, .service = RPMB_SERVICE_ID,
		.shared = buffer, .work = work, .size = RPMB_BUFFER_BYTES,
	};
	uint32_t wide = UINT32_MAX;
	ObjectArg args[6] = { [1].b = { &wide, sizeof(wide) } };

	storage->rpmb = &backend;
	goodix_storage_allow_rpmb(storage, 1);
	memset(work, 0xa5, RPMB_BUFFER_BYTES);
	rpmb_write_request();
	const uint32_t versions[] = {0, 0x100};
	for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
		rpmb_write_request();
		memcpy(buffer + 16, &versions[i], sizeof(versions[i]));
		mutate_rpmb_shared = 1;
		rpmb_ioctls = 0;
		assert(storage_invoke(&listener, 0, args,
				      ObjectCounts_pack(0, 2, 0, 4)) == Object_OK);
		assert(!mutate_rpmb_shared && rpmb_ioctls == 3);
		uint32_t response[5];
		memcpy(response, buffer, sizeof(response));
		assert(response[0] == 0x103 && response[1] == 0 && response[2] == FRAME_BYTES &&
		       response[3] == 20 && response[4] == versions[i]);
		assert(frame_word(buffer + 20, FRAME_TYPE) == AUTH_WRITE << 8);
		for (size_t j = 0; j < RPMB_BUFFER_BYTES; ++j)
			assert(work[j] == 0);
	}
	goodix_storage_allow_rpmb(storage, 0);
	storage->rpmb = NULL;
}

static void rpmb_transfer_failures(struct goodix_storage *storage)
{
	struct storage_listener listener = {
		.storage = storage, .service = RPMB_SERVICE_ID,
		.shared = buffer, .work = work, .size = RPMB_BUFFER_BYTES,
	};
	uint32_t wide = UINT32_MAX;
	ObjectArg args[6] = { [1].b = { &wide, sizeof(wide) } };

	for (int stage = 0; stage < 3; ++stage)
		for (int short_transfer = 0; short_transfer < 2; ++short_transfer) {
			struct goodix_rpmb backend = { .fd = 123, .sectors = 32768, .max_frames = 39 };
			storage->rpmb = &backend;
			goodix_storage_allow_rpmb(storage, 1);
			rpmb_failure_stage = stage;
			rpmb_short_transfer = short_transfer;
			rpmb_ioctls = 0;
			rpmb_write_request();
			assert(storage_invoke(&listener, 0, args,
					      ObjectCounts_pack(0, 2, 0, 4)) == Object_OK);
			uint32_t response[5];
			memcpy(response, buffer, sizeof(response));
			assert((int32_t)response[1] == -EIO && response[2] == 0);
			assert(backend.failed && rpmb_ioctls == stage + 1);
			/* A second secure-world request must not retry an uncertain
			 * write, its result query or a partially received response.
			 */
			rpmb_write_request();
			assert(storage_invoke(&listener, 0, args,
					      ObjectCounts_pack(0, 2, 0, 4)) == Object_OK);
			memcpy(response, buffer, sizeof(response));
			assert((int32_t)response[1] == -EIO && response[2] == 0);
			assert(rpmb_ioctls == stage + 1);
			for (size_t i = 0; i < RPMB_BUFFER_BYTES; ++i)
				assert(work[i] == 0);
			goodix_storage_allow_rpmb(storage, 0);
			storage->rpmb = NULL;
		}
}

struct listener_release {
	struct storage_listener *listener;
	int *registrations, *memories;
	unsigned int total;
};

static int32_t registration_release(ObjectCxt context, ObjectOp operation,
				    ObjectArg *args, ObjectCounts counts)
{
	struct listener_release *release = context;
	(void)args;
	assert(operation == Object_OP_release && counts == 0);
	assert(release->listener->storage->stopped);
	assert(release->listener->storage->directory == -1);
	assert(!release->listener->storage->files[0].handle);
	(*release->registrations)++;
	Object callback = {storage_invoke, release->listener};
	Object_release(callback);
	if (*release->registrations == (int)release->total)
		assert(!sem_post(&registrations_released));
	return Object_OK;
}

static int32_t memory_release(ObjectCxt context, ObjectOp operation,
			      ObjectArg *args, ObjectCounts counts)
{
	struct listener_release *release = context;
	struct storage_listener *listener = release->listener;
	(void)args;
	assert(operation == Object_OP_release && counts == 0);
	for (size_t i = 0; i < listener->size; ++i)
		assert(((uint8_t *)listener->shared)[i] == 0);
	(*release->memories)++;
	free(listener->shared);
	return Object_OK;
}

static void *pending_file_write(void *context)
{
	struct storage_listener *listener = context;
	uint32_t wide;
	ObjectArg args[6] = { [1].b = { &wide, sizeof(wide) } };

	assert(storage_invoke(listener, 0, args, ObjectCounts_pack(0, 2, 0, 4)) == Object_OK);
	assert(!((tz_gpfile_write_rsp_t *)listener->shared)->err);
	return NULL;
}

static void *close_storage(void *context)
{
	assert(!sem_post(&close_started));
	goodix_storage_close(context);
	assert(!sem_post(&close_finished));
	return NULL;
}

static void listener_lifetime(int directory, unsigned int registered)
{
	struct goodix_storage *storage = calloc(1, sizeof(*storage));
	struct listener_release releases[LISTENER_COUNT];
	int registrations = 0, memories = 0;

	assert(storage);
	atomic_init(&storage->references, 1);
	assert(!pthread_mutex_init(&storage->mutex, NULL));
	assert(!pthread_cond_init(&storage->released, NULL));
	assert(!mkdirat(directory, "listeners", 0700));
	storage->directory = openat(directory, "listeners", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(storage->directory >= 0);
	assert(!storage_claim_directory(storage->directory, 1));
	int owned_directory = storage->directory;
	assert(!write_range(storage, "file", "data", 4, 0, 0));
	tz_file_open_req_t *request = (void *)buffer;
	memset(buffer, 0, sizeof(buffer));
	request->cmd_id = TZ_FS_MSG_CMD_FILE_OPEN;
	path(request->pathname, "file");
	assert(!storage_fs_dispatch(storage, buffer, sizeof(buffer)));
	assert(((tz_file_open_rsp_t *)buffer)->ret > 0);
	int owned_file = storage->files[0].fd;
	storage->rpmb = calloc(1, sizeof(*storage->rpmb));
	assert(storage->rpmb);
	storage->rpmb->fd = -1;
	for (size_t i = 0; i < registered; ++i) {
		struct storage_listener *listener = &storage->listeners[i];
		listener->storage = storage;
		listener->service = i == 2 ? RPMB_SERVICE_ID :
			(i ? FILE_SERVICE_ID : GPFILE_SERVICE_ID);
		listener->size = RPMB_BUFFER_BYTES;
		listener->shared = malloc(listener->size);
		listener->work = malloc(listener->size);
		assert(listener->shared && listener->work);
		memset(listener->shared, 0xa5, listener->size);
		memset(listener->work, 0xa5, listener->size);
		releases[i] = (struct listener_release){listener, &registrations, &memories, registered};
		listener->registration = (Object){registration_release, &releases[i]};
		listener->memory = (Object){memory_release, &releases[i]};
		Object callback = {storage_invoke, listener};
		Object_retain(callback);
		/* Model secure-world callback references arriving after listener
		 * deregistration. Shared memory must outlive those references.
		 */
		Object_retain(callback);
	}
	goodix_storage_allow_rpmb(storage, 1);

	/* Pause an actual GPFS write inside its real fsync. Shutdown must wait
	 * for the callback's mutex before closing the directory used by rename.
	 */
	assert(!sem_init(&callback_in_fsync, 0, 0) && !sem_init(&callback_continue, 0, 0));
	assert(!sem_init(&close_started, 0, 0) && !sem_init(&close_finished, 0, 0));
	assert(!sem_init(&registrations_released, 0, 0));
	struct storage_listener *gpfs = &storage->listeners[0];
	tz_gpfile_write_req_t *write = gpfs->shared;
	memset(write, 0, gpfs->size);
	write->cmd_id = (tz_gpfs_msg_cmd_type)GPFS_WRITE;
	path(write->pathname, "inflight");
	write->count = 4;
	memcpy(write->buf, "data", 4);
	atomic_store(&pause_fsync, 1);
	pthread_t callback_thread, close_thread;
	assert(!pthread_create(&callback_thread, NULL, pending_file_write, gpfs));
	assert(!sem_wait(&callback_in_fsync));
	assert(!pthread_create(&close_thread, NULL, close_storage, storage));
	assert(!sem_wait(&close_started));
	struct timespec deadline;
	assert(!clock_gettime(CLOCK_REALTIME, &deadline));
	deadline.tv_nsec += 50000000;
	if (deadline.tv_nsec >= 1000000000) {
		deadline.tv_nsec -= 1000000000;
		deadline.tv_sec++;
	}
	int waited;
	do {
		waited = sem_timedwait(&close_finished, &deadline);
	} while (waited && errno == EINTR);
	assert(waited == -1 && errno == ETIMEDOUT);
	assert(fcntl(owned_directory, F_GETFD) >= 0 && fcntl(owned_file, F_GETFD) >= 0);
	assert(!storage->stopped && storage->allow_rpmb);
	assert(!sem_post(&callback_continue));
	assert(!pthread_join(callback_thread, NULL));
	assert(!sem_wait(&registrations_released));
	assert(registrations == (int)registered && !memories);
	assert(sem_trywait(&close_finished) == -1 && errno == EAGAIN);
	assert(fcntl(owned_directory, F_GETFD) == -1 && errno == EBADF);
	assert(fcntl(owned_file, F_GETFD) == -1 && errno == EBADF);
	assert(!storage->allow_rpmb && storage->rpmb && storage->directory == -1);

	/* Files are already drained, but close cannot finish until every remote
	 * reference is returned. No shared buffer may be released in the meantime.
	 */
	int next = openat(directory, "listeners", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(next >= 0 && !storage_claim_directory(next, 1));
	expect_file(next, "inflight", "data", 4);
	close(next);
	for (size_t i = 0; i < registered; ++i) {
		struct storage_listener *listener = &storage->listeners[i];
		memset(listener->shared, 0xa5, listener->size);
		uint32_t wide = UINT32_MAX;
		ObjectArg args[6] = { [1].b = { &wide, sizeof(wide) } };
		assert(storage_invoke(listener, 0, args,
			ObjectCounts_pack(0, 2, 0, 4)) == Object_ERROR);
		assert(wide == 0 && !args[0].b.size && !storage->rpmb_requests);
		for (size_t j = 0; j < listener->size; ++j)
			assert(((uint8_t *)listener->shared)[j] == 0xa5);
	}
	for (size_t i = 0; i < registered; ++i) {
		Object callback = {storage_invoke, &storage->listeners[i]};
		Object_release(callback);
		if (i + 1 < registered) {
			assert(!memories);
			assert(sem_trywait(&close_finished) == -1 && errno == EAGAIN);
		}
	}
	assert(!sem_wait(&close_finished));
	assert(!pthread_join(close_thread, NULL));
	assert(memories == (int)registered);
	assert(!sem_destroy(&callback_in_fsync) && !sem_destroy(&callback_continue));
	assert(!sem_destroy(&close_started) && !sem_destroy(&close_finished));
	assert(!sem_destroy(&registrations_released));
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	struct goodix_storage storage = {
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.released = PTHREAD_COND_INITIALIZER,
		.directory = storage_directory(argv[2]), .profile = 1,
	};
	assert(storage.directory >= 0 && storage_claim_directory(storage.directory, 1) == 0);
	if (!strcmp(argv[1], "writes"))
		writes(&storage, argv[2]);
	else if (!strcmp(argv[1], "large-files"))
		large_files(&storage, argv[2]);
	else if (!strcmp(argv[1], "names"))
		names(&storage);
	else if (!strcmp(argv[1], "boundaries"))
		boundaries(&storage);
	else if (!strcmp(argv[1], "failures"))
		write_failures(&storage);
	else if (!strcmp(argv[1], "rpmb"))
		rpmb_denial(&storage);
	else if (!strcmp(argv[1], "snapshot"))
		rpmb_snapshot(&storage);
	else if (!strcmp(argv[1], "rpmb-failures"))
		rpmb_transfer_failures(&storage);
	else if (!strcmp(argv[1], "listeners"))
		listener_lifetime(storage.directory, LISTENER_COUNT);
	else if (!strcmp(argv[1], "listeners-partial"))
		listener_lifetime(storage.directory, 1);
	else {
		assert(!strcmp(argv[1], "handles"));
		fs_handles(&storage, argv[2]);
	}
	close(storage.directory);
	pthread_cond_destroy(&storage.released);
	pthread_mutex_destroy(&storage.mutex);
	return 0;
}
