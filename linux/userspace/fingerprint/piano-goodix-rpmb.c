// SPDX-License-Identifier: BSD-3-Clause
/* Qualcomm CBO RPMB headers and UFS SECURITY PROTOCOL transport follow
 * minkipc/listeners/librpmbservice. Unlike that general listener, this backend
 * accepts one bounded authenticated transaction per request and has no API
 * for programming a key, changing UFS configuration or creating partitions.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/bsg.h>
#include <scsi/scsi_bsg_ufs.h>
#include <scsi/sg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "piano-goodix-rpmb.h"

enum { FRAME_BYTES = 512, CALLBACK_BYTES = 20040, MAX_FRAMES = 39 };
enum { GET_COUNTER = 2, AUTH_WRITE = 3, AUTH_READ = 4, RESULT_READ = 5 };
enum { FRAME_COUNTER = 500, FRAME_ADDRESS = 504, FRAME_COUNT = 506,
	FRAME_RESULT = 508, FRAME_TYPE = 510 };

struct goodix_rpmb {
	int fd, failed;
	uint32_t sectors, max_frames;
};

struct rpmb_transaction {
	uint32_t command, version, frames, input_bytes, input_offset;
	uint16_t type;
};

static uint16_t frame_word(const uint8_t *frame, size_t offset)
{
	return (uint16_t)frame[offset] << 8 | frame[offset + 1];
}

static int sg_result(const struct sg_io_v4 *io, int result)
{
	if (result)
		return result < 0 ? -errno : -EIO;
	if (io->info || io->device_status || io->transport_status || io->driver_status ||
	    io->din_resid || io->dout_resid)
		return -EIO;
	return 0;
}

/* Fixed protocol fields only. Never log a frame, payload, MAC, nonce or
 * write counter; the explicit diagnostic already owns any credential output.
 */
static void log_scsi(const char *stage, const struct sg_io_v4 *io, int call_result,
		     int call_errno, const uint8_t *sense)
{
	unsigned int key = 0, asc = 0, ascq = 0;
	if (sense && io->response_len >= 4) {
		unsigned int format = sense[0] & 0x7f;
		if ((format == 0x70 || format == 0x71) && io->response_len >= 14) {
			key = sense[2] & 15;
			asc = sense[12];
			ascq = sense[13];
		} else if (format == 0x72 || format == 0x73) {
			key = sense[1] & 15;
			asc = sense[2];
			ascq = sense[3];
		}
	}
	fprintf(stderr, "RPMB SG stage=%s ioctl=%d errno=%d info=%#x device=%#x "
		"transport=%#x driver=%#x din_resid=%d dout_resid=%d response_bytes=%u "
		"sense=%x/%02x/%02x duration_ms=%u\n", stage, call_result, call_errno,
		io->info, io->device_status, io->transport_status, io->driver_status,
		io->din_resid, io->dout_resid, io->response_len, key, asc, ascq, io->duration);
}

/* Validate against the kernel-created BSG dev_t, never an arbitrary path. */
static int open_bsg(const char *name)
{
	char path[PATH_MAX];
	unsigned int major_number, minor_number;
	struct stat st;

	snprintf(path, sizeof(path), "/sys/class/bsg/%s/dev", name);
	FILE *stream = fopen(path, "r");
	if (!stream)
		return -errno;
	int count = fscanf(stream, "%u:%u", &major_number, &minor_number);
	fclose(stream);
	if (count != 2)
		return -EINVAL;
	snprintf(path, sizeof(path), "/dev/bsg/%s", name);
	int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISCHR(st.st_mode) ||
	    st.st_rdev != makedev(major_number, minor_number)) {
		close(fd);
		return -ENODEV;
	}
	return fd;
}

/* Only READ_DESCRIPTOR with fixed ID/index pairs is emitted here. No query
 * opcode, descriptor or SCSI CDB comes from the TA or command line.
 */
