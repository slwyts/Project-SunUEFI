// SPDX-License-Identifier: BSD-3-Clause
/* Exercise real credential persistence and CBOR code using an in-process TA
 * callback. No device, password handle or firmware outside this test is read.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <qcbor/qcbor_decode.h>
#include <qcbor/qcbor_encode.h>
#include <qcbor/qcbor_spiffy_decode.h>
#include "../../linux/userspace/fingerprint/piano-goodix-gatekeeper.h"

enum fault { NONE, RETRY, APPLICATION_ERROR, TRANSPORT_ERROR, BAD_LENGTH,
	     BAD_MAC, WRONG_CHALLENGE, WRONG_TYPE };

struct simulated_ta {
	uint32_t hardware_uid;
	uint8_t secret[32];
	int captured_secret, enroll_calls, verify_calls;
	enum fault fault;
};

static const uint8_t test_handle[] = { 2, 8, 6, 4, 2, 7, 9, 3 };

static int command(void *context, const void *request, size_t request_bytes,
		   uint8_t *response, size_t capacity, size_t *actual, int32_t *status)
{
	struct simulated_ta *ta = context;
	uint32_t opcode;
	uint64_t uid, challenge = 0;
	UsefulBufC secret, handle, encoded;
	QCBORDecodeContext decoder;
	QCBOREncodeContext encoder;

	assert(request_bytes > 4 && capacity >= 256);
	memcpy(&opcode, request, sizeof(opcode));
	opcode = le32toh(opcode);
	assert(opcode == 0x21001 || opcode == 0x21002);
	QCBORDecode_Init(&decoder,
		(UsefulBufC){(const uint8_t *)request + 4, request_bytes - 4},
		QCBOR_DECODE_MODE_NORMAL);
	QCBORDecode_EnterMap(&decoder, NULL);
	QCBORDecode_GetUInt64InMapN(&decoder, 3, &uid);
	if (opcode == 0x21001) {
		++ta->enroll_calls;
		QCBORDecode_GetByteStringInMapN(&decoder, 8, &secret);
	} else {
		++ta->verify_calls;
		QCBORDecode_GetUInt64InMapN(&decoder, 4, &challenge);
		QCBORDecode_GetByteStringInMapN(&decoder, 6, &handle);
		QCBORDecode_GetByteStringInMapN(&decoder, 9, &secret);
	}
	QCBORDecode_ExitMap(&decoder);
	assert(QCBORDecode_Finish(&decoder) == QCBOR_SUCCESS); /* No padded CBOR. */
	assert(uid == ta->hardware_uid);
	assert(secret.len == sizeof(ta->secret));
	if (ta->captured_secret)
		assert(!memcmp(ta->secret, secret.ptr, secret.len));
	else {
		memcpy(ta->secret, secret.ptr, secret.len);
		ta->captured_secret = 1;
	}
	if (opcode == 0x21002) {
		assert(handle.len == sizeof(test_handle));
		assert(!memcmp(handle.ptr, test_handle, handle.len));
	}
	if (ta->fault == TRANSPORT_ERROR)
		return -EIO;
	*status = ta->fault == RETRY ? 30000 : ta->fault == APPLICATION_ERROR ? -30 : 0;
	memcpy(response, status, sizeof(*status));
	if (*status) {
		*actual = 4;
		return 0;
	}
	QCBOREncode_Init(&encoder, (UsefulBuf){response + 8, capacity - 8});
	QCBOREncode_OpenMap(&encoder);
	if (opcode == 0x21001) {
		QCBOREncode_AddBytesToMapN(&encoder, 6,
			(UsefulBufC){test_handle, sizeof(test_handle)});
	} else {
		uint8_t mac[32];

		memset(mac, 0x5a, sizeof(mac));
		QCBOREncode_AddUInt64ToMapN(&encoder, 11, 0);
		QCBOREncode_AddUInt64ToMapN(&encoder, 4,
			challenge + (ta->fault == WRONG_CHALLENGE));
		QCBOREncode_AddUInt64ToMapN(&encoder, 12, UINT64_C(0x1011121314151617));
		QCBOREncode_AddUInt64ToMapN(&encoder, 13, 0);
		QCBOREncode_AddUInt64ToMapN(&encoder, 14,
			htobe32(ta->fault == WRONG_TYPE ? 2 : 1));
		QCBOREncode_AddUInt64ToMapN(&encoder, 15,
			htobe64(UINT64_C(0x2122232425262728)));
		QCBOREncode_AddBytesToMapN(&encoder, 16,
			(UsefulBufC){mac, ta->fault == BAD_MAC ? 31 : sizeof(mac)});
	}
	QCBOREncode_CloseMap(&encoder);
	assert(QCBOREncode_Finish(&encoder, &encoded) == QCBOR_SUCCESS);
	uint32_t length = htole32(encoded.len + (ta->fault == BAD_LENGTH ? 99 : 0));

	memcpy(response + 4, &length, sizeof(length));
	*actual = encoded.len + 8;
	return 0;
}

