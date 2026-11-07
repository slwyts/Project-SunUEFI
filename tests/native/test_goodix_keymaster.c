// SPDX-License-Identifier: BSD-3-Clause
/* Compile the real client's static transport functions. Only Object's local
 * callback is used; unused device and firmware code is removed by the linker.
 */
#define main piano_goodix_client_main
#include "../../linux/userspace/fingerprint/piano-goodix.c"
#undef main

#include <assert.h>

struct reply {
	const void *request;
	size_t request_size, reported_size;
	int32_t status, transport_error;
	unsigned int calls;
};

static int32_t reply_invoke(ObjectCxt context, ObjectOp operation,
			    ObjectArg *args, ObjectCounts counts)
{
	struct reply *reply = context;

	assert(operation == 0);
	assert(counts == ObjectCounts_pack(1, 1, 0, 0));
	assert(args[0].bi.ptr == reply->request);
	assert(args[0].bi.size == reply->request_size);
	++reply->calls;
	if (reply->transport_error)
		return reply->transport_error;
	/* A malformed returned length must not itself overrun the test buffer. */
	memset(args[1].b.ptr, 0xa5, args[1].b.size);
	if (args[1].b.size >= sizeof(reply->status))
		memcpy(args[1].b.ptr, &reply->status, sizeof(reply->status));
	args[1].b.size = reply->reported_size;
	return Object_OK;
}

static void native_status(void)
{
	uint32_t request = 0x21002;
	uint8_t response[24];
	struct reply reply = {
		.request = &request, .request_size = sizeof(request),
		.reported_size = sizeof(response),
	};
	Object app = { reply_invoke, &reply };
	const int32_t statuses[] = { 0, 30000, -30 };

	for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
		size_t actual = 0;
		int32_t status = INT32_MAX;

		reply.status = statuses[i];
		assert(keymaster_native_command(app, &request, sizeof(request),
			response, sizeof(response), &actual, &status) == 0);
		assert(status == statuses[i]);
		assert(actual == sizeof(response));
		for (size_t byte = sizeof(status); byte < actual; ++byte)
			assert(response[byte] == 0xa5);
	}
	assert(reply.calls == 3);
}

static void framing_errors(void)
{
	uint32_t request = 0x220e;
	uint8_t response[16];
	struct reply reply = {
		.request = &request, .request_size = sizeof(request), .status = 12,
	};
	Object app = { reply_invoke, &reply };
	const size_t lengths[] = { 0, 3, sizeof(response) + 1 };

	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		size_t actual = SIZE_MAX;
		int32_t status = INT32_MAX;

		reply.reported_size = lengths[i];
		assert(keymaster_native_command(app, &request, sizeof(request),
			response, sizeof(response), &actual, &status) == -EMSGSIZE);
		assert(actual == 0 && status == 0);
	}
	size_t actual = SIZE_MAX;
	int32_t status = INT32_MAX;

	reply.transport_error = Object_ERROR_BUSY;
	assert(keymaster_native_command(app, &request, sizeof(request),
		response, sizeof(response), &actual, &status) == -EIO);
	assert(actual == 0 && status == 0);
}

static void strict_status(void)
{
	_Alignas(uint32_t) uint8_t request_storage[8];
	uint8_t *request = request_storage + 1;
	uint32_t opcode = 0x200;
	uint8_t response[20];
	struct reply reply = {
		.request = request, .request_size = sizeof(opcode),
		.reported_size = sizeof(response),
	};
	Object app = { reply_invoke, &reply };
	size_t actual = SIZE_MAX;

	memcpy(request, &opcode, sizeof(opcode));
	assert((uintptr_t)request % _Alignof(uint32_t) != 0);
	/* 0x200/0x207 fixed replies have no CBOR length header. Bytes after
	 * status deliberately cannot be interpreted as a bounded CBOR length.
	 */
	assert(keymaster_native_command_strict(app, request, sizeof(opcode),
		response, sizeof(response), &actual) == 0);
	assert(actual == sizeof(response));
	const int32_t statuses[] = { 12, -10020 };

	for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
		reply.status = statuses[i];
		actual = SIZE_MAX;
		assert(keymaster_native_command_strict(app, request, sizeof(opcode),
			response, sizeof(response), &actual) == -EREMOTEIO);
		assert(actual == 0);
	}
	/* Error logging must not read an opcode from an incomplete request. */
	reply.request_size = 1;
	assert(keymaster_native_command_strict(app, request, 1,
		response, sizeof(response), &actual) == -EREMOTEIO);
	assert(actual == 0);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "status"))
		native_status();
	else if (!strcmp(argv[1], "framing"))
		framing_errors();
	else {
		assert(!strcmp(argv[1], "strict"));
		strict_status();
	}
	return 0;
}
