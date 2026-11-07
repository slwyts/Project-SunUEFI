// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2026 slwyts <slwyts@foxmail.com>
 *
 * Native Piano Goodix control client. The application interface follows the
 * factory libgf_ca/QSEECom calls; transport is Qualcomm's public MinkIPC API.
 * Linux profiles have their own Gatekeeper credential and template storage;
 * factory Android credentials and fingerprint files are never opened.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "MinkCom.h"
#include <qcbor/qcbor_decode.h>
#include <qcbor/qcbor_encode.h>
#include <qcbor/qcbor_spiffy_decode.h>
#include "piano-goodix-events.h"
#include "piano-goodix-gatekeeper.h"
#include "piano-goodix-operations.h"
#include "piano-goodix-storage.h"
#include "piano-goodix.h"

#define FIRMWARE "/usr/lib/firmware/gfpia.elf"
#define FIRMWARE_LIMIT (32U * 1024U * 1024U)
#define LOADER_UID 122
#define PAYLOAD_SIZE 640
#define SHARED_SIZE 4096
#define MODEL_BYTES 0x124ff4
#define MODEL_CHUNK_BYTES 0x7cf9c
#define MODEL_PACKET_BYTES (MODEL_CHUNK_BYTES + 0x78)
static volatile sig_atomic_t interrupted;