static int read_descriptor(int fd, uint8_t id, uint8_t index,
			   uint8_t *data, size_t bytes, size_t minimum)
{
	struct ufs_bsg_request request = { .msgcode = 0x16 };
	struct ufs_bsg_reply response = {0};
	struct sg_io_v4 io = {
		.guard = 'Q', .protocol = BSG_PROTOCOL_SCSI,
		.subprotocol = BSG_SUB_PROTOCOL_SCSI_TRANSPORT,
		.request_len = sizeof(request), .request = (uintptr_t)&request,
		.max_response_len = sizeof(response), .response = (uintptr_t)&response,
		.din_xfer_len = bytes, .din_xferp = (uintptr_t)data, .timeout = 30000,
	};
	request.upiu_req.header.dword_0 = htobe32(0x16000000);
	request.upiu_req.header.dword_1 = htobe32(0x00010000);
	request.upiu_req.header.dword_2 = htobe32(bytes);
	request.upiu_req.qr.opcode = 1;
	request.upiu_req.qr.idn = id;
	request.upiu_req.qr.index = index;
	request.upiu_req.qr.length = htobe16(bytes);
	int result = ioctl(fd, SG_IO, &io);
	int call_errno = result < 0 ? errno : 0;
	log_scsi("read-descriptor", &io, result, call_errno, NULL);
	fprintf(stderr, "RPMB descriptor id=%u index=%u requested=%zu received=%u "
		"result=%d protocol_status=%#x\n", id, index, bytes,
		response.reply_payload_rcv_len, response.result,
		be32toh(response.upiu_rsp.header.dword_1) & 0xffff);
	/* A shorter descriptor is valid if all fields we use were transferred. */
	if (result)
		return result < 0 ? -call_errno : -EIO;
	if (io.info || io.device_status || io.transport_status || io.driver_status ||
	    response.result || response.reply_payload_rcv_len < minimum ||
	    response.reply_payload_rcv_len > bytes || data[0] < minimum || data[1] != id ||
	    (be32toh(response.upiu_rsp.header.dword_1) & 0xffff))
		return -EIO;
	return 0;
}

static int parse_geometry(struct goodix_rpmb *rpmb, const uint8_t device[64],
			  const uint8_t geometry[72], const uint8_t unit[35])
{
	if (unit[2] != 0xc4 || (unit[9] & 0x10))
		return -EOPNOTSUPP; /* Advanced RPMB is a different protocol. */
	rpmb->max_frames = geometry[23];
	if (rpmb->max_frames > MAX_FRAMES)
		rpmb->max_frames = MAX_FRAMES;
	if (((unsigned int)device[16] << 8 | device[17]) >= 0x300) {
		rpmb->sectors = (uint32_t)unit[19] * 256;
	} else {
		uint64_t blocks;
		memcpy(&blocks, unit + 11, sizeof(blocks));
		blocks = be64toh(blocks);
		if (blocks > 65536 || (blocks & 1) || unit[10] != 8)
			return -EOVERFLOW;
		rpmb->sectors = blocks / 2;
	}
	if (!rpmb->sectors || rpmb->sectors > 32768 || !rpmb->max_frames)
		return -EINVAL;
	return 0;
}

static int geometry(struct goodix_rpmb *rpmb, int fd)
{
	uint8_t device[64] = {0}, geometry[72] = {0}, unit[35] = {0};
	int result = read_descriptor(fd, 0, 0, device, sizeof(device), 18);
	if (!result)
		result = read_descriptor(fd, 7, 0, geometry, sizeof(geometry), 24);
	if (!result)
		result = read_descriptor(fd, 2, 0xc4, unit, sizeof(unit), 23);
	return result ? result : parse_geometry(rpmb, device, geometry, unit);
}

/* Match the upstream UFS listener's initial REQUEST SENSE, clearing an old
 * Unit Attention before an authenticated transaction. This reads sense only.
 */