static void cleanup(int directory_fd)
{
	unlinkat(directory_fd, "gatekeeper.credential", 0);
	unlinkat(directory_fd, ".gatekeeper.lock", 0);
}

static void lifecycle(int directory_fd)
{
	struct simulated_ta ta = { .hardware_uid = 0x400003e8 };
	struct goodix_gatekeeper gatekeeper = { .command = command, .context = &ta };
	struct goodix_gatekeeper_status status;
	uint8_t hat[GOODIX_GATEKEEPER_HAT_BYTES], expected[GOODIX_GATEKEEPER_HAT_BYTES] = {0};

	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == 0);
	assert(status.value == 0 && status.retry_ms == 0 && ta.enroll_calls == 1);
	struct stat information;

	assert(fstatat(directory_fd, "gatekeeper.credential", &information, 0) == 0);
	assert((information.st_mode & 0777) == 0600);
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == 0);
	assert(ta.enroll_calls == 1); /* A restart must not replace the secure SID. */
	uint64_t challenge = UINT64_C(0x0102030405060708);
	uint64_t sid = UINT64_C(0x1011121314151617);
	uint64_t timestamp = htobe64(UINT64_C(0x2122232425262728));
	uint32_t type = htobe32(1);

	memcpy(expected + 1, &challenge, sizeof(challenge));
	memcpy(expected + 9, &sid, sizeof(sid));
	memcpy(expected + 25, &type, sizeof(type));
	memcpy(expected + 29, &timestamp, sizeof(timestamp));
	memset(expected + 37, 0x5a, 32);
	assert(goodix_gatekeeper_verify(&gatekeeper, directory_fd, 1000,
		challenge, hat, &status) == 0);
	assert(!memcmp(hat, expected, sizeof(hat)));
	assert(ta.verify_calls == 1);
	/* No other Linux UID may use the record, even if the directory is reused. */
	assert(goodix_gatekeeper_verify(&gatekeeper, directory_fd, 1001,
		challenge, hat, &status) == -EBADMSG);
	assert(ta.verify_calls == 1);
}

static void retries(int directory_fd)
{
	struct simulated_ta ta = { .hardware_uid = 0x400003e8, .fault = RETRY };
	struct goodix_gatekeeper gatekeeper = { .command = command, .context = &ta };
	struct goodix_gatekeeper_status status;
	uint8_t hat[GOODIX_GATEKEEPER_HAT_BYTES];

	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == -EAGAIN);
	assert(status.value == 30000 && status.retry_ms == 30000 && ta.enroll_calls == 1);
	assert(goodix_gatekeeper_verify(&gatekeeper, directory_fd, 1000, 1, hat, &status)
		== -ENODATA);
	ta.fault = NONE;
	/* Recover the pending transaction with the same randomly generated secret. */
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == 0);
	assert(ta.enroll_calls == 2);
	ta.fault = RETRY;
	memset(hat, 0xff, sizeof(hat));
	assert(goodix_gatekeeper_verify(&gatekeeper, directory_fd, 1000, 1, hat, &status)
		== -EAGAIN);
	assert(status.retry_ms == 30000);
	for (size_t i = 0; i < sizeof(hat); ++i)
		assert(hat[i] == 0);
}