static void stop_watching(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

struct client {
	Object root, env, loader, app, memory;
	void *shared;
	size_t shared_size;
	uint32_t architecture;
	uint32_t sequence;
	int controller;
	int sensor_started;
};

struct goodix_request {
	uint64_t buffer;
	uint32_t sequence;
	uint32_t route;
	uint64_t milliseconds;
	uint32_t log_level;
	uint32_t reserved;
	uint32_t command;
	uint32_t payload_size;
	uint8_t padding[88];
};
_Static_assert(sizeof(struct goodix_request) == 128, "Goodix request ABI");

struct sensor_geometry {
	uint32_t rows, columns, nav_rows, nav_columns;
	uint32_t chip_type, sensor_type;
	uint32_t max_templates;
	uint32_t model_required;
	uint8_t anti_spoof_required;
	uint8_t anti_spoof_variant;
	uint8_t enrollment_extended, interaction_policy;
	int models_loaded;
};

struct template_entry {
	uint32_t group, id;
};

struct template_list {
	struct template_entry entries[10];
	size_t count;
};

static int sensor_command(struct client *client, uint32_t command, size_t size);

static int invoke(Object object, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	int32_t result = Object_invoke(object, op, args, counts);

	if (result) {
		fprintf(stderr, "QTEE operation %u failed: %d\n", (unsigned int)op,
			result);
		return -EIO;
	}
	return 0;
}

static int close_client(struct client *client)
{
	int result = 0;

	if (client->sensor_started && client->shared) {
		memset(client->shared, 0, client->shared_size);
		result = sensor_command(client, 1002, 0x6c);
	}
	if (!Object_isNull(client->app)) {
		int shutdown = invoke(client->app, 2, NULL, ObjectCounts_pack(0, 0, 0, 0));
		if (!result)
			result = shutdown;
	}
	if (client->controller >= 0)
		close(client->controller);
	if (client->shared)
		explicit_bzero(client->shared, client->shared_size);
	Object_RELEASE_IF(client->memory);
	client->memory = Object_NULL;
	Object_RELEASE_IF(client->app);
	Object_RELEASE_IF(client->loader);
	Object_RELEASE_IF(client->env);
	Object_RELEASE_IF(client->root);
	return result;
}

static int open_controller(void)
{
	glob_t matches = {0};
	int fd = -ENODEV;

	if (glob("/sys/class/uio/uio*/name", 0, NULL, &matches))
		return -ENODEV;
	for (size_t i = 0; i < matches.gl_pathc; i++) {
		char name[64], path[128];
		FILE *stream = fopen(matches.gl_pathv[i], "r");
		unsigned int index;

		if (!stream)
			continue;
		char *read = fgets(name, sizeof(name), stream);
		fclose(stream);
		if (!read || strcmp(name, "piano-goodix\n"))
			continue;
		if (sscanf(matches.gl_pathv[i], "/sys/class/uio/uio%u/name", &index) != 1)
			continue;
		snprintf(path, sizeof(path), "/dev/uio%u", index);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			fd = -errno;
		break;
	}
	globfree(&matches);
	return fd;
}

static int load_application(struct client *client)
{
	struct stat st;
	Object image = Object_NULL;
	void *address = NULL;
	size_t capacity = 0;
	const char name[] = "gfpia";
	int fd = open(FIRMWARE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	int result = -EINVAL;

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 64 ||
	    st.st_size > FIRMWARE_LIMIT)
		goto done;
	if (MinkCom_getMemoryObject(client->root, st.st_size, &image) ||
	    MinkCom_getMemoryObjectInfo(image, &address, &capacity) ||
	    capacity < (size_t)st.st_size) {
		result = -ENOMEM;
		goto done;
	}
	for (size_t offset = 0; offset < (size_t)st.st_size;) {
		ssize_t count = read(fd, (uint8_t *)address + offset, st.st_size - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			result = -EIO;
			goto done;
		}
		offset += count;
	}
	if (memcmp(address, "\177ELF", 4))
		goto done;
	ObjectArg load[] = {
		{ .bi = {name, sizeof(name) - 1} },
		{ .o = image },
		{ .o = Object_NULL },
	};
	result = invoke(client->loader, 0, load, ObjectCounts_pack(1, 0, 1, 1));
	if (result)
		goto done;
	client->app = load[2].o;
	/* The signed container can be ELF32 while the command interface is
	 * 64-bit. Ask the loader instead of inferring this from the file.
	 */
	ObjectArg lookup[] = {
		{ .bi = {name, sizeof(name) - 1} },
		{ .b = {&client->architecture, sizeof(client->architecture)} },
		{ .o = Object_NULL },
	};
	result = invoke(client->loader, 2, lookup, ObjectCounts_pack(1, 1, 0, 1));
	Object_RELEASE_IF(lookup[2].o);
	if (!result && client->architecture != 1 && client->architecture != 2)
		result = -EOPNOTSUPP;
done:
	Object_RELEASE_IF(image);
	close(fd);
	return result;
}

static int open_client(struct client *client)
{
	uint32_t uid = LOADER_UID;
	int result;

	memset(client, 0, sizeof(*client));
	client->controller = -1;
	if (MinkCom_getRootEnvObject(&client->root) ||
	    MinkCom_getClientEnvObject(client->root, &client->env))
		return -EIO;
	ObjectArg args[] = {
		{ .bi = {&uid, sizeof(uid)} },
		{ .o = Object_NULL },
	};
	result = invoke(client->env, 0, args, ObjectCounts_pack(1, 0, 0, 1));
	if (result)
		return result;
	client->loader = args[1].o;
	result = load_application(client);
	if (result)
		return result;
	/* Keep the factory ordering: load signed TA, then power/reset sensor. */
	client->controller = open_controller();
	if (client->controller < 0)
		return client->controller;
	if (MinkCom_getMemoryObject(client->root, SHARED_SIZE, &client->memory) ||
	    MinkCom_getMemoryObjectInfo(client->memory, &client->shared,
				       &client->shared_size))
		return -ENOMEM;
	return client->shared_size >= SHARED_SIZE ? 0 : -EMSGSIZE;
}

static int application_command_status(struct client *client, uint32_t route,
				      uint32_t command, size_t size,
				      int32_t *application_status)
{
	struct goodix_request request = {0};
	int32_t response[16] = {0};
	uint32_t offsets[4] = {0};
	uint32_t is_64_bit = client->architecture == 2;
	time_t now = time(NULL);
	struct tm local;
	int result;

	if (!size || size > client->shared_size)
		return -EMSGSIZE;
	localtime_r(&now, &local);
	request.sequence = ++client->sequence;
	request.route = route;
	request.command = command;
	request.payload_size = size;
	request.milliseconds = (uint64_t)(local.tm_hour * 3600 +
		local.tm_min * 60 + local.tm_sec) * 1000;
	ObjectArg args[] = {
		{ .bi = {&request, sizeof(request)} },
		{ .bi = {response, sizeof(response)} },
		{ .bi = {offsets, sizeof(offsets)} },
		{ .bi = {&is_64_bit, sizeof(is_64_bit)} },
		{ .b = {&request, sizeof(request)} },
		{ .b = {response, sizeof(response)} },
		{ .o = client->memory },
		{ .o = Object_NULL },
		{ .o = Object_NULL },
		{ .o = Object_NULL },
	};
	result = invoke(client->app, 0, args, ObjectCounts_pack(4, 2, 4, 0));
	if (result)
		return result;
	if (args[5].b.size < 2 * sizeof(int32_t))
		return -EMSGSIZE;
	if (response[1]) {
		fprintf(stderr, "Goodix command %u: application=%d buffer=%d\n",
			command, response[0], response[1]);
		return -EREMOTEIO;
	}
	*application_status = response[0];
	return 0;
}

static int application_command(struct client *client, uint32_t route,
			       uint32_t command, size_t size)
{
	int32_t status = 0;
	int result = application_command_status(client, route, command, size, &status);

	if (!result && status) {
		fprintf(stderr, "Goodix command %u: application=%d\n", command, status);
		result = -EREMOTEIO;
	}
	return result;
}

static int sensor_command_status(void *context, uint32_t command, size_t size,
				 int32_t *application_status)
{
	struct client *client = context;
	time_t now = time(NULL);
	struct tm local;

	if (size < 0x6c || size > client->shared_size)
		return -EMSGSIZE;
	memcpy((uint8_t *)client->shared + 0x44, &client->sequence,
	       sizeof(client->sequence));
	localtime_r(&now, &local);
	/* Descriptive factory metadata, unrelated to authentication tokens. */
	strftime((char *)client->shared + 0x4c, 0x20, "%Y-%m-%d %H:%M:%S", &local);
	return application_command_status(client, 1, command, size, application_status);
}

static int sensor_command(struct client *client, uint32_t command, size_t size)
{
	int32_t status = 0;
	int result = sensor_command_status(client, command, size, &status);

	if (!result && status) {
		fprintf(stderr, "Goodix command %u: application=%d\n", command, status);
		result = -EREMOTEIO;
	}
	return result;
}

static void clear_sensitive(void *buffer, size_t size)
{
	volatile uint8_t *bytes = buffer;

	while (size--)
		*bytes++ = 0;
}

static int keymaster_native_command(Object app, const void *request, size_t size,
				    uint8_t *response, size_t capacity, size_t *actual,
				    int32_t *status)
{
	ObjectArg args[] = {
		{ .bi = {request, size} }, { .b = {response, capacity} },
	};

	/* A native TA result may be a positive retry code. Keep it separate
	 * from transport/framing errors and preserve its opaque reply payload.
	 */
	*actual = 0;
	*status = 0;
	int result = invoke(app, 0, args, ObjectCounts_pack(1, 1, 0, 0));

	if (result)
		return result;
	if (args[1].b.size < sizeof(*status) || args[1].b.size > capacity)
		return -EMSGSIZE;
	memcpy(status, response, sizeof(*status));
	*actual = args[1].b.size;
	return 0;
}

/* Version negotiation and HMAC sharing require success, unlike a caller
 * interpreting a native protocol's retry result and accompanying payload.
 */
static int keymaster_native_command_strict(Object app, const void *request, size_t size,
					   uint8_t *response, size_t capacity, size_t *actual)
{
	int32_t status;
	int result = keymaster_native_command(app, request, size, response, capacity,
					      actual, &status);

	if (result)
		return result;
	if (status) {
		if (size >= sizeof(uint32_t)) {
			uint32_t opcode;

			memcpy(&opcode, request, sizeof(opcode));
			fprintf(stderr, "Keymaster native command %#x: status=%d\n", opcode, status);
		} else {
			fprintf(stderr, "Keymaster native command: status=%d\n", status);
		}
		*actual = 0;
		return -EREMOTEIO;
	}
	return 0;
}

static int open_keymaster(struct client *client, Object *output)
{
	const char name[] = "keymaster64";
	Object opener = Object_NULL, environment = Object_NULL, app = Object_NULL;
	uint32_t uid = 151, version = 0x200;
	uint32_t handshake[] = {0x207, 4, 5, 4, 5, 0};
	uint8_t response[512] = {0};
	size_t actual;
	int result;
	ObjectArg service[] = {
		{ .bi = {&uid, sizeof(uid)} }, { .o = Object_NULL },
	};

	/* Native Keymaster interface recovered from this ROM's KeymasterUtils:
	 * client-env service 151 opens the named app environment; service 150
	 * inside it exposes command input/output buffers without QSEECOMPAT.
	 */
	result = invoke(client->env, 0, service, ObjectCounts_pack(1, 0, 0, 1));
	if (result)
		goto done;
	opener = service[1].o;
	ObjectArg open[] = {
		{ .bi = {name, sizeof(name) - 1} }, { .o = Object_NULL },
	};
	result = invoke(opener, 0, open, ObjectCounts_pack(1, 0, 0, 1));
	if (result)
		goto done;
	environment = open[1].o;
	uid = 150;
	service[1].o = Object_NULL;
	result = invoke(environment, 0, service, ObjectCounts_pack(1, 0, 0, 1));
	if (result)
		goto done;
	app = service[1].o;
	result = keymaster_native_command_strict(app, &version, sizeof(version),
					 response, sizeof(response), &actual);
	if (!result)
		result = keymaster_native_command_strict(app, handshake, sizeof(handshake),
						 response, sizeof(response), &actual);
	if (!result) {
		*output = app;
		app = Object_NULL;
	}
done:
	clear_sensitive(response, sizeof(response));
	Object_RELEASE_IF(app);
	Object_RELEASE_IF(environment);
	Object_RELEASE_IF(opener);
	return result;
}

static int keymaster_sharing_init(struct client *client)
{
	Object app = Object_NULL;
	uint32_t length, get = 0x220e, compute = 0x220f;
	uint8_t response[512] = {0}, request[128] = {0}, parameters[64] = {0};
	size_t actual = 0;
	UsefulBufC seed = NULLUsefulBufC, nonce = NULLUsefulBufC, encoded;
	QCBORDecodeContext decoder;
	QCBOREncodeContext encoder;
	int result = open_keymaster(client, &app);

	if (!result)
		result = keymaster_native_command_strict(app, &get, sizeof(get),
						 response, sizeof(response), &actual);
	if (result)
		goto done;
	if (actual < 8) {
		result = -EMSGSIZE;
		goto done;
	}
	memcpy(&length, response + 4, sizeof(length));
	if (length > actual - 8) {
		result = -EMSGSIZE;
		goto done;
	}
	QCBORDecode_Init(&decoder, (UsefulBufC){response + 8, length},
			 QCBOR_DECODE_MODE_NORMAL);
	QCBORDecode_EnterMap(&decoder, NULL);
	QCBORDecode_GetByteStringInMapN(&decoder, 41, &seed);
	QCBORDecode_GetByteStringInMapN(&decoder, 42, &nonce);
	QCBORDecode_ExitMap(&decoder);
	if (QCBORDecode_Finish(&decoder) || seed.len != 32 || nonce.len != 32) {
		result = -EBADMSG;
		goto done;
	}
	memcpy(parameters, seed.ptr, 32);
	memcpy(parameters + 32, nonce.ptr, 32);
	clear_sensitive(response, sizeof(response));
	memcpy(request, &compute, sizeof(compute));
	QCBOREncode_Init(&encoder, (UsefulBuf){request + 4, sizeof(request) - 4});
	QCBOREncode_OpenMap(&encoder);
	QCBOREncode_AddBytesToMapN(&encoder, 43, (UsefulBufC){parameters, sizeof(parameters)});
	QCBOREncode_CloseMap(&encoder);
	if (QCBOREncode_Finish(&encoder, &encoded)) {
		result = -EMSGSIZE;
		goto done;
	}
	/* Preserve the exact CBOR length; padding is not part of this protocol. */
	result = keymaster_native_command_strict(app, request, encoded.len + 4,
					 response, sizeof(response), &actual);
done:
	clear_sensitive(response, sizeof(response));
	clear_sensitive(request, sizeof(request));
	clear_sensitive(parameters, sizeof(parameters));
	Object_RELEASE_IF(app);
	return result;
}

static int keymaster_binding(Object app, uint32_t wide, uint8_t response[256])
{
	uint32_t request[] = {0x205, 2}, offsets[4] = {0};
	int32_t status;
	ObjectArg args[] = {
		{ .bi = {request, sizeof(request)} },
		{ .bi = {response, 256} },
		{ .bi = {offsets, sizeof(offsets)} },
		{ .bi = {&wide, sizeof(wide)} },
		{ .b = {request, sizeof(request)} }, { .b = {response, 256} },
		{ .o = Object_NULL }, { .o = Object_NULL },
		{ .o = Object_NULL }, { .o = Object_NULL },
	};
	int result = invoke(app, 0, args, ObjectCounts_pack(4, 2, 4, 0));

	if (result)
		return result;
	if (args[5].b.size != 256)
		return -EMSGSIZE;
	memcpy(&status, response, sizeof(status));
	return status;
}

static int prepare_authentication(struct client *client)
{
	const char name[] = "keymaster64";
	uint32_t architecture = 0, offset, length;
	uint8_t binding[256] = {0};
	Object app = Object_NULL;
	ObjectArg lookup[] = {
		{ .bi = {name, sizeof(name) - 1} },
		{ .b = {&architecture, sizeof(architecture)} },
		{ .o = Object_NULL },
	};
	int result = invoke(client->loader, 2, lookup, ObjectCounts_pack(1, 1, 0, 1));

	if (result)
		goto done;
	app = lookup[2].o;
	if (architecture != 1 && architecture != 2) {
		result = -EOPNOTSUPP;
		goto done;
	}
	/* Keep Goodix loaded before requesting a blob encrypted for its TA.
	 * Initialize this boot's sharing key only if Keymaster reports it absent,
	 * so another Linux client cannot accidentally rekey an active session.
	 */
	result = keymaster_binding(app, architecture == 2, binding);
	if (result == -10020) {
		result = keymaster_sharing_init(client);
		if (!result)
			result = keymaster_binding(app, architecture == 2, binding);
	}
	if (result) {
		fprintf(stderr, "Keymaster binding request: status=%d\n", result);
		result = -EREMOTEIO;
		goto done;
	}
	memcpy(&offset, binding + 4, sizeof(offset));
	memcpy(&length, binding + 8, sizeof(length));
	if (offset < 12 || offset > sizeof(binding) || !length ||
	    length > sizeof(binding) - offset) {
		result = -EBADMSG;
		goto done;
	}
	memcpy(client->shared, binding, sizeof(binding));
	result = application_command(client, 2, 109, sizeof(binding));
done:
	clear_sensitive(binding, sizeof(binding));
	if (client->shared)
		clear_sensitive(client->shared, client->shared_size);
	Object_RELEASE_IF(app);
	return result;
}

static int device_info(struct client *client, uint32_t *sensor_type)
{
	for (uint32_t candidate = 4; candidate; candidate--) {
		int result;

		if (candidate != 4) {
			close(client->controller);
			usleep(5000);
			client->controller = open_controller();
			if (client->controller < 0)
				return client->controller;
		}
		memset(client->shared, 0, client->shared_size);
		((uint8_t *)client->shared)[0x278] = candidate;
		result = sensor_command(client, 1000, PAYLOAD_SIZE);
		if (result && result != -EREMOTEIO)
			return result;
		if (result)
			continue;
		memcpy(sensor_type, (uint8_t *)client->shared + 0x74,
		       sizeof(*sensor_type));
		return *sensor_type ? 0 : -ENODEV;
	}
	return -ENODEV;
}

static int read_model(const char *name, uint8_t *destination, size_t size)
{
	char path[256];
	struct stat st;
	size_t offset = 0;
	int result = 0;

	snprintf(path, sizeof(path), "/usr/lib/firmware/%s", name);
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (off_t)size) {
		result = -EMSGSIZE;
		goto done;
	}
	while (offset < size) {
		ssize_t count = read(fd, destination + offset, size - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			result = -EIO;
			break;
		}
		offset += count;
	}
done:
	close(fd);
	return result;
}

