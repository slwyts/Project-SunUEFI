// SPDX-License-Identifier: BSD-3-Clause
/* Exercise actual wire validation and refusal paths. Never open a device or
 * invoke a SCSI ioctl; no transport implementation is substituted or mocked.
 */
#include "../../linux/userspace/fingerprint/piano-goodix-rpmb.c"

#include <assert.h>

static uint8_t buffer[CALLBACK_BYTES];
static struct goodix_rpmb rpmb = { .fd = -1, .sectors = 32768, .max_frames = 64 };

static void word(uint8_t *frame, size_t offset, uint16_t value)
{
	frame[offset] = value >> 8;
	frame[offset + 1] = value;
}

static void request(uint32_t command, uint32_t count, uint16_t type)
{
	uint32_t header[] = {command, count, command == 0x103 ? count * FRAME_BYTES : FRAME_BYTES,
			     24, 0, command == 0x103 ? count : 0};

	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer, header, sizeof(header));
	for (size_t i = 0; i < header[2] / FRAME_BYTES; ++i) {
		uint8_t *frame = buffer + 24 + i * FRAME_BYTES;
		word(frame, FRAME_TYPE, type);
		word(frame, FRAME_ADDRESS, 1024);
		word(frame, FRAME_COUNT, count);
	}
}

static void accepted_frames(void)
{
	struct rpmb_transaction transaction;
	const uint32_t versions[] = {0, 0x100};

	/* The live denial probe supplied exactly this outer request geometry. */
	for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
		request(0x102, 24, AUTH_READ);
		memcpy(buffer + 16, &versions[i], sizeof(versions[i]));
		assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == 0);
		assert(transaction.command == 0x102 && transaction.frames == 24 &&
		       transaction.input_bytes == 512 && transaction.input_offset == 24 &&
		       transaction.version == versions[i]);
		request(0x102, 1, GET_COUNTER);
		memcpy(buffer + 16, &versions[i], sizeof(versions[i]));
		assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == 0);
		request(0x103, 2, AUTH_WRITE);
		memcpy(buffer + 16, &versions[i], sizeof(versions[i]));
		assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == 0);
		assert(transaction.type == AUTH_WRITE && transaction.input_bytes == 1024 &&
		       transaction.frames == 2 && transaction.version == versions[i]);
	}
	request(0x102, MAX_FRAMES, AUTH_READ);
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == 0);
}

static void prohibited_frames(void)
{
	struct rpmb_transaction transaction;
	const uint32_t unsupported_versions[] = {1, 0xff, 0x101, 0x200, UINT32_MAX};

	for (size_t i = 0; i < sizeof(unsupported_versions) / sizeof(unsupported_versions[0]); ++i) {
		request(0x103, 2, AUTH_WRITE);
		memcpy(buffer + 16, &unsupported_versions[i], sizeof(unsupported_versions[i]));
		assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EOPNOTSUPP);
		assert(goodix_rpmb_dispatch(&rpmb, buffer, sizeof(buffer)) == 0);
		uint32_t response[5];
		memcpy(response, buffer, sizeof(response));
		assert((int32_t)response[1] == -EOPNOTSUPP && response[2] == 0 &&
		       response[4] == unsupported_versions[i]);
	}

	for (uint16_t type = 0; type < 12; ++type) {
		if (type != GET_COUNTER && type != AUTH_READ) {
			request(0x102, 1, type);
			assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EPERM);
		}
		if (type != AUTH_WRITE) {
			request(0x103, 1, type);
			assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EPERM);
		}
	}
	request(0x103, 2, AUTH_WRITE);
	word(buffer + 24 + FRAME_BYTES, FRAME_TYPE, 1); /* Key programming inside a batch. */
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EPERM);
	/* The real dispatch must refuse before reaching transfer/ioctl. */
	assert(goodix_rpmb_dispatch(&rpmb, buffer, sizeof(buffer)) == 0);
	uint32_t response[5];
	memcpy(response, buffer, sizeof(response));
	assert((int32_t)response[1] == -EPERM && response[2] == 0);
	assert(frame_word(buffer + 24 + FRAME_BYTES, FRAME_TYPE) == 1);
	request(0x102, 2, GET_COUNTER);
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EPERM);
}

