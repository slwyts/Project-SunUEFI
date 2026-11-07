// SPDX-License-Identifier: BSD-3-Clause
/* Native Gatekeeper CBOR follows this device's libqtigatekeeper serializer.
 * Only the caller's private Linux directory is used for credential storage.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <qcbor/qcbor_decode.h>
#include <qcbor/qcbor_encode.h>
#include <qcbor/qcbor_spiffy_decode.h>
#include "piano-goodix-gatekeeper.h"

#define CREDENTIAL_FILE "gatekeeper.credential"
#define HANDLE_LIMIT 256U
#define SECRET_BYTES 32U
#define COMMAND_ENROLL 0x21001U
#define COMMAND_VERIFY 0x21002U

struct credential {
	uint8_t magic[8];
	uint32_t linux_uid, hardware_uid, handle_bytes;
	uint8_t secret[SECRET_BYTES];
	uint8_t handle[HANDLE_LIMIT];
};

static const uint8_t credential_magic[8] = { 'P', 'I', 'A', 'N', 'O', 'G', 'K', 1 };
_Static_assert(offsetof(struct credential, handle) == 52, "Credential file header");

static void clear_secret(void *memory, size_t bytes)
{
	volatile uint8_t *position = memory;

	while (bytes--)
		*position++ = 0;
}

int goodix_gatekeeper_uid(uint32_t linux_uid, uint32_t *hardware_uid)
{
	if (linux_uid > 0x0fffffffU)
		return -ERANGE;
	*hardware_uid = 0x40000000U | linux_uid;
	return 0;
}

static int random_bytes(void *memory, size_t bytes)
{
	uint8_t *position = memory;

	while (bytes) {
		ssize_t count = getrandom(position, bytes, 0);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count ? -errno : -EIO;
		position += count;
		bytes -= count;
	}
	return 0;
}

static int private_file(int fd)
{
	struct stat information;

	if (fstat(fd, &information))
		return -errno;
	return S_ISREG(information.st_mode) && information.st_nlink == 1 &&
		information.st_uid == geteuid() && (information.st_mode & 0777) == 0600 ?
		0 : -EACCES;
}

static int lock_directory(int directory_fd)
{
	struct stat information;

	if (fstat(directory_fd, &information))
		return -errno;
	if (!S_ISDIR(information.st_mode) || information.st_uid != geteuid() ||
	    (information.st_mode & 0777) != 0700)
		return -EACCES;
	int fd = openat(directory_fd, ".gatekeeper.lock",
			O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
	if (fd < 0)
		return -errno;
	int result = private_file(fd);
	if (!result && flock(fd, LOCK_EX | LOCK_NB))
		result = -errno;
	if (result) {
		close(fd);
		return result;
	}
	return fd;
}

static int load_credential(int directory_fd, uint32_t linux_uid,
			   struct credential *credential)
{
	struct stat information;
	uint32_t hardware_uid;
	int result = goodix_gatekeeper_uid(linux_uid, &hardware_uid);

	if (result)
		return result;
	int fd = openat(directory_fd, CREDENTIAL_FILE,
			O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	result = private_file(fd);
	if (!result && fstat(fd, &information))
		result = -errno;
	if (!result && (information.st_size < (off_t)offsetof(struct credential, handle) ||
			information.st_size > (off_t)sizeof(*credential)))
		result = -EBADMSG;
	size_t done = 0;
	while (!result && done < (size_t)information.st_size) {
		ssize_t count = read(fd, (uint8_t *)credential + done,
				     information.st_size - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			result = count ? -errno : -EIO;
		else
			done += count;
	}
	close(fd);
	if (result)
		return result;
	uint32_t handle_bytes = le32toh(credential->handle_bytes);
	if (memcmp(credential->magic, credential_magic, sizeof(credential_magic)) ||
	    le32toh(credential->linux_uid) != linux_uid ||
	    le32toh(credential->hardware_uid) != hardware_uid || handle_bytes > HANDLE_LIMIT ||
	    done != offsetof(struct credential, handle) + handle_bytes)
		return -EBADMSG;
	return 0;
}

static int save_credential(int directory_fd, const struct credential *credential)
{
	char temporary[48];
	uint64_t suffix;
	int result = random_bytes(&suffix, sizeof(suffix));

	if (result)
		return result;
	snprintf(temporary, sizeof(temporary), ".gatekeeper-%016" PRIx64, suffix);
	int fd = openat(directory_fd, temporary,
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -errno;
	size_t bytes = offsetof(struct credential, handle) + le32toh(credential->handle_bytes);
	size_t done = 0;
	while (done < bytes) {
		ssize_t count = write(fd, (const uint8_t *)credential + done, bytes - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			result = count ? -errno : -EIO;
			break;
		}
		done += count;
	}
	if (!result && fsync(fd))
		result = -errno;
	if (close(fd) && !result)
		result = -errno;
	if (!result && renameat(directory_fd, temporary, directory_fd, CREDENTIAL_FILE))
		result = -errno;
	if (!result && fsync(directory_fd))
		result = -errno;
	if (result)
		unlinkat(directory_fd, temporary, 0);
	return result;
}

static int exchange(const struct goodix_gatekeeper *gatekeeper,
		    uint32_t command, QCBOREncodeContext *encoder, uint8_t request[512],
		    uint8_t response[512], UsefulBufC *payload,
		    struct goodix_gatekeeper_status *status)
{
	UsefulBufC encoded;
	size_t actual = 0;
	uint32_t bytes;
	int32_t application_status = 0;

	if (QCBOREncode_Finish(encoder, &encoded))
		return -EMSGSIZE;
	uint32_t opcode = htole32(command);
	memcpy(request, &opcode, sizeof(opcode));
	int result = gatekeeper->command(gatekeeper->context, request, encoded.len + 4,
					response, 512, &actual, &application_status);
	if (result)
		return result;
	status->value = application_status;
	if (application_status > 0) {
		status->retry_ms = application_status;
		return -EAGAIN;
	}
	if (application_status < 0)
		return -EREMOTEIO;
	if (actual < 8 || actual > 512)
		return -EMSGSIZE;
	memcpy(&bytes, response + 4, sizeof(bytes));
	bytes = le32toh(bytes);
	if (bytes > actual - 8)
		return -EMSGSIZE;
	*payload = (UsefulBufC){response + 8, bytes};
	return 0;
}

int goodix_gatekeeper_create(const struct goodix_gatekeeper *gatekeeper,
			    int directory_fd, uint32_t linux_uid,
			    struct goodix_gatekeeper_status *status)
{
	struct credential credential = {0};
	uint8_t request[512] = {0}, response[512] = {0};
	uint32_t hardware_uid;
	UsefulBufC payload, handle;
	QCBOREncodeContext encoder;
	QCBORDecodeContext decoder;
	int locked = -1;
	int result = goodix_gatekeeper_uid(linux_uid, &hardware_uid);

	memset(status, 0, sizeof(*status));
	if (result)
		return result;
	locked = lock_directory(directory_fd);
	if (locked < 0)
		return locked;
	result = load_credential(directory_fd, linux_uid, &credential);
	if (result == -ENOENT) {
		memcpy(credential.magic, credential_magic, sizeof(credential_magic));
		credential.linux_uid = htole32(linux_uid);
		credential.hardware_uid = htole32(hardware_uid);
		result = random_bytes(credential.secret, sizeof(credential.secret));
		if (!result)
			result = save_credential(directory_fd, &credential);
	}
	if (result || credential.handle_bytes)
		goto done;
	QCBOREncode_Init(&encoder, (UsefulBuf){request + 4, sizeof(request) - 4});
	QCBOREncode_OpenMap(&encoder);
	QCBOREncode_AddUInt64ToMapN(&encoder, 3, hardware_uid);
	QCBOREncode_AddBytesToMapN(&encoder, 8,
				 (UsefulBufC){credential.secret, sizeof(credential.secret)});
	QCBOREncode_CloseMap(&encoder);
	result = exchange(gatekeeper, COMMAND_ENROLL, &encoder, request, response, &payload, status);
	if (result)
		goto done;
	QCBORDecode_Init(&decoder, payload, QCBOR_DECODE_MODE_NORMAL);
	QCBORDecode_EnterMap(&decoder, NULL);
	QCBORDecode_GetByteStringInMapN(&decoder, 6, &handle);
	QCBORDecode_ExitMap(&decoder);
	if (QCBORDecode_Finish(&decoder) || !handle.len || handle.len > sizeof(credential.handle)) {
		result = -EBADMSG;
		goto done;
	}
	memcpy(credential.handle, handle.ptr, handle.len);
	credential.handle_bytes = htole32(handle.len);
	result = save_credential(directory_fd, &credential);
done:
	clear_secret(&credential, sizeof(credential));
	clear_secret(request, sizeof(request));
	clear_secret(response, sizeof(response));
	close(locked);
	return result;
}

static int decode_hat(UsefulBufC payload, uint64_t expected_challenge,
		      uint8_t hat[GOODIX_GATEKEEPER_HAT_BYTES])
{
	QCBORDecodeContext decoder;
	uint64_t version, challenge, sid, authenticator, type, timestamp;
	UsefulBufC mac;

	QCBORDecode_Init(&decoder, payload, QCBOR_DECODE_MODE_NORMAL);
	QCBORDecode_EnterMap(&decoder, NULL);
	QCBORDecode_GetUInt64InMapN(&decoder, 11, &version);
	QCBORDecode_GetUInt64InMapN(&decoder, 4, &challenge);
	QCBORDecode_GetUInt64InMapN(&decoder, 12, &sid);
	QCBORDecode_GetUInt64InMapN(&decoder, 13, &authenticator);
	QCBORDecode_GetUInt64InMapN(&decoder, 14, &type);
	QCBORDecode_GetUInt64InMapN(&decoder, 15, &timestamp);
	QCBORDecode_GetByteStringInMapN(&decoder, 16, &mac);
	QCBORDecode_ExitMap(&decoder);
	if (QCBORDecode_Finish(&decoder) || version != 0 || challenge != expected_challenge ||
	    !sid || type != htobe32(1) || mac.len != 32)
		return -EBADMSG;
	/* The factory wire already represents type/timestamp in network byte
	 * order. Keep those integers' byte layout; challenge/SIDs stay native.
	 */
	uint32_t raw_type = type;

	hat[0] = version;
	memcpy(hat + 1, &challenge, sizeof(challenge));
	memcpy(hat + 9, &sid, sizeof(sid));
	memcpy(hat + 17, &authenticator, sizeof(authenticator));
	memcpy(hat + 25, &raw_type, sizeof(raw_type));
	memcpy(hat + 29, &timestamp, sizeof(timestamp));
	memcpy(hat + 37, mac.ptr, mac.len);
	return 0;
}