static int send_model(struct client *client, const uint8_t *model)
{
	void *shared;
	size_t capacity;

	/* The factory sends three bounded pieces. Only the current piece is
	 * shared with QTEE; the assembled model remains ordinary CPU memory.
	 */
	Object_RELEASE_IF(client->memory);
	client->memory = Object_NULL;
	client->shared = NULL;
	client->shared_size = 0;
	if (MinkCom_getMemoryObject(client->root, MODEL_PACKET_BYTES, &client->memory) ||
	    MinkCom_getMemoryObjectInfo(client->memory, &shared, &capacity) ||
	    capacity < MODEL_PACKET_BYTES)
		return -ENOMEM;
	client->shared = shared;
	client->shared_size = capacity;
	for (size_t offset = 0, part = 0; offset < MODEL_BYTES;
	     offset += MODEL_CHUNK_BYTES, part++) {
		uint8_t *payload = shared;
		size_t bytes = MODEL_BYTES - offset;
		uint32_t total = MODEL_BYTES, command = 1111;
		int result;

		if (bytes > MODEL_CHUNK_BYTES)
			bytes = MODEL_CHUNK_BYTES;
		memset(payload, 0, capacity);
		memcpy(payload + 0x6c, model + offset, bytes);
		payload[0x7d008] = part;
		memcpy(payload + 0x7d00c, &total, sizeof(total));
		memcpy(payload + 0x7d010, &command, sizeof(command));
		result = sensor_command(client, 1113, MODEL_PACKET_BYTES);
		if (result)
			return result;
	}
	return 0;
}

static int load_models(struct client *client, const struct sensor_geometry *geometry)
{
	int result = 0;
	uint8_t *model;

	if (!geometry->model_required && !geometry->anti_spoof_required)
		return 0;
	/* Fixed model selection recovered for this actual chip. Do not use a
	 * similarly named model from another Goodix sensor.
	 */
	if (geometry->chip_type != 49)
		return -EOPNOTSUPP;
	model = calloc(1, MODEL_BYTES);
	if (!model)
		return -ENOMEM;
	if (geometry->anti_spoof_required) {
		uint32_t size = 308544;
		const char *name = geometry->anti_spoof_variant ?
			"piano-goodix-cap-t.bin" : "piano-goodix-cap-s.bin";

		result = read_model(name, model + 0x70, size);
		if (result)
			goto done;
		memcpy(model + 0x1248b8, &size, sizeof(size));
		result = send_model(client, model);
		if (result)
			goto done;
	}
	if (geometry->model_required) {
		uint32_t elements = 74530;

		memset(model, 0, MODEL_BYTES);
		model[0x6c] = 1;
		result = read_model("piano-goodix-dubai-u4.bin", model + 0x70,
				    elements * sizeof(uint32_t));
		if (result)
			goto done;
		memcpy(model + 0x124ff0, &elements, sizeof(elements));
		result = send_model(client, model);
	}
done:
	free(model);
	return result;
}