static int request_sense(int fd)
{
	uint8_t cdb[6] = {3, 0, 0, 0, 18, 0}, data[18] = {0}, sense[96] = {0};
	struct sg_io_v4 io = {
		.guard = 'Q', .protocol = BSG_PROTOCOL_SCSI,
		.subprotocol = BSG_SUB_PROTOCOL_SCSI_CMD,
		.request_len = sizeof(cdb), .request = (uintptr_t)cdb,
		.max_response_len = sizeof(sense), .response = (uintptr_t)sense,
		.din_xfer_len = sizeof(data), .din_xferp = (uintptr_t)data, .timeout = 30000,
	};
	int result = ioctl(fd, SG_IO, &io);
	int error = sg_result(&io, result);
	log_scsi("request-sense", &io, result, result < 0 ? errno : 0, sense);
	return error;
}

int goodix_rpmb_open(struct goodix_rpmb **output)
{
	glob_t nodes = {0};
	unsigned int host, channel, target, lun;
	char control[32], tail;
	int result = -ENODEV, descriptor_fd = -1;
	struct goodix_rpmb *rpmb = NULL;

	*output = NULL;
	if (geteuid())
		return -EACCES;
	if (glob("/sys/class/bsg/*:49476", 0, NULL, &nodes) || nodes.gl_pathc != 1)
		goto done;
	const char *name = strrchr(nodes.gl_pathv[0], '/') + 1;
	if (sscanf(name, "%u:%u:%u:%u%c", &host, &channel, &target, &lun, &tail) != 4 ||
	    channel || target || lun != 49476)
		goto done;
	rpmb = calloc(1, sizeof(*rpmb));
	if (!rpmb) {
		result = -ENOMEM;
		goto done;
	}
	rpmb->fd = -1;
	snprintf(control, sizeof(control), "ufs-bsg%u", host);
	descriptor_fd = open_bsg(control);
	if (descriptor_fd < 0) {
		result = descriptor_fd;
		goto done;
	}
	result = geometry(rpmb, descriptor_fd);
	if (result)
		goto done;
	rpmb->fd = open_bsg(name);
	if (rpmb->fd < 0) {
		result = rpmb->fd;
		goto done;
	}
	result = request_sense(rpmb->fd);
	if (result)
		goto done;
	*output = rpmb;
	rpmb = NULL;
done:
	if (descriptor_fd >= 0)
		close(descriptor_fd);
	goodix_rpmb_close(rpmb);
	globfree(&nodes);
	return result;
}

/* Read only structural frame fields. Payload, MAC, nonce and counter are
 * forwarded unchanged for verification inside the signed TA and RPMB device.
 */
static int validate_transaction(const struct goodix_rpmb *rpmb, const void *buffer,
				size_t size, struct rpmb_transaction *transaction)
{
	uint32_t header[6];

	if (size < sizeof(header))
		return -EMSGSIZE;
	memcpy(header, buffer, sizeof(header));
	/* minkipc uses the same 24-byte header, 512-byte frames and 20-byte
	 * response for both versions. This TA sends 0 for reads and 0x100 for
	 * authenticated writes; retain the exact version in the response.
	 */
	if ((header[0] != 0x102 && header[0] != 0x103) ||
	    (header[4] != 0 && header[4] != 0x100))
		return -EOPNOTSUPP;
	uint32_t frames = header[1], bytes = header[2], offset = header[3];
	if (!frames || frames > MAX_FRAMES || frames > rpmb->max_frames ||
	    offset < sizeof(header) || offset > size || bytes > size - offset ||
	    bytes > CALLBACK_BYTES || bytes % FRAME_BYTES)
		return -EMSGSIZE;
	const uint8_t *input = (const uint8_t *)buffer + offset;
	if (bytes < FRAME_BYTES)
		return -EMSGSIZE;
	uint16_t type = frame_word(input, FRAME_TYPE);
	if (header[0] == 0x102) {
		if (bytes != FRAME_BYTES || (type != GET_COUNTER && type != AUTH_READ) ||
		    (type == GET_COUNTER && frames != 1))
			return -EPERM;
	} else if (type != AUTH_WRITE || frames != header[5] ||
		   bytes != frames * FRAME_BYTES) {
		return -EPERM;
	}
	uint32_t output_bytes = (type == AUTH_WRITE ? 1 : frames) * FRAME_BYTES;
	if (size < 20 || output_bytes > size - 20)
		return -EMSGSIZE;
	for (size_t i = 0; i < bytes / FRAME_BYTES; ++i) {
		const uint8_t *frame = input + i * FRAME_BYTES;
		uint32_t address = frame_word(frame, FRAME_ADDRESS);
		uint32_t count = frame_word(frame, FRAME_COUNT);
		if (frame_word(frame, FRAME_TYPE) != type)
			return -EPERM;
		if (type == AUTH_WRITE &&
		    (count != frames || address != frame_word(input, FRAME_ADDRESS) ||
		     memcmp(frame + FRAME_COUNTER, input + FRAME_COUNTER, 4)))
			return -EINVAL;
		if (type != GET_COUNTER &&
		    (address >= rpmb->sectors * 2 || frames > rpmb->sectors * 2 - address ||
		     (count && count != frames)))
			return -EINVAL;
	}
	*transaction = (struct rpmb_transaction){
		.command = header[0], .version = header[4], .frames = frames,
		.input_bytes = bytes, .input_offset = offset, .type = type,
	};
	return 0;
}