static void protocol_errors(int directory_fd)
{
	struct simulated_ta ta = { .hardware_uid = 0x400003e8 };
	struct goodix_gatekeeper gatekeeper = { .command = command, .context = &ta };
	struct goodix_gatekeeper_status status;
	uint8_t hat[GOODIX_GATEKEEPER_HAT_BYTES];

	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == 0);
	const enum fault faults[] = { APPLICATION_ERROR, TRANSPORT_ERROR, BAD_LENGTH,
		BAD_MAC, WRONG_CHALLENGE, WRONG_TYPE };
	const int results[] = { -EREMOTEIO, -EIO, -EMSGSIZE, -EBADMSG, -EBADMSG, -EBADMSG };

	for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
		ta.fault = faults[i];
		memset(hat, 0xff, sizeof(hat));
		assert(goodix_gatekeeper_verify(&gatekeeper, directory_fd, 1000, 1, hat, &status)
			== results[i]);
		if (faults[i] == APPLICATION_ERROR)
			assert(status.value == -30 && !status.retry_ms);
		for (size_t byte = 0; byte < sizeof(hat); ++byte)
			assert(hat[byte] == 0);
	}
}

static void storage_errors(int directory_fd)
{
	struct simulated_ta ta = { .hardware_uid = 0x400003e8 };
	struct goodix_gatekeeper gatekeeper = { .command = command, .context = &ta };
	struct goodix_gatekeeper_status status;
	uint32_t hardware_uid;

	assert(goodix_gatekeeper_uid(0, &hardware_uid) == 0 && hardware_uid == 0x40000000);
	assert(goodix_gatekeeper_uid(0x0fffffff, &hardware_uid) == 0 && hardware_uid == 0x4fffffff);
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 0x10000000, &status) == -ERANGE);
	assert(!ta.enroll_calls);
	assert(symlinkat("/dev/null", directory_fd, "gatekeeper.credential") == 0);
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == -ELOOP);
	assert(!ta.enroll_calls);
	assert(unlinkat(directory_fd, "gatekeeper.credential", 0) == 0);
	int fd = openat(directory_fd, "gatekeeper.credential", O_WRONLY | O_CREAT, 0600);

	assert(fd >= 0 && write(fd, "bad", 3) == 3);
	close(fd);
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == -EBADMSG);
	assert(!ta.enroll_calls); /* Corrupt state must not reset a secure user. */
	assert(unlinkat(directory_fd, "gatekeeper.credential", 0) == 0);
	fd = openat(directory_fd, "gatekeeper.credential", O_WRONLY | O_CREAT, 0600);
	assert(fd >= 0);
	assert(fchmod(fd, 0644) == 0);
	close(fd);
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == -EACCES);
	assert(unlinkat(directory_fd, "gatekeeper.credential", 0) == 0);
	fd = openat(directory_fd, "gatekeeper.credential", O_WRONLY | O_CREAT, 0600);
	assert(fd >= 0);
	close(fd);
	assert(linkat(directory_fd, "gatekeeper.credential", directory_fd, "alias", 0) == 0);
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == -EACCES);
	assert(unlinkat(directory_fd, "alias", 0) == 0);
	assert(unlinkat(directory_fd, "gatekeeper.credential", 0) == 0);
	assert(unlinkat(directory_fd, ".gatekeeper.lock", 0) == 0);
	assert(symlinkat("/dev/null", directory_fd, ".gatekeeper.lock") == 0);
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == -ELOOP);
	assert(unlinkat(directory_fd, ".gatekeeper.lock", 0) == 0);
	assert(!ta.enroll_calls);
	assert(fchmod(directory_fd, 0755) == 0);
	assert(goodix_gatekeeper_create(&gatekeeper, directory_fd, 1000, &status) == -EACCES);
	assert(fchmod(directory_fd, 0700) == 0);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	char directory[] = "/tmp/piano-gatekeeper-state-XXXXXX";

	assert(mkdtemp(directory));
	int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	assert(fd >= 0);
	if (!strcmp(argv[1], "lifecycle"))
		lifecycle(fd);
	else if (!strcmp(argv[1], "retry"))
		retries(fd);
	else if (!strcmp(argv[1], "protocol"))
		protocol_errors(fd);
	else {
		assert(!strcmp(argv[1], "storage"));
		storage_errors(fd);
	}
	cleanup(fd);
	close(fd);
	assert(rmdir(directory) == 0);
	return 0;
}