static int initialize_sensor(struct client *client, struct sensor_geometry *geometry)
{
	int result;
	uint32_t chip_type, sensor_type;
	uint8_t *payload = client->shared;

	if (geometry->sensor_type != 5)
		return -EOPNOTSUPP;
	memcpy(&geometry->chip_type, payload + 0x70, sizeof(uint32_t));
	memcpy(&geometry->max_templates, payload + 0x7c, sizeof(uint32_t));
	memcpy(&geometry->model_required, payload + 0x1ec, sizeof(uint32_t));
	geometry->anti_spoof_required = payload[0x20d];
	geometry->anti_spoof_variant = payload[0x212];
	/* The factory copies device-info +0x70 into g_hal_config. Preserve the
	 * actual extended-enrollment and interaction bytes before INIT replaces it.
	 */
	geometry->enrollment_extended = payload[0xd3];
	geometry->interaction_policy = payload[0x20c];
	/* Milan initialization, recovered from the factory HAL's sensor-type-5
	 * function table. Empty OTP cache means read calibration from the real
	 * sensor. No Android storage, user template or enrollment command is used.
	 */
	memset(payload, 0, client->shared_size);
	result = sensor_command(client, 1001, 0x168);
	if (result)
		return result;
	client->sensor_started = 1;
	memcpy(&geometry->rows, payload + 0x70, sizeof(uint32_t));
	memcpy(&geometry->columns, payload + 0x74, sizeof(uint32_t));
	memcpy(&geometry->nav_rows, payload + 0x80, sizeof(uint32_t));
	memcpy(&geometry->nav_columns, payload + 0x84, sizeof(uint32_t));
	memcpy(&chip_type, payload + 0x15c, sizeof(uint32_t));
	memcpy(&sensor_type, payload + 0x160, sizeof(uint32_t));
	/* As in the factory HAL, zero here means retain the device-info value. */
	if (chip_type)
		geometry->chip_type = chip_type;
	if (sensor_type)
		geometry->sensor_type = sensor_type;
	if (!geometry->rows || !geometry->columns ||
	    geometry->rows > 4096 || geometry->columns > 4096)
		return -EPROTO;
	result = load_models(client, geometry);
	if (result)
		return result;
	geometry->models_loaded = 1;
	memset(client->shared, 0, client->shared_size);
	/* GF_CMD_INIT_FINISH: no debug dumping, matching the stock default. */
	result = sensor_command(client, 1005, 0x74);
	if (result)
		return result;
	memset(client->shared, 0, client->shared_size);
	/* The factory's 1109 packet copies g_hal_config +0x19c, saved from
	 * device-info +0x20c before INIT replaced the shared payload.
	 */
	((uint8_t *)client->shared)[0x6c] = geometry->interaction_policy;
	return sensor_command(client, 1109, 0xc0);
}

static int enumerate_templates(struct client *client, uint32_t maximum,
			       struct template_list *list)
{
	uint8_t *payload = client->shared;
	int result;

	if (!maximum || maximum > sizeof(list->entries) / sizeof(list->entries[0]))
		return -EPROTO;
	memset(payload, 0, client->shared_size);
	result = sensor_command(client, 1015, 0xc0);
	if (result)
		return result;
	for (size_t i = 0; i < maximum; i++) {
		struct template_entry entry;

		memcpy(&entry.group, payload + 0x70 + i * 4, sizeof(uint32_t));
		memcpy(&entry.id, payload + 0x98 + i * 4, sizeof(uint32_t));
		if (entry.id)
			list->entries[list->count++] = entry;
	}
	return 0;
}

static int watch_events(struct client *client, unsigned int seconds)
{
	struct timespec start, now;
	struct pollfd event = { .fd = client->controller, .events = POLLIN };
	struct goodix_events events = {
		.command = sensor_command_status, .context = client,
		.buffer = client->shared, .buffer_size = client->shared_size,
	};
	struct sigaction action = { .sa_handler = stop_watching }, previous_int, previous_term;
	int result, handlers = 0, stopped;
	interrupted = 0;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &previous_int)) {
		result = -errno;
		goto cancel;
	}
	handlers = 1;
	if (sigaction(SIGTERM, &action, &previous_term)) {
		result = -errno;
		goto cancel;
	}
	handlers = 2;
	result = goodix_events_start(&events);
	if (result || interrupted)
		goto cancel;
	clock_gettime(CLOCK_MONOTONIC, &start);
	fprintf(stderr, "Sensor ready; watching interrupts for %u seconds.\n", seconds);
	while (!interrupted) {
		uint32_t counter, enable = 1;
		clock_gettime(CLOCK_MONOTONIC, &now);
		int64_t remaining = (int64_t)seconds * 1000 -
			((now.tv_sec - start.tv_sec) * 1000 +
			 (now.tv_nsec - start.tv_nsec) / 1000000);
		if (remaining <= 0)
			break;
		int ready = poll(&event, 1, remaining > 500 ? 500 : remaining);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			result = -errno;
			break;
		}
		if (!ready)
			continue;
		if (!(event.revents & POLLIN) ||
		    read(client->controller, &counter, sizeof(counter)) != sizeof(counter)) {
			result = -EIO;
			break;
		}
		/* Drain the TA handshake before re-enabling its IRQ. The deadline
		 * and cancellation are checked between calls, including phases 4/5.
		 * Never print image buffers, template identities or authentication tokens.
		 */
		do {
			struct goodix_sensor_event sample;

			result = goodix_events_read(&events, &sample);
			if (result < 0) {
				if (result == -EOPNOTSUPP)
					fputs("Sensor requested an unsupported exchange; cancelling the event watch.\n",
					      stderr);
				break;
			}
			if (printf("{\"irq_count\":%u,\"sensor_events\":%u,\"operation\":%u,"
			       "\"phase\":%u,\"application_status\":%d}\n",
			       counter, sample.mask, sample.operation, sample.phase,
			       sample.application_status) < 0 || fflush(stdout) == EOF) {
				result = -errno;
				break;
			}
			clock_gettime(CLOCK_MONOTONIC, &now);
			remaining = (int64_t)seconds * 1000 -
				((now.tv_sec - start.tv_sec) * 1000 +
				 (now.tv_nsec - start.tv_nsec) / 1000000);
		} while (result == 1 && remaining > 0 && !interrupted);
		if (result || remaining <= 0 || interrupted)
			break;
		if (write(client->controller, &enable, sizeof(enable)) != sizeof(enable)) {
			result = -EIO;
			break;
		}
	}
	/* A handshake interrupted by the deadline is a normal end to the watch.
	 * The explicit cancel below must succeed in either case.
	 */
	if (result == 1)
		result = 0;
cancel:
	stopped = goodix_events_cancel(&events);
	if (handlers == 2)
		sigaction(SIGTERM, &previous_term, NULL);
	if (handlers)
		sigaction(SIGINT, &previous_int, NULL);
	return result ? result : stopped;
}

/* The profile contains Linux finger names and opaque IDs, never templates or
 * authentication tokens. A pending ID is written before 1012 so interruption
 * between the secure save and local commit can be recovered in this group.
 */
struct linux_profile {
	uint32_t linux_uid, group;
	uint64_t sid;
	uint32_t pending_finger, pending_id;
	uint32_t templates[10];
};

struct profile_session {
	struct client client;
	struct sensor_geometry geometry;
	struct goodix_storage *storage;
	struct linux_profile profile;
	Object keymaster;
	int directory, gatekeeper_directory, hardware_lock;
	char fpdata[128];
};

#define PROFILE_RECORD_BYTES 72U
static const uint8_t profile_magic[8] = {'P', 'I', 'A', 'N', 'O', 'F', 'P', 1};

static int private_directory(int parent, const char *name, int create)
{
	struct stat st;

	if (create) {
		if (mkdirat(parent, name, 0700)) {
			if (errno != EEXIST)
				return -errno;
		} else if (fsync(parent))
			return -errno;
	}
	int fd = openat(parent, name, O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) {
		close(fd);
		return -EACCES;
	}
	return fd;
}

static uint32_t profile_word(const uint8_t *record, size_t offset)
{
	uint32_t value;

	memcpy(&value, record + offset, sizeof(value));
	return le32toh(value);
}

static void profile_put_word(uint8_t *record, size_t offset, uint32_t value)
{
	value = htole32(value);
	memcpy(record + offset, &value, sizeof(value));
}