/* CDBs are generated here, never accepted from a listener request. Region0's
 * protocol-specific value is fixed to1; no ordinary SCSI WRITE or purge exists.
 */
static int transfer(struct goodix_rpmb *rpmb, void *frames, size_t bytes, int send,
		    const char *stage)
{
	uint8_t cdb[12] = { send ? 0xb5 : 0xa2, 0xec, 0, 1 };
	uint8_t sense[96] = {0};
	uint32_t length = htobe32(bytes);
	memcpy(cdb + 6, &length, sizeof(length));
	struct sg_io_v4 io = {
		.guard = 'Q', .protocol = BSG_PROTOCOL_SCSI,
		.subprotocol = BSG_SUB_PROTOCOL_SCSI_CMD,
		.request_len = sizeof(cdb), .request = (uintptr_t)cdb,
		.max_response_len = sizeof(sense), .response = (uintptr_t)sense,
		.timeout = 30000,
	};
	if (send) {
		io.dout_xfer_len = bytes;
		io.dout_xferp = (uintptr_t)frames;
	} else {
		io.din_xfer_len = bytes;
		io.din_xferp = (uintptr_t)frames;
	}
	/* Do not retry an ioctl: a failed return may follow a completed write. */
	int call_result = ioctl(rpmb->fd, SG_IO, &io);
	int result = sg_result(&io, call_result);
	log_scsi(stage, &io, call_result, call_result < 0 ? errno : 0, sense);
	if (result)
		rpmb->failed = 1;
	return result;
}