int goodix_gatekeeper_verify(const struct goodix_gatekeeper *gatekeeper,
			    int directory_fd, uint32_t linux_uid, uint64_t challenge,
			    uint8_t hat[GOODIX_GATEKEEPER_HAT_BYTES],
			    struct goodix_gatekeeper_status *status)
{
	struct credential credential = {0};
	uint8_t request[512] = {0}, response[512] = {0};
	QCBOREncodeContext encoder;
	UsefulBufC payload;
	uint32_t hardware_uid;
	int result = goodix_gatekeeper_uid(linux_uid, &hardware_uid);

	memset(status, 0, sizeof(*status));
	memset(hat, 0, GOODIX_GATEKEEPER_HAT_BYTES);
	if (result)
		return result;
	int locked = lock_directory(directory_fd);
	if (locked < 0)
		return locked;
	result = load_credential(directory_fd, linux_uid, &credential);
	if (!result && !credential.handle_bytes)
		result = -ENODATA;
	if (result)
		goto done;
	QCBOREncode_Init(&encoder, (UsefulBuf){request + 4, sizeof(request) - 4});
	QCBOREncode_OpenMap(&encoder);
	QCBOREncode_AddUInt64ToMapN(&encoder, 3, hardware_uid);
	QCBOREncode_AddUInt64ToMapN(&encoder, 4, challenge);
	QCBOREncode_AddBytesToMapN(&encoder, 6,
				 (UsefulBufC){credential.handle, le32toh(credential.handle_bytes)});
	QCBOREncode_AddBytesToMapN(&encoder, 9,
				 (UsefulBufC){credential.secret, sizeof(credential.secret)});
	QCBOREncode_CloseMap(&encoder);
	result = exchange(gatekeeper, COMMAND_VERIFY, &encoder, request, response, &payload, status);
	if (!result)
		result = decode_hat(payload, challenge, hat);
done:
	clear_secret(&credential, sizeof(credential));
	clear_secret(request, sizeof(request));
	clear_secret(response, sizeof(response));
	close(locked);
	return result;
}