static int profile_load(int directory, struct linux_profile *profile)
{
	uint8_t record[PROFILE_RECORD_BYTES];
	struct stat st;
	uint64_t sid;
	int fd = openat(directory, "profile", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	int result = 0;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
	    st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 ||
	    st.st_size != sizeof(record)) {
		result = -EACCES;
		goto done;
	}
	ssize_t bytes;
	do {
		bytes = read(fd, record, sizeof(record));
	} while (bytes < 0 && errno == EINTR);
	if (bytes != sizeof(record)) {
		result = bytes < 0 ? -errno : -EIO;
		goto done;
	}
	if (memcmp(record, profile_magic, sizeof(profile_magic)) ||
	    profile_word(record, 8) != profile->linux_uid ||
	    profile_word(record, 12) != profile->group) {
		result = -EBADMSG;
		goto done;
	}
	memcpy(&sid, record + 16, sizeof(sid));
	profile->sid = le64toh(sid);
	profile->pending_finger = profile_word(record, 24);
	profile->pending_id = profile_word(record, 28);
	if (profile->pending_finger > 10 ||
	    (!!profile->pending_finger != !!profile->pending_id)) {
		result = -EBADMSG;
		goto done;
	}
	for (size_t i = 0; i < 10; ++i) {
		profile->templates[i] = profile_word(record, 32 + 4 * i);
		if ((profile->templates[i] || profile->pending_id) && !profile->sid) {
			result = -EBADMSG;
			goto done;
		}
		for (size_t j = 0; j < i; ++j)
			if (profile->templates[i] && profile->templates[i] == profile->templates[j]) {
				result = -EBADMSG;
				goto done;
			}
		if (profile->pending_id && profile->templates[i] == profile->pending_id &&
		    profile->pending_finger != i + 1) {
			result = -EBADMSG;
			goto done;
		}
	}
	if (profile->pending_finger && profile->templates[profile->pending_finger - 1] &&
	    profile->templates[profile->pending_finger - 1] != profile->pending_id)
		result = -EBADMSG;
done:
	close(fd);
	return result;
}

static int profile_save(int directory, const struct linux_profile *profile)
{
	uint8_t record[PROFILE_RECORD_BYTES] = {0};
	uint64_t suffix, sid = htole64(profile->sid);
	char temporary[48];
	int result = 0;

	memcpy(record, profile_magic, sizeof(profile_magic));
	profile_put_word(record, 8, profile->linux_uid);
	profile_put_word(record, 12, profile->group);
	memcpy(record + 16, &sid, sizeof(sid));
	profile_put_word(record, 24, profile->pending_finger);
	profile_put_word(record, 28, profile->pending_id);
	for (size_t i = 0; i < 10; ++i)
		profile_put_word(record, 32 + 4 * i, profile->templates[i]);
	if (getrandom(&suffix, sizeof(suffix), 0) != sizeof(suffix))
		return -EIO;
	snprintf(temporary, sizeof(temporary), ".profile-%016llx",
		 (unsigned long long)suffix);
	int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
				       O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;
	for (size_t offset = 0; offset < sizeof(record);) {
		ssize_t bytes = write(fd, record + offset, sizeof(record) - offset);
		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0) {
			result = bytes < 0 ? -errno : -EIO;
			break;
		}
		offset += bytes;
	}
	if (!result && fsync(fd))
		result = -errno;
	close(fd);
	if (!result && renameat(directory, temporary, directory, "profile"))
		result = -errno;
	if (!result && fsync(directory))
		result = -errno;
	if (result)
		unlinkat(directory, temporary, 0);
	return result;
}

static int profile_open_files(struct profile_session *session, int create)
{
	int directory = session->directory;
	int result = profile_load(directory, &session->profile);
	int missing = result == -ENOENT;

	if (missing)
		result = create ? 0 : -EUCLEAN;
	if (result)
		return result;
	session->gatekeeper_directory = private_directory(directory, "gatekeeper", create);
	if (session->gatekeeper_directory < 0)
		return session->gatekeeper_directory == -ENOENT ? -EUCLEAN :
			session->gatekeeper_directory;
	int fpdata = private_directory(directory, "fpdata", create);
	if (fpdata < 0)
		return fpdata == -ENOENT ? -EUCLEAN : fpdata;
	close(fpdata);

	/* Publish an empty profile before any TA operation. A later initialization
	 * or Gatekeeper failure must not leave a UID that breaks storage listing.
	 * Existing identities, templates and pending saves remain untouched.
	 */
	return missing ? profile_save(directory, &session->profile) : 0;
}

static int profile_open(struct profile_session *session, uint32_t linux_uid, int create)
{
	char name[16];
	int base, directory, result;

	memset(session, 0, sizeof(*session));
	session->directory = session->gatekeeper_directory = session->hardware_lock =
		session->client.controller = -1;
	session->profile.linux_uid = linux_uid;
	result = goodix_gatekeeper_uid(linux_uid, &session->profile.group);
	if (result)
		return result;
	if (geteuid())
		return -EACCES;
	int parent = open("/var/lib", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent < 0)
		return -errno;
	base = private_directory(parent, "piano-goodix", create);
	close(parent);
	if (base < 0)
		return base;
	/* One physical sensor and one pair of FS listeners serve every user.
	 * Lock their existing parent directory before registering any listener.
	 */
	session->hardware_lock = base;
	if (flock(base, LOCK_EX | LOCK_NB))
		return errno == EWOULDBLOCK ? -EBUSY : -errno;
	snprintf(name, sizeof(name), "%u", linux_uid);
	directory = private_directory(base, name, create);
	if (directory < 0)
		return directory;
	session->directory = directory;
	snprintf(session->fpdata, sizeof(session->fpdata), "/var/lib/piano-goodix/%u/fpdata",
		 linux_uid);
	return profile_open_files(session, create);
}

static int session_close(struct profile_session *session, int result)
{
	Object_RELEASE_IF(session->keymaster);
	int closed = close_client(&session->client);
	goodix_storage_close(session->storage);
	if (session->gatekeeper_directory >= 0)
		close(session->gatekeeper_directory);
	if (session->directory >= 0)
		close(session->directory);
	if (session->hardware_lock >= 0)
		close(session->hardware_lock);
	clear_sensitive(&session->profile, sizeof(session->profile));
	return result ? result : closed;
}

static int session_start(struct profile_session *session, int gatekeeper)
{
	struct client *client = &session->client;
	int result = goodix_storage_open_profile(session->fpdata, gatekeeper, &session->storage);

	if (!result)
		result = open_client(client);
	if (!result)
		result = device_info(client, &session->geometry.sensor_type);
	if (!result)
		result = initialize_sensor(client, &session->geometry);
	if (!result)
		result = prepare_authentication(client);
	if (result)
		return result;
	memset(client->shared, 0, client->shared_size);
	memcpy((uint8_t *)client->shared + 0x6c, &session->profile.group, sizeof(uint32_t));
	const char path[] = "/data/vendor_de/0/fpdata";
	memcpy((uint8_t *)client->shared + 0x70, path, sizeof(path));
	return sensor_command(client, 1014, 0x270);
}

static int profile_contains(const struct template_list *list, uint32_t group, uint32_t id)
{
	for (size_t i = 0; i < list->count; ++i)
		if (list->entries[i].group == group && list->entries[i].id == id)
			return 1;
	return 0;
}

static int profile_reconcile_list(struct profile_session *session, const struct template_list *list)
{
	struct linux_profile *profile = &session->profile;
	int changed = 0;

	/* Unknown TA IDs are not exposed or removed. Only recover the exact ID
	 * persisted before our own save, and prune stale Linux-owned mappings.
	 */
	if (profile->pending_finger) {
		if (profile_contains(list, profile->group, profile->pending_id))
			profile->templates[profile->pending_finger - 1] = profile->pending_id;
		profile->pending_id = profile->pending_finger = 0;
		changed = 1;
	}
	for (size_t i = 0; i < 10; ++i)
		if (profile->templates[i] &&
		    !profile_contains(list, profile->group, profile->templates[i])) {
			profile->templates[i] = 0;
			changed = 1;
		}
	return changed ? profile_save(session->directory, profile) : 0;
}