static void boundaries(void)
{
	struct rpmb_transaction transaction;
	uint32_t header[6];

	request(0x102, 24, AUTH_READ);
	assert(validate_transaction(&rpmb, buffer, 23, &transaction) == -EMSGSIZE);
	memcpy(header, buffer, sizeof(header));
	header[3] = UINT32_MAX;
	memcpy(buffer, header, sizeof(header));
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EMSGSIZE);
	request(0x102, 40, AUTH_READ);
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EMSGSIZE);
	request(0x102, 24, AUTH_READ);
	assert(validate_transaction(&rpmb, buffer, 1024, &transaction) == -EMSGSIZE);
	word(buffer + 24, FRAME_ADDRESS, 65535);
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EINVAL);
	request(0x103, 2, AUTH_WRITE);
	buffer[24 + FRAME_BYTES + FRAME_COUNTER] = 1;
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EINVAL);
	request(0x103, 2, AUTH_WRITE);
	word(buffer + 24, FRAME_COUNT, 0);
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EINVAL);
	request(0x103, 2, AUTH_WRITE);
	word(buffer + 24 + FRAME_BYTES, FRAME_ADDRESS, 1025);
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EINVAL);
	request(0x103, 2, AUTH_WRITE);
	memcpy(header, buffer, sizeof(header));
	header[5] = 1;
	memcpy(buffer, header, sizeof(header));
	assert(validate_transaction(&rpmb, buffer, sizeof(buffer), &transaction) == -EPERM);
}

static void metadata_and_errors(void)
{
	uint8_t device[64] = { [16] = 4 }, geometry[72] = { [23] = 64 };
	uint8_t unit[35] = { [2] = 0xc4, [19] = 128 };
	struct goodix_rpmb parsed = { .fd = -1 };

	assert(parse_geometry(&parsed, device, geometry, unit) == 0);
	assert(parsed.sectors == 32768 && parsed.max_frames == MAX_FRAMES);
	unit[9] = 0x10;
	assert(parse_geometry(&parsed, device, geometry, unit) == -EOPNOTSUPP);
	unit[9] = 0;
	unit[19] = 0;
	assert(parse_geometry(&parsed, device, geometry, unit) == -EINVAL);
	device[16] = 2;
	unit[10] = 8;
	uint64_t blocks = htobe64(65536);
	memcpy(unit + 11, &blocks, sizeof(blocks));
	assert(parse_geometry(&parsed, device, geometry, unit) == 0 && parsed.sectors == 32768);
	uint32_t init[] = {0x101, 0};
	memcpy(buffer, init, sizeof(init));
	assert(goodix_rpmb_dispatch(&rpmb, buffer, sizeof(buffer)) == 0);
	uint32_t response[5];
	memcpy(response, buffer, sizeof(response));
	assert(response[0] == 0x101 && response[2] == 0 && response[3] == 32768 && response[4] == 64);
	uint32_t partition[] = {0x104, 0x100, 9};
	memcpy(buffer, partition, sizeof(partition));
	assert(goodix_rpmb_dispatch(&rpmb, buffer, sizeof(buffer)) == 0);
	memcpy(response, buffer, sizeof(response));
	assert((int32_t)response[1] == -EPERM && response[2] == 0 && response[3] == 16);
	request(0x102, 24, AUTH_READ);
	rpmb.failed = 1;
	assert(goodix_rpmb_dispatch(&rpmb, buffer, sizeof(buffer)) == 0);
	memcpy(response, buffer, sizeof(response));
	assert((int32_t)response[1] == -EIO && response[2] == 0);
	struct sg_io_v4 io = { .din_resid = 1 };
	assert(sg_result(&io, 0) == -EIO);
	io.din_resid = 0;
	assert(sg_result(&io, 0) == 0);
	errno = ETIMEDOUT;
	assert(sg_result(&io, -1) == -ETIMEDOUT);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "allowed"))
		accepted_frames();
	else if (!strcmp(argv[1], "prohibited"))
		prohibited_frames();
	else if (!strcmp(argv[1], "bounds"))
		boundaries();
	else {
		assert(!strcmp(argv[1], "metadata"));
		metadata_and_errors();
	}
	return 0;
}