int goodix_rpmb_dispatch(struct goodix_rpmb *rpmb, void *buffer, size_t size)
{
	uint32_t header[6] = {0}, response[5] = {0};
	struct rpmb_transaction transaction;
	uint8_t *input = NULL, *output = NULL;
	size_t output_bytes = 0;
	int result = -EOPNOTSUPP;

	if (size < sizeof(response))
		return -EMSGSIZE;
	memcpy(header, buffer, size < sizeof(header) ? size : sizeof(header));
	response[0] = header[0];
	if (header[0] == 0x101) {
		fprintf(stderr, "RPMB command=%#x version=%#x\n", header[0], header[1]);
		response[1] = header[1];
		response[2] = rpmb->failed ? (uint32_t)-EIO : 0;
		response[3] = rpmb->sectors;
		response[4] = rpmb->max_frames;
		fprintf(stderr, "RPMB init status=%d sectors=%u max_frames=%u\n",
			(int32_t)response[2], rpmb->sectors, rpmb->max_frames);
		memcpy(buffer, response, sizeof(response));
		return 0;
	}
	if (header[0] == 0x104) {
		fprintf(stderr, "RPMB command=%#x version=%#x device=%u\n",
			header[0], header[1], header[2]);
		response[1] = (uint32_t)-EPERM;
		response[3] = 16;
		fputs("RPMB partition-table request refused\n", stderr);
		memcpy(buffer, response, 16);
		return 0;
	}
	if (header[0] == 0x102 || header[0] == 0x103)
		fprintf(stderr, "RPMB command=%#x version=%#x frames=%u bytes=%u offset=%u batch=%u\n",
			header[0], header[4], header[1], header[2], header[3], header[5]);
	else
		fprintf(stderr, "RPMB command=%#x unsupported\n", header[0]);
	if (!rpmb->failed)
		result = validate_transaction(rpmb, buffer, size, &transaction);
	else
		result = -EIO;
	if (result) {
		fprintf(stderr, "RPMB request rejected: error=%d session_failed=%d\n", result, rpmb->failed);
		goto done;
	}
	const uint8_t *frame = (uint8_t *)buffer + transaction.input_offset;
	fprintf(stderr, "RPMB request type=%#x address=%u block_count=%u\n",
		transaction.type, frame_word(frame, FRAME_ADDRESS), frame_word(frame, FRAME_COUNT));
	input = malloc(transaction.input_bytes);
	output_bytes = (transaction.type == AUTH_WRITE ? 1 : transaction.frames) * FRAME_BYTES;
	output = calloc(1, output_bytes);
	if (!input || !output) {
		result = -ENOMEM;
		goto done;
	}
	memcpy(input, (uint8_t *)buffer + transaction.input_offset, transaction.input_bytes);
	const char *stage = transaction.type == AUTH_WRITE ? "write-request" :
		(transaction.type == GET_COUNTER ? "counter-request" : "read-request");
	result = transfer(rpmb, input, transaction.input_bytes, 1, stage);
	if (!result && transaction.type == AUTH_WRITE) {
		output[FRAME_TYPE + 1] = RESULT_READ;
		result = transfer(rpmb, output, FRAME_BYTES, 1, "result-request");
		memset(output, 0, output_bytes);
	}
	if (!result)
		result = transfer(rpmb, output, output_bytes, 0, "response");
	if (!result) {
		const uint8_t *last = output + output_bytes - FRAME_BYTES;
		fprintf(stderr, "RPMB response first_type=%#x last_type=%#x first_result=%#x "
			"last_result=%#x first_address=%u last_address=%u block_count=%u\n",
			frame_word(output, FRAME_TYPE), frame_word(last, FRAME_TYPE),
			frame_word(output, FRAME_RESULT), frame_word(last, FRAME_RESULT),
			frame_word(output, FRAME_ADDRESS), frame_word(last, FRAME_ADDRESS),
			frame_word(last, FRAME_COUNT));
	}
	if (!result)
		for (size_t i = 0; i < output_bytes / FRAME_BYTES; ++i)
			if (frame_word(output + i * FRAME_BYTES, FRAME_TYPE) != transaction.type << 8) {
				result = -EBADMSG;
				rpmb->failed = 1;
				break;
			}
	if (!result)
		memcpy((uint8_t *)buffer + 20, output, output_bytes);
done:
	/* Error replies carry no frames, and no raw data is printed or persisted. */
	fprintf(stderr, "RPMB completed command=%#x status=%d response_bytes=%zu\n",
		header[0], result, result ? 0 : output_bytes);
	response[1] = result;
	response[2] = result ? 0 : output_bytes;
	response[3] = sizeof(response);
	response[4] = header[4];
	memcpy(buffer, response, sizeof(response));
	if (input) {
		explicit_bzero(input, transaction.input_bytes);
		free(input);
	}
	if (output) {
		explicit_bzero(output, output_bytes);
		free(output);
	}
	return 0;
}

void goodix_rpmb_close(struct goodix_rpmb *rpmb)
{
	if (!rpmb)
		return;
	if (rpmb->fd >= 0)
		close(rpmb->fd);
	explicit_bzero(rpmb, sizeof(*rpmb));
	free(rpmb);
}