static int profile_reconcile(struct profile_session *session)
{
	struct template_list list = {0};
	int result = enumerate_templates(&session->client, session->geometry.max_templates, &list);

	return result ? result : profile_reconcile_list(session, &list);
}

static int grow_operation_buffer(struct client *client, size_t required)
{
	Object memory = Object_NULL;
	void *shared;
	size_t capacity;

	if (client->shared_size >= required)
		return 0;
	if (MinkCom_getMemoryObject(client->root, required, &memory) ||
	    MinkCom_getMemoryObjectInfo(memory, &shared, &capacity) || capacity < required) {
		Object_RELEASE_IF(memory);
		return -ENOMEM;
	}
	clear_sensitive(client->shared, client->shared_size);
	Object_RELEASE_IF(client->memory);
	client->memory = memory;
	client->shared = shared;
	client->shared_size = capacity;
	return 0;
}

static int gatekeeper_command(void *context, const void *request, size_t request_bytes,
			      uint8_t *response, size_t capacity, size_t *actual, int32_t *status)
{
	struct profile_session *session = context;
	uint32_t opcode;
	uint64_t uid;
	QCBORDecodeContext decoder;

	if (request_bytes < sizeof(opcode))
		return -EMSGSIZE;
	memcpy(&opcode, request, sizeof(opcode));
	opcode = le32toh(opcode);
	if (opcode != 0x21001 && opcode != 0x21002)
		return -EPERM;
	QCBORDecode_Init(&decoder, (UsefulBufC){(const uint8_t *)request + 4, request_bytes - 4},
			 QCBOR_DECODE_MODE_NORMAL);
	QCBORDecode_EnterMap(&decoder, NULL);
	QCBORDecode_GetUInt64InMapN(&decoder, 3, &uid);
	QCBORDecode_ExitMap(&decoder);
	if (QCBORDecode_Finish(&decoder) || uid != session->profile.group ||
	    (uid & 0xf0000000) != 0x40000000)
		return -EPERM;
	/* CBO carries no TA identity: allow its protected I/O only during the
	 * selected Linux user's synchronous create/verify call. Sensor setup,
	 * sampling, enumeration and deletion never keep this window open.
	 */
	goodix_storage_allow_rpmb(session->storage, 1);
	int result = keymaster_native_command(session->keymaster, request, request_bytes,
					     response, capacity, actual, status);
	goodix_storage_allow_rpmb(session->storage, 0);
	return result;
}

static int cancellation_requested(const struct piano_goodix_options *options)
{
	return options && ((options->cancelled && *options->cancelled) ||
		(options->should_cancel && options->should_cancel(options->context)));
}

static int64_t monotonic_milliseconds(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static void report_progress(const struct piano_goodix_options *options,
			     enum piano_goodix_progress event, unsigned int remaining)
{
	if (options && options->progress)
		options->progress(event, remaining, options->context);
}

static int run_operation(struct profile_session *session, struct goodix_operation *operation,
			  unsigned int finger, unsigned int *matched_finger,
			  const struct piano_goodix_options *options)
{
	unsigned int seconds = options && options->timeout_seconds ? options->timeout_seconds :
		(finger && !matched_finger ? 120 : 30);
	int64_t deadline = monotonic_milliseconds() + (int64_t)seconds * 1000;
	struct pollfd irq = { .fd = session->client.controller, .events = POLLIN };
	int interrupted_result = 0, failure = 0, wait_irq = 0;

	for (;;) {
		if (!interrupted_result && cancellation_requested(options))
			interrupted_result = -ECANCELED;
		if (!interrupted_result && monotonic_milliseconds() >= deadline)
			interrupted_result = -ETIMEDOUT;
		if (interrupted_result || failure) {
			goodix_operation_cancel(operation);
			wait_irq = 0;
		}
		if (wait_irq) {
			int ready = poll(&irq, 1, 100);
			if (ready < 0 && errno == EINTR)
				continue;
			if (ready < 0 || (ready && !(irq.revents & POLLIN))) {
				failure = ready < 0 ? -errno : -EIO;
				continue;
			}
			if (!ready)
				continue;
			uint32_t count;
			if (read(irq.fd, &count, sizeof(count)) != sizeof(count)) {
				failure = -EIO;
				continue;
			}
			wait_irq = 0;
		}
		struct goodix_operation_sample sample;
		int result = goodix_operation_step(operation, &sample);
		if (result < 0) {
			clear_sensitive(&sample, sizeof(sample));
			/* Do not continue a failed exchange. The explicit cleanup resets
			 * this session only; no template or global credential is removed.
			 */
			goodix_events_cancel(operation->events);
			if (!matched_finger)
				goodix_operation_post_enroll(operation->events);
			return failure ? failure : result;
		}
		if (sample.kind == GOODIX_OPERATION_ENROLL_PROGRESS) {
			if (!sample.remaining) {
				session->profile.pending_finger = finger;
				session->profile.pending_id = sample.template_id;
				failure = profile_save(session->directory, &session->profile);
			}
			report_progress(options, PIANO_GOODIX_ENROLL_PROGRESS, sample.remaining);
		} else if (sample.kind == GOODIX_OPERATION_TEMPLATE_SAVED) {
			/* 1012 committed securely. Preserve the pending on-disk record
			 * if this local atomic replacement fails; the next call recovers it.
			 */
			session->profile.templates[finger - 1] = sample.template_id;
			session->profile.pending_finger = session->profile.pending_id = 0;
			failure = profile_save(session->directory, &session->profile);
		} else if (sample.kind == GOODIX_OPERATION_QUALITY_RETRY) {
			report_progress(options, PIANO_GOODIX_RETRY, 0);
		} else if (sample.kind == GOODIX_OPERATION_NOT_MATCH) {
			report_progress(options, PIANO_GOODIX_NOT_MATCH, 0);
			/* A completed negative match is one verification attempt. Run
			 * the normal cancel exchange before returning the denial.
			 */
			interrupted_result = -EACCES;
		} else if (sample.kind == GOODIX_OPERATION_ACQUIRED) {
			/* Factory HAL emits acquired 1022/1023 for bits 1/2. */
			if (sample.mask & 2)
				report_progress(options, PIANO_GOODIX_FINGER_DOWN, 0);
			if (sample.mask & 4)
				report_progress(options, PIANO_GOODIX_FINGER_UP, 0);
		}
		if (result == GOODIX_OPERATION_COMPLETE) {
			int complete = sample.kind == GOODIX_OPERATION_ENROLLED ||
				sample.kind == GOODIX_OPERATION_AUTHENTICATED;
			if (sample.kind == GOODIX_OPERATION_AUTHENTICATED && matched_finger) {
				for (size_t i = 0; i < 10; ++i)
					if (session->profile.templates[i] == sample.template_id)
						*matched_finger = i + 1;
				if (!*matched_finger)
					failure = -EACCES;
			}
			clear_sensitive(&sample, sizeof(sample));
			return failure ? failure : complete ? 0 : interrupted_result ? interrupted_result :
				-ECANCELED;
		}
		clear_sensitive(&sample, sizeof(sample));
		if (result == GOODIX_OPERATION_WAIT_IRQ) {
			uint32_t enable = 1;
			if (write(irq.fd, &enable, sizeof(enable)) != sizeof(enable))
				failure = -EIO;
			else
				wait_irq = 1;
		}
	}
}

int piano_goodix_enroll(uint32_t linux_uid, unsigned int finger,
			 const struct piano_goodix_options *options)
{
	struct profile_session session;
	struct goodix_operation operation = {0};
	struct goodix_gatekeeper_status status = {0};
	uint8_t hat[GOODIX_GATEKEEPER_HAT_BYTES] = {0};
	uint64_t challenge = 0, sid;
	uint32_t gatekeeper_opcode = 0;
	int started = 0;
	const char *stage = "opening Linux profile";

	if (!finger || finger > 10)
		return -EINVAL;
	if (cancellation_requested(options))
		return -ECANCELED;
	int result = profile_open(&session, linux_uid, 1);
	if (result)
		goto done;
	stage = "initializing sensor and Keymaster binding";
	result = session_start(&session, 1);
	if (!result) {
		stage = "enumerating Linux templates";
		result = profile_reconcile(&session);
	}
	if (!result && session.profile.templates[finger - 1]) {
		stage = "checking existing enrollment";
		result = -EEXIST;
	}
	if (!result) {
		stage = "opening native Keymaster";
		result = open_keymaster(&session.client, &session.keymaster);
	}
	struct goodix_gatekeeper gatekeeper = { .command = gatekeeper_command, .context = &session };
	if (!result) {
		stage = "provisioning Linux Gatekeeper credential";
		gatekeeper_opcode = 0x21001;
		result = goodix_gatekeeper_create(&gatekeeper, session.gatekeeper_directory, linux_uid,
						  &status);
	}
	if (!result) {
		stage = "allocating enrollment buffer";
		gatekeeper_opcode = 0;
		result = grow_operation_buffer(&session.client, session.geometry.enrollment_extended ?
			GOODIX_ENROLL_EVENT_BUFFER_BYTES : GOODIX_EVENT_BUFFER_BYTES);
	}
	struct goodix_events events = {
		.command = sensor_command_status, .context = &session.client,
		.buffer = session.client.shared, .buffer_size = session.client.shared_size,
	};
	if (!result) {
		stage = "preparing enrollment event exchange";
		result = goodix_events_start(&events);
		started = !result;
	}
	if (!result) {
		stage = "getting Goodix enrollment challenge";
		result = goodix_operation_challenge(&events, &challenge);
	}
	if (!result) {
		stage = "verifying Linux Gatekeeper credential";
		gatekeeper_opcode = 0x21002;
		result = goodix_gatekeeper_verify(&gatekeeper, session.gatekeeper_directory, linux_uid,
						  challenge, hat, &status);
	}
	if (!result) {
		stage = "saving Linux profile identity";
		gatekeeper_opcode = 0;
		memcpy(&sid, hat + 9, sizeof(sid));
		if (session.profile.sid && session.profile.sid != sid)
			result = -EACCES;
		else {
			session.profile.sid = sid;
			result = profile_save(session.directory, &session.profile);
		}
	}
	if (!result) {
		stage = "preparing authorized enrollment";
		result = goodix_operation_start_enroll(&operation, &events, session.profile.group, hat,
			session.geometry.enrollment_extended ? GOODIX_ENROLL_PACKET_EXTENDED :
			GOODIX_ENROLL_PACKET_NORMAL);
	}
	clear_sensitive(hat, sizeof(hat));
	if (!result) {
		stage = "capturing and saving fingerprint";
		result = run_operation(&session, &operation, finger, NULL, options);
	} else if (started) {
		/* Do not invalidate PRE_ENROLL before the HAT is consumed. This
		 * branch is only the failed preparation path, before 1007 was sent.
		 */
		goodix_events_cancel(&events);
		if (challenge)
			goodix_operation_post_enroll(&events);
	}
done:
	if (result) {
		fprintf(stderr, "Fingerprint enrollment failed while %s: error=%d\n", stage, result);
		if (gatekeeper_opcode)
			fprintf(stderr, "Gatekeeper command %#x: status=%d retry_ms=%u\n",
				gatekeeper_opcode, status.value, status.retry_ms);
	}
	clear_sensitive(hat, sizeof(hat));
	clear_sensitive(&operation, sizeof(operation));
	return session_close(&session, result);
}

int piano_goodix_verify(uint32_t linux_uid, unsigned int finger,
			 unsigned int *matched_finger, const struct piano_goodix_options *options)
{
	struct profile_session session;
	struct goodix_operation operation = {0};
	uint32_t allowed[10];
	size_t count = 0;
	uint64_t challenge;
	const char *stage = "opening Linux profile";

	if (finger > 10 || !matched_finger)
		return -EINVAL;
	*matched_finger = 0;
	if (cancellation_requested(options))
		return -ECANCELED;
	int result = profile_open(&session, linux_uid, 0);
	if (result)
		goto done;
	stage = "initializing sensor and Keymaster binding";
	result = session_start(&session, 0);
	if (!result) {
		stage = "enumerating Linux templates";
		result = profile_reconcile(&session);
	}
	if (result)
		goto done;
	stage = "selecting enrolled finger";
	for (size_t i = 0; i < 10; ++i)
		if (session.profile.templates[i] && (!finger || finger == i + 1))
			allowed[count++] = session.profile.templates[i];
	if (!count) {
		result = -ENOENT;
		goto done;
	}
	if (getrandom(&challenge, sizeof(challenge), 0) != sizeof(challenge) || !challenge) {
		result = -EIO;
		goto done;
	}
	stage = "preparing authentication";
	result = grow_operation_buffer(&session.client, GOODIX_EVENT_BUFFER_BYTES);
	struct goodix_events events = {
		.command = sensor_command_status, .context = &session.client,
		.buffer = session.client.shared, .buffer_size = session.client.shared_size,
	};
	if (!result)
		result = goodix_events_start(&events);
	if (!result)
		result = goodix_operation_start_authenticate(&operation, &events, session.profile.group,
			session.profile.sid, challenge, allowed, count, session.geometry.interaction_policy);
	if (!result) {
		stage = "capturing fingerprint";
		result = run_operation(&session, &operation, finger, matched_finger, options);
	}
done:
	clear_sensitive(&operation, sizeof(operation));
	int closed = session_close(&session, result);
	if (closed)
		fprintf(stderr, "Fingerprint verification failed while %s: error=%d\n",
			result ? stage : "closing sensor and storage", closed);
	return closed;
}

int piano_goodix_list(uint32_t linux_uid, unsigned int fingers[10], size_t *count)
{
	struct profile_session session;

	if (!fingers || !count)
		return -EINVAL;
	*count = 0;
	memset(fingers, 0, 10 * sizeof(*fingers));
	int result = profile_open(&session, linux_uid, 0);
	if (result == -ENOENT)
		return session_close(&session, 0);
	if (!result)
		result = session_start(&session, 0);
	if (!result)
		result = profile_reconcile(&session);
	if (!result)
		for (size_t i = 0; i < 10; ++i)
			if (session.profile.templates[i])
				fingers[(*count)++] = i + 1;
	return session_close(&session, result);
}

int piano_goodix_delete(uint32_t linux_uid, unsigned int finger)
{
	struct profile_session session;
	struct template_list remaining = {0};

	if (!finger || finger > 10)
		return -EINVAL;
	int result = profile_open(&session, linux_uid, 0);
	if (result)
		goto done;
	result = session_start(&session, 0);
	if (!result)
		result = profile_reconcile(&session);
	uint32_t id = session.profile.templates[finger - 1];
	if (!result && !id)
		result = -ENOENT;
	if (result)
		goto done;
	memset(session.client.shared, 0, session.client.shared_size);
	memcpy((uint8_t *)session.client.shared + 0x6c, &session.profile.group, sizeof(uint32_t));
	memcpy((uint8_t *)session.client.shared + 0x70, &id, sizeof(id));
	result = sensor_command(&session.client, 1013, 0xcc);
	if (!result)
		result = enumerate_templates(&session.client, session.geometry.max_templates, &remaining);
	/* 1013 +0xc8 is the remaining total, not a deletion count. Confirm the
	 * selected Linux-owned ID is absent instead of interpreting that number.
	 */
	if (!result && profile_contains(&remaining, session.profile.group, id))
		result = -EIO;
	if (!result) {
		session.profile.templates[finger - 1] = 0;
		result = profile_save(session.directory, &session.profile);
	}
done:
	return session_close(&session, result);
}

/* Reuse only the Linux Gatekeeper profile. This diagnostic never loads the
 * Goodix TA or opens UIO. Protected-storage I/O needs the explicit UFS option;
 * the original denial probe remains available without that capability.
 */
static int gatekeeper_storage_probe(uint32_t linux_uid, int ufs)
{
	struct profile_session session;
	struct goodix_gatekeeper_status status = {0};
	unsigned int requests = 0;
	int result = profile_open(&session, linux_uid, 1);

	if (!result)
		result = ufs ? goodix_storage_open_rpmb_ufs(&session.storage) :
			goodix_storage_open_rpmb_probe(&session.storage);
	if (!result && (MinkCom_getRootEnvObject(&session.client.root) ||
	    MinkCom_getClientEnvObject(session.client.root, &session.client.env)))
		result = -EIO;
	if (!result)
		result = open_keymaster(&session.client, &session.keymaster);
	struct goodix_gatekeeper gatekeeper = { .command = gatekeeper_command, .context = &session };
	if (!result)
		result = goodix_gatekeeper_create(&gatekeeper, session.gatekeeper_directory,
						 linux_uid, &status);
	if (session.storage)
		requests = goodix_storage_rpmb_probe_requests(session.storage);
	fprintf(stderr, "Gatekeeper RPMB %s probe: requests=%u status=%d retry_ms=%u error=%d\n",
		ufs ? "UFS" : "denial", requests, status.value, status.retry_ms, result);
	return session_close(&session, result) ? 1 : 0;
}

int piano_goodix_diagnostics(int argc, char **argv)
{
	struct client client = { .controller = -1 };
	struct sensor_geometry geometry = {0};
	struct goodix_storage *storage = NULL;
	uint32_t sensor_type = 0;
	int storage_probe = argc >= 2 && !strcmp(argv[1], "storage-probe");
	int watch = argc >= 2 && !strcmp(argv[1], "watch");
	int auth_ready = argc >= 2 && !strcmp(argv[1], "prepare-auth");
	int initialize = argc >= 2 && (!strcmp(argv[1], "initialize") ||
				       watch || auth_ready || storage_probe);
	unsigned int seconds = 30;
	uint32_t user_id = 0;
	int result;

	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--help"))) {
		puts("Usage: piano-fingerprint-ctl info|initialize|prepare-auth|watch [seconds]\n"
		     "       piano-fingerprint-ctl storage-probe --state-dir <empty-directory> <group>\n"
		     "       piano-fingerprint-ctl storage-probe --rpmb-deny <LinuxUID>\n"
		     "       piano-fingerprint-ctl storage-probe --rpmb-ufs <LinuxUID>\n"
		     "Query the real Goodix sensor through UIO and QTEE.\n"
		     "initialize also reads the sensor's geometry and calibration.\n"
		     "watch reports sensor event flags for 1-120 seconds (default 30), then cancels.\n"
		     "storage-probe serves restricted FS/GPFS in an explicit empty Linux directory,\n"
		     "then initializes and selects a group; it does not enroll, remove or authenticate.\n"
		     "prepare-auth initializes Keymaster sharing when needed and binds Goodix.\n"
		     "--rpmb-deny probes the private Linux Gatekeeper credential, refuses every RPMB\n"
		     "request and stops before Goodix initialization or fingerprint sampling.\n"
		     "--rpmb-ufs instead permits authenticated UFS RPMB reads/writes for that probe;\n"
		     "use only after authorizing protected-storage access. It never programs a key.");
		return 0;
	}
	if (storage_probe && argc == 4 &&
	    (!strcmp(argv[2], "--rpmb-deny") || !strcmp(argv[2], "--rpmb-ufs"))) {
		char *end;
		errno = 0;
		unsigned long value = strtoul(argv[3], &end, 10);
		if (errno || !*argv[3] || *end || *argv[3] == '-' || value > UINT32_MAX) {
			fputs("Linux UID must be an unsigned 32-bit ID.\n", stderr);
			return 2;
		}
		return gatekeeper_storage_probe(value, !strcmp(argv[2], "--rpmb-ufs"));
	}
	if (storage_probe && (argc != 5 || strcmp(argv[2], "--state-dir"))) {
		fputs("storage-probe requires --state-dir <empty-directory> <group>.\n", stderr);
		return 2;
	}
	if (!storage_probe && ((argc != 2 && !(watch && argc == 3)) ||
	    (!initialize && strcmp(argv[1], "info")))) {
		fputs("Use --help for supported operations.\n", stderr);
		return 2;
	}
	if (watch && argc == 3) {
		char *end;
		unsigned long value = strtoul(argv[2], &end, 10);
		if (!*argv[2] || *end || value < 1 || value > 120) {
			fputs("Watch duration must be between 1 and 120 seconds.\n", stderr);
			return 2;
		}
		seconds = value;
	}
	if (storage_probe) {
		char *end;
		const char *number = argv[4];
		errno = 0;
		unsigned long value = strtoul(number, &end, 10);
		if (errno || !*number || *end || *number == '-' || value >= UINT32_MAX) {
			fputs("Group must be an unsigned 32-bit ID.\n", stderr);
			return 2;
		}
		user_id = value;
	}
	if (storage_probe) {
		result = goodix_storage_open(argv[3], &storage);
		if (result) {
			fprintf(stderr, "Cannot start restricted FS/GPFS: %s (%d)\n",
				strerror(-result), result);
			return 1;
		}
	}
	/* A closed output pipe must not bypass sensor cancellation or teardown. */
	signal(SIGPIPE, SIG_IGN);
	result = open_client(&client);
	if (!result)
		result = device_info(&client, &sensor_type);
	if (!result && initialize) {
		geometry.sensor_type = sensor_type;
		result = initialize_sensor(&client, &geometry);
	}
	if (!result && auth_ready)
		result = prepare_authentication(&client);
	if (!result && watch)
		result = watch_events(&client, seconds);
	if (!result && storage_probe) {
		uint8_t *payload = client.shared;
		const char path[] = "/data/vendor_de/0/fpdata";
		memset(payload, 0, client.shared_size);
		memcpy(payload + 0x6c, &user_id, sizeof(user_id));
		memcpy(payload + 0x70, path, sizeof(path));
		result = sensor_command(&client, 1014, 0x270);
	}
	int closed = close_client(&client);
	goodix_storage_close(storage);
	if (!result)
		result = closed;
	if (!result && storage_probe)
		puts("{\"storage_probe_ok\":true}");
	else if (!result && auth_ready)
		puts("{\"sensor\":\"Goodix\",\"sensor_initialized\":true,"
		     "\"keymaster_binding_ready\":true}");
	else if (!result && initialize)
		printf("{\"sensor\":\"Goodix\",\"sensor_type\":%u,"
		       "\"chip_type\":%u,\"max_templates\":%u,\"rows\":%u,\"columns\":%u,"
		       "\"nav_rows\":%u,\"nav_columns\":%u,"
		       "\"model_required\":%u,\"anti_spoof_required\":%u,"
		       "\"interaction_policy\":%u,"
		       "\"models_loaded\":%s,\"sensor_initialized\":true}\n",
		       geometry.sensor_type, geometry.chip_type, geometry.max_templates, geometry.rows,
		       geometry.columns, geometry.nav_rows, geometry.nav_columns,
		       geometry.model_required, geometry.anti_spoof_required,
		       geometry.interaction_policy,
		       geometry.models_loaded ? "true" : "false");
	else if (!result)
		printf("{\"sensor\":\"Goodix\",\"sensor_type\":%u,"
		       "\"command_address_bits\":%u,\"device_info_ok\":true}\n",
		       sensor_type, client.architecture == 2 ? 64 : 32);
	if (result)
		fprintf(stderr, "Fingerprint operation failed: %s (%d)\n",
			strerror(-result), result);
	return result ? 1 : 0;
}
