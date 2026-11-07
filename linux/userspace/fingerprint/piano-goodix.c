// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2026 slwyts <slwyts@foxmail.com>
 *
 * Native Piano Goodix control client. The application interface follows the
 * factory libgf_ca/QSEECom calls; transport is Qualcomm's public MinkIPC API.
 * Enrollment is not offered: the TA requires a password-verified token that
 * Linux cannot obtain without writing Keymaster storage
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "MinkCom.h"
#include <qcbor/qcbor_decode.h>
#include <qcbor/qcbor_encode.h>
#include <qcbor/qcbor_spiffy_decode.h>

#include "piano-goodix-protocol.h"

#define FIRMWARE "/usr/lib/firmware/gfpia.elf"
#define STATE_DIR "/var/lib/piano-goodix"
#define REGISTER_LISTENER_SERVICE 87
#define FIRMWARE_LIMIT (32U * 1024U * 1024U)
#define LOADER_UID 122
#define PAYLOAD_SIZE 640
#define SHARED_SIZE 4096
#define MODEL_BYTES 0x124ff4
#define MODEL_CHUNK_BYTES 0x7cf9c
#define MODEL_PACKET_BYTES (MODEL_CHUNK_BYTES + 0x78)
static volatile sig_atomic_t interrupted;

static void stop_operation(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

struct file_service {
	pthread_mutex_t lock;
	int directory;
	uint8_t *buffer;
	size_t size;
	struct goodix_file_counts counts;
};

struct client {
	Object root, env, loader, app, memory;
	Object file_registry, file_memory;
	struct file_service files;
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
		result = sensor_command(client, GOODIX_EXIT, GOODIX_HEADER_BYTES);
	}
	if (!Object_isNull(client->app)) {
		int shutdown = invoke(client->app, 2, NULL, ObjectCounts_pack(0, 0, 0, 0));
		if (!result)
			result = shutdown;
	}
	if (client->controller >= 0)
		close(client->controller);
	Object_RELEASE_IF(client->memory);
	client->memory = Object_NULL;
	Object_RELEASE_IF(client->app);
	/* Storage stays available until the TA has shut down */
	Object_RELEASE_IF(client->file_registry);
	Object_RELEASE_IF(client->file_memory);
	Object_RELEASE_IF(client->loader);
	Object_RELEASE_IF(client->env);
	Object_RELEASE_IF(client->root);
	if (client->files.buffer) {
		const struct goodix_file_counts *counts = &client->files.counts;

		fprintf(stderr, "TA storage: %u reads, %u writes, %u removals, "
			"%u renames, %u refused.\n", counts->reads, counts->writes,
			counts->removes, counts->renames, counts->refused);
	}
	if (client->files.directory >= 0)
		close(client->files.directory); /* also drops the operation lock */
	return result;
}

/* Named so an unexpected path layout does not look like empty storage */
static void report_refusal(const struct goodix_file_counts *counts)
{
	fprintf(stderr, "TA file service refused command %u for \"", counts->refused_command);
	for (const char *c = counts->refused_path; *c; c++)
		fprintf(stderr, *c >= 0x20 && *c < 0x7f ? "%c" : "\\x%02x",
			(unsigned char)*c);
	fputs("\"\n", stderr);
}

/* IListenerCBO: method 0 is request, 1 is wait. Requests are answered in
 * place in the registered buffer
 */
static int32_t file_service_invoke(ObjectCxt context, ObjectOp op,
				   ObjectArg *args, ObjectCounts counts)
{
	struct file_service *files = context;
	int result;

	switch (ObjectOp_methodID(op)) {
	case Object_OP_retain:
	case Object_OP_release:
		return Object_OK;
	case 0:
		if (counts != ObjectCounts_pack(0, 2, 0, 4))
			return Object_ERROR_INVALID;
		/* No embedded buffers and no extra memory objects */
		args[0].b.size = 0;
		if (args[1].b.size >= sizeof(uint32_t))
			memset(args[1].b.ptr, 0, sizeof(uint32_t));
		for (size_t i = 2; i < 6; i++)
			args[i].o = Object_NULL;
		pthread_mutex_lock(&files->lock);
		unsigned int refused = files->counts.refused;
		result = goodix_file_service(files->directory, files->buffer,
					     files->size, &files->counts);
		if (files->counts.refused != refused)
			report_refusal(&files->counts);
		pthread_mutex_unlock(&files->lock);
		return result ? Object_ERROR : Object_OK;
	case 1:
		return Object_OK;
	default:
		return Object_ERROR_INVALID;
	}
}

static int open_state_directory(struct client *client)
{
	struct stat st;
	int fd;

	if (mkdir(STATE_DIR, 0700) && errno != EEXIST)
		return -errno;
	fd = open(STATE_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || st.st_uid != 0 || (st.st_mode & 077)) {
		fprintf(stderr, STATE_DIR " must be a root-owned 0700 directory.\n");
		close(fd);
		return -EPERM;
	}
	/* One TA session at a time, whether started by the CLI or a service */
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		int error = errno == EWOULDBLOCK ? -EBUSY : -errno;

		if (error == -EBUSY)
			fprintf(stderr, "Another fingerprint operation is running.\n");
		close(fd);
		return error;
	}
	client->files.directory = fd;
	return 0;
}

static int start_file_service(struct client *client)
{
	uint32_t service = REGISTER_LISTENER_SERVICE, id = GOODIX_FILE_SERVICE_ID;
	void *address = NULL;
	size_t capacity = 0;
	int result;
	ObjectArg open[] = {
		{ .bi = {&service, sizeof(service)} }, { .o = Object_NULL },
	};

	result = invoke(client->env, 0, open, ObjectCounts_pack(1, 0, 0, 1));
	if (result)
		return result;
	client->file_registry = open[1].o;
	if (MinkCom_getMemoryObject(client->root, GOODIX_FILE_SERVICE_BYTES,
				    &client->file_memory) ||
	    MinkCom_getMemoryObjectInfo(client->file_memory, &address, &capacity) ||
	    capacity < GOODIX_FILE_SERVICE_BYTES)
		return -ENOMEM;
	client->files.buffer = address;
	client->files.size = capacity;
	ObjectArg registration[] = {
		{ .bi = {&id, sizeof(id)} },
		{ .o = (Object){ file_service_invoke, &client->files } },
		{ .o = client->file_memory },
	};
	return invoke(client->file_registry, 0, registration,
		      ObjectCounts_pack(1, 0, 2, 0));
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

static int open_client(struct client *client, int needs_storage)
{
	uint32_t uid = LOADER_UID;
	int result;

	memset(client, 0, sizeof(*client));
	client->controller = -1;
	client->files.directory = -1;
	pthread_mutex_init(&client->files.lock, NULL);
	result = open_state_directory(client);
	if (result)
		return result;
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
	/* Registered before the first TA command, as on Android */
	result = start_file_service(client);
	if (result && needs_storage) {
		fputs("The TA file service could not be registered, so stored "
		      "fingerprints cannot be read.\n", stderr);
		return result;
	}
	if (result)
		fputs("The TA file service is unavailable; continuing without "
		      "fingerprint storage.\n", stderr);
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

static int application_command(struct client *client, uint32_t route,
			       uint32_t command, size_t size)
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
	/* As libgf_ca: response[1] is the transport status, response[0] the
	 * TA's GF_ERROR code, returned as a positive value
	 */
	if (response[1]) {
		fprintf(stderr, "Goodix command %u: buffer=%d\n", command, response[1]);
		return -EREMOTEIO;
	}
	return response[0] > 0 ? response[0] : response[0] ? -EPROTO : 0;
}

static int sensor_command(struct client *client, uint32_t command, size_t size)
{
	time_t now = time(NULL);
	struct tm local;

	if (size < 0x6c || size > client->shared_size)
		return -EMSGSIZE;
	memcpy((uint8_t *)client->shared + 0x44, &client->sequence,
	       sizeof(client->sequence));
	localtime_r(&now, &local);
	/* Descriptive factory metadata, unrelated to authentication tokens. */
	strftime((char *)client->shared + 0x4c, 0x20, "%Y-%m-%d %H:%M:%S", &local);
	return application_command(client, 1, command, size);
}

static void clear_sensitive(void *buffer, size_t size)
{
	volatile uint8_t *bytes = buffer;

	while (size--)
		*bytes++ = 0;
}

static int keymaster_native_command(Object app, const void *request, size_t size,
				    uint8_t *response, size_t capacity, size_t *actual)
{
	ObjectArg args[] = {
		{ .bi = {request, size} }, { .b = {response, capacity} },
	};
	int32_t status;
	int result = invoke(app, 0, args, ObjectCounts_pack(1, 1, 0, 0));

	if (result)
		return result;
	if (args[1].b.size < sizeof(status) || args[1].b.size > capacity)
		return -EMSGSIZE;
	memcpy(&status, response, sizeof(status));
	if (status) {
		fprintf(stderr, "Keymaster native command %#x: status=%d\n",
			*(const uint32_t *)request, status);
		return -EREMOTEIO;
	}
	*actual = args[1].b.size;
	return 0;
}

static int keymaster_sharing_init(struct client *client)
{
	const char name[] = "keymaster64";
	Object opener = Object_NULL, environment = Object_NULL, app = Object_NULL;
	uint32_t uid = 151, length, version = 0x200;
	uint32_t handshake[] = {0x207, 4, 5, 4, 5, 0};
	uint32_t get = 0x220e, compute = 0x220f;
	uint8_t response[512] = {0}, request[128] = {0}, parameters[64] = {0};
	size_t actual = 0;
	UsefulBufC seed = NULLUsefulBufC, nonce = NULLUsefulBufC, encoded;
	QCBORDecodeContext decoder;
	QCBOREncodeContext encoder;
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
	result = keymaster_native_command(app, &version, sizeof(version),
					 response, sizeof(response), &actual);
	if (!result)
		result = keymaster_native_command(app, handshake, sizeof(handshake),
						 response, sizeof(response), &actual);
	if (!result)
		result = keymaster_native_command(app, &get, sizeof(get),
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
	result = keymaster_native_command(app, request, encoded.len + 4,
					 response, sizeof(response), &actual);
done:
	clear_sensitive(response, sizeof(response));
	clear_sensitive(request, sizeof(request));
	clear_sensitive(parameters, sizeof(parameters));
	Object_RELEASE_IF(app);
	Object_RELEASE_IF(environment);
	Object_RELEASE_IF(opener);
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
		result = sensor_command(client, GOODIX_DETECT_SENSOR, PAYLOAD_SIZE);
		if (result < 0 && result != -EREMOTEIO)
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
		uint32_t total = MODEL_BYTES, command = GOODIX_INIT_MODEL;
		int result;

		if (bytes > MODEL_CHUNK_BYTES)
			bytes = MODEL_CHUNK_BYTES;
		memset(payload, 0, capacity);
		memcpy(payload + 0x6c, model + offset, bytes);
		payload[0x7d008] = part;
		memcpy(payload + 0x7d00c, &total, sizeof(total));
		memcpy(payload + 0x7d010, &command, sizeof(command));
		result = sensor_command(client, GOODIX_TRANSPORT_DATA, MODEL_PACKET_BYTES);
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
	/* Milan initialization, recovered from the factory HAL's sensor-type-5
	 * function table. Empty OTP cache means read calibration from the real
	 * sensor. No Android storage, user template or enrollment command is used.
	 */
	memset(payload, 0, client->shared_size);
	result = sensor_command(client, GOODIX_INIT, 0x168);
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
	/* No debug dumping, matching the stock default */
	result = sensor_command(client, GOODIX_INIT_FINISHED, 0x74);
	if (result)
		return result;
	memset(client->shared, 0, client->shared_size);
	/* Byte 0x6c is disable_study; stock default 0 */
	return sensor_command(client, GOODIX_UPDATE_CONFIG, 0xc0);
}

static int select_group(struct client *client, uint32_t user_id)
{
	int result;

	/* The group is the Linux UID; the TA ignores the path (gfpia 0x47810) */
	memset(client->shared, 0, client->shared_size);
	result = goodix_set_active_group_request(client->shared, user_id, STATE_DIR);
	if (!result)
		result = sensor_command(client, GOODIX_SET_ACTIVE_GROUP,
					GOODIX_SET_ACTIVE_GROUP_BYTES);
	return result;
}

static int enumerate_templates(struct client *client, uint32_t maximum,
			       struct template_list *list)
{
	uint8_t *payload = client->shared;
	int result;

	if (!maximum || maximum > sizeof(list->entries) / sizeof(list->entries[0]))
		return -EPROTO;
	memset(payload, 0, client->shared_size);
	result = sensor_command(client, GOODIX_ENUMERATE, GOODIX_ENUMERATE_BYTES);
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

static int64_t milliseconds_left(const struct timespec *deadline)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (deadline->tv_sec - now.tv_sec) * 1000 +
	       (deadline->tv_nsec - now.tv_nsec) / 1000000;
}

/* 1 on IRQ, 0 on timeout or Ctrl+C */
static int wait_for_interrupt(struct client *client, const struct timespec *deadline,
			      uint32_t *counter)
{
	struct pollfd event = { .fd = client->controller, .events = POLLIN };

	while (!interrupted) {
		int64_t remaining = milliseconds_left(deadline);
		int ready;

		if (remaining <= 0)
			return 0;
		ready = poll(&event, 1, remaining > 500 ? 500 : remaining);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return -errno;
		if (!ready)
			continue;
		if (!(event.revents & POLLIN))
			return -EIO;
		if (read(client->controller, counter, sizeof(*counter)) != sizeof(*counter))
			return -EIO;
		return 1;
	}
	return 0;
}

static int enable_interrupt(struct client *client)
{
	uint32_t enable = 1;

	return write(client->controller, &enable, sizeof(enable)) == sizeof(enable) ?
	       0 : -EIO;
}

static int reserve_irq_buffer(struct client *client)
{
	void *shared;
	size_t capacity;

	if (client->shared_size >= GOODIX_IRQ_BYTES)
		return 0;
	Object_RELEASE_IF(client->memory);
	client->memory = Object_NULL;
	client->shared = NULL;
	client->shared_size = 0;
	if (MinkCom_getMemoryObject(client->root, GOODIX_IRQ_BYTES, &client->memory) ||
	    MinkCom_getMemoryObjectInfo(client->memory, &shared, &capacity) ||
	    capacity < GOODIX_IRQ_BYTES)
		return -ENOMEM;
	client->shared = shared;
	client->shared_size = capacity;
	return 0;
}

static int watch_events(struct client *client, unsigned int seconds)
{
	struct timespec deadline;
	uint8_t *payload;
	int result;

	result = reserve_irq_buffer(client);
	if (result)
		return result;
	payload = client->shared;
	memset(payload, 0, client->shared_size);
	/* GF_CMD_CANCEL leaves the sensor in idle finger detection */
	result = sensor_command(client, GOODIX_CANCEL, GOODIX_CANCEL_BYTES);
	if (result)
		return result;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += seconds;
	fprintf(stderr, "Sensor ready; watching interrupts for %u seconds.\n", seconds);
	for (;;) {
		uint32_t counter;

		result = wait_for_interrupt(client, &deadline, &counter);
		if (result <= 0)
			return result;
		/* Query/acknowledge the sensor before allowing its next IRQ.
		 * No image or template data is saved or printed.
		 */
		goodix_irq_request(payload);
		result = sensor_command(client, GOODIX_IRQ, GOODIX_IRQ_BYTES);
		if (result)
			return result;
		printf("{\"irq_count\":%u,\"sensor_events\":%u}\n", counter,
		       goodix_get32(payload, 0x6c));
		fflush(stdout);
		clear_sensitive(payload, GOODIX_IRQ_BYTES);
		result = enable_interrupt(client);
		if (result)
			return result;
	}
}

struct verification {
	enum goodix_result result;
	uint32_t finger;
	int status;
	int finger_down;
};

static void report_event(const char *event)
{
	printf("{\"event\":\"%s\"}\n", event);
	fflush(stdout);
}

/* Follows hal_milan_irq 0x41390 and irq_process 0x41bc8 in libgf_hal.so.
 * The token at GOODIX_TOKEN_OFFSET is never copied out
 */
static int process_interrupt(struct client *client, struct verification *state)
{
	uint8_t *irq = client->shared;
	uint32_t type, operation, step;
	int status;

	goodix_irq_request(irq);
	status = sensor_command(client, GOODIX_IRQ, GOODIX_IRQ_BYTES);
	if (status < 0)
		return status;
	type = goodix_get32(irq, 0x6c);
	operation = goodix_get32(irq, 0x70);
	step = status ? 0 : goodix_get32(irq, 0x98);
	if (!type)
		return 0;
	/* A finger-up without a finger-down is stale */
	if ((type & GOODIX_IRQ_FINGER_UP) && !state->finger_down)
		return 0;
	if (!status && step == GOODIX_STEP_REPEAT) {
		status = sensor_command(client, GOODIX_IRQ, GOODIX_IRQ_BYTES);
		step = status ? 0 : goodix_get32(irq, 0x98);
	}
	if (!status && step == GOODIX_STEP_CONTINUE)
		status = sensor_command(client, GOODIX_IRQ, GOODIX_IRQ_BYTES);
	if (status < 0)
		return status;
	type = goodix_get32(irq, 0x6c);
	if (type & GOODIX_IRQ_FINGER_DOWN) {
		state->finger_down = 1;
		report_event("finger_down");
		step = status ? 0 : goodix_get32(irq, 0x98);
		if (!status && step == GOODIX_STEP_CAPTURE &&
		    operation != GOODIX_OPERATION_RAW_DATA) {
			/* Capture and match inside the TA */
			status = sensor_command(client, GOODIX_IRQ, GOODIX_IRQ_BYTES);
			if (status < 0)
				return status;
			type = goodix_get32(irq, 0x6c);
		}
	}
	if ((type & GOODIX_IRQ_NEEDS_RESET) || irq[0]) {
		fprintf(stderr, "The sensor asked for a reset (events %#x). Run the "
			"command again; the sensor is reset when it is reopened.\n", type);
		return -EIO;
	}
	while (type & GOODIX_IRQ_IMAGE) {
		state->result = goodix_authenticate_result(goodix_get32(irq, 0x70), status);
		state->status = status;
		if (state->result == GOODIX_RESULT_MATCH)
			state->finger = goodix_get32(irq, 0x84);
		if (state->result != GOODIX_RESULT_RETRY)
			break;
		goodix_irq_capture_again(irq);
		status = sensor_command(client, GOODIX_IRQ, GOODIX_IRQ_BYTES);
		if (status < 0)
			return status;
		type = goodix_get32(irq, 0x6c);
		state->result = GOODIX_RESULT_NONE;
	}
	if (state->result == GOODIX_RESULT_MATCH) {
		uint8_t finish[GOODIX_AUTHENTICATE_FINISH_BYTES];

		/* Lets the TA update the template, as the stock HAL does */
		goodix_authenticate_finish_request(finish, irq);
		clear_sensitive(irq, GOODIX_IRQ_BYTES);
		memcpy(irq, finish, sizeof(finish));
		status = sensor_command(client, GOODIX_AUTHENTICATE_FINISH, sizeof(finish));
		if (status)
			fprintf(stderr, "Template update after the match failed: %s (%d).\n",
				goodix_error_name(status), status);
	}
	if (type & GOODIX_IRQ_FINGER_UP) {
		state->finger_down = 0;
		report_event("finger_up");
	}
	return 0;
}

static const char *hint_name(enum goodix_result result)
{
	switch (result) {
	case GOODIX_RESULT_PARTIAL: return "partial";
	case GOODIX_RESULT_DIRTY: return "dirty";
	case GOODIX_RESULT_TOO_FAST: return "too_fast";
	case GOODIX_RESULT_TOO_SLOW: return "too_slow";
	case GOODIX_RESULT_PALM: return "palm";
	default: return NULL;
	}
}

static int verify_finger(struct client *client, uint32_t user_id, unsigned int seconds,
			 size_t templates, struct verification *state)
{
	struct timespec deadline;
	int result, cancel;

	result = reserve_irq_buffer(client);
	if (result)
		return result;
	memset(client->shared, 0, client->shared_size);
	goodix_authenticate_request(client->shared, user_id);
	result = sensor_command(client, GOODIX_AUTHENTICATE, GOODIX_AUTHENTICATE_BYTES);
	if (result)
		return result;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += seconds;
	fprintf(stderr, "Touch the power button sensor (%zu enrolled for UID %u, "
		"%u seconds).\n", templates, user_id, seconds);
	while (state->result == GOODIX_RESULT_NONE ||
	       state->result == GOODIX_RESULT_ERROR || hint_name(state->result)) {
		uint32_t counter;

		result = wait_for_interrupt(client, &deadline, &counter);
		if (result <= 0)
			break;
		state->result = GOODIX_RESULT_NONE;
		result = process_interrupt(client, state);
		if (result)
			break;
		if (hint_name(state->result)) {
			printf("{\"event\":\"retry\",\"reason\":\"%s\"}\n",
			       hint_name(state->result));
			fflush(stdout);
		} else if (state->result == GOODIX_RESULT_ERROR) {
			fprintf(stderr, "Capture failed: %s (%d); touch again.\n",
				goodix_error_name(state->status), state->status);
		} else if (state->result == GOODIX_RESULT_MATCH ||
			   state->result == GOODIX_RESULT_NO_MATCH) {
			/* Final: no further IRQ is needed, so a failing re-arm
			 * cannot turn this result into an error
			 */
			break;
		}
		result = enable_interrupt(client);
		if (result)
			break;
	}
	/* Ends the operation, as hal_milan_cancel 0x40188 */
	memset(client->shared, 0, client->shared_size);
	cancel = sensor_command(client, GOODIX_CANCEL, GOODIX_CANCEL_BYTES);
	clear_sensitive(client->shared, client->shared_size);
	return result < 0 ? result : cancel;
}

static int parse_number(const char *text, unsigned long minimum,
			unsigned long maximum, unsigned long *value)
{
	char *end;

	errno = 0;
	*value = strtoul(text, &end, 10);
	return errno || !*text || *end || *value < minimum || *value > maximum ?
	       -EINVAL : 0;
}

static void usage(void)
{
	puts("Usage: piano-fingerprint-ctl info|initialize|prepare-auth\n"
	     "       piano-fingerprint-ctl watch [seconds]\n"
	     "       piano-fingerprint-ctl list <uid>\n"
	     "       piano-fingerprint-ctl verify <uid> [seconds]\n"
	     "Query the real Goodix sensor through UIO and QTEE.\n"
	     "initialize also reads the sensor's geometry and calibration.\n"
	     "watch reports sensor event flags for 1-120 seconds (default 30).\n"
	     "list prints the fingerprints enrolled for a Linux UID.\n"
	     "verify waits 1-60 seconds (default 10) for one touch and matches it\n"
	     "against that UID's fingerprints; exit status 0 means a match.\n"
	     "prepare-auth initializes Keymaster sharing when needed and binds Goodix.\n"
	     "Fingerprint data stays encrypted by the TA in " STATE_DIR ".\n"
	     "Enrollment is not available: the TA requires a password-verified\n"
	     "authorization token, which Linux cannot obtain.");
}

int main(int argc, char **argv)
{
	struct client client = { .controller = -1, .files = { .directory = -1 } };
	struct sensor_geometry geometry = {0};
	struct template_list templates = {0};
	struct verification verification = { .result = GOODIX_RESULT_NONE };
	struct sigaction stop = { .sa_handler = stop_operation };
	const char *operation = argc >= 2 ? argv[1] : "";
	int watch = !strcmp(operation, "watch");
	int list = !strcmp(operation, "list");
	int verify = !strcmp(operation, "verify");
	int auth_ready = !strcmp(operation, "prepare-auth");
	int initialize = !strcmp(operation, "initialize") || watch || list ||
			 verify || auth_ready;
	unsigned long value;
	unsigned int seconds = verify ? 10 : 30;
	uint32_t user_id = 0, sensor_type = 0;
	int result;

	if (argc == 1 || (argc == 2 && !strcmp(operation, "--help"))) {
		usage();
		return 0;
	}
	if (!(initialize || !strcmp(operation, "info")) ||
	    (watch && argc > 3) || (list && argc != 3) ||
	    (verify && (argc < 3 || argc > 4)) ||
	    (!watch && !list && !verify && argc != 2)) {
		fputs("Use --help for supported operations.\n", stderr);
		return 2;
	}
	if (watch && argc == 3) {
		if (parse_number(argv[2], 1, 120, &value)) {
			fputs("Watch duration must be between 1 and 120 seconds.\n", stderr);
			return 2;
		}
		seconds = value;
	}
	if (list || verify) {
		if (parse_number(argv[2], 0, UINT32_MAX - 1, &value)) {
			fputs("User ID must be an unsigned 32-bit Linux UID.\n", stderr);
			return 2;
		}
		user_id = value;
	}
	if (verify && argc == 4) {
		if (parse_number(argv[3], 1, 60, &value)) {
			fputs("Verification timeout must be between 1 and 60 seconds.\n", stderr);
			return 2;
		}
		seconds = value;
	}
	/* Ctrl+C ends waits early; the TA, IRQ and power are then released */
	sigaction(SIGINT, &stop, NULL);
	sigaction(SIGTERM, &stop, NULL);
	result = open_client(&client, list || verify);
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
	if (!result && (list || verify))
		result = select_group(&client, user_id);
	if (!result && (list || verify))
		result = enumerate_templates(&client, geometry.max_templates, &templates);
	if (!result && verify)
		result = verify_finger(&client, user_id, seconds, templates.count,
				       &verification);
	int closed = close_client(&client);
	if (!result)
		result = closed;
	if (!result && list) {
		printf("{\"user_id\":%u,\"template_count\":%zu,\"templates\":[",
		       user_id, templates.count);
		for (size_t i = 0; i < templates.count; i++)
			printf("%s{\"group\":%u,\"id\":%u}", i ? "," : "",
			       templates.entries[i].group, templates.entries[i].id);
		puts("]}");
	} else if (!result && verify) {
		const char *outcome =
			verification.result == GOODIX_RESULT_MATCH ? "match" :
			verification.result == GOODIX_RESULT_NO_MATCH ? "no_match" :
			interrupted ? "canceled" : "timeout";

		if (verification.result == GOODIX_RESULT_MATCH)
			printf("{\"user_id\":%u,\"result\":\"match\",\"id\":%u}\n",
			       user_id, verification.finger);
		else
			printf("{\"user_id\":%u,\"result\":\"%s\"}\n", user_id, outcome);
		return verification.result == GOODIX_RESULT_MATCH ? 0 : 1;
	} else if (!result && auth_ready)
		puts("{\"sensor\":\"Goodix\",\"sensor_initialized\":true,"
		     "\"keymaster_binding_ready\":true}");
	else if (!result && initialize)
		printf("{\"sensor\":\"Goodix\",\"sensor_type\":%u,"
		       "\"chip_type\":%u,\"max_templates\":%u,\"rows\":%u,\"columns\":%u,"
		       "\"nav_rows\":%u,\"nav_columns\":%u,"
		       "\"model_required\":%u,\"anti_spoof_required\":%u,"
		       "\"models_loaded\":%s,\"sensor_initialized\":true}\n",
		       geometry.sensor_type, geometry.chip_type, geometry.max_templates, geometry.rows,
		       geometry.columns, geometry.nav_rows, geometry.nav_columns,
		       geometry.model_required, geometry.anti_spoof_required,
		       geometry.models_loaded ? "true" : "false");
	else if (!result)
		printf("{\"sensor\":\"Goodix\",\"sensor_type\":%u,"
		       "\"command_address_bits\":%u,\"device_info_ok\":true}\n",
		       sensor_type, client.architecture == 2 ? 64 : 32);
	if (result > 0)
		fprintf(stderr, "Fingerprint operation failed: the TA returned %s (%d).\n",
			goodix_error_name(result), result);
	else if (result)
		fprintf(stderr, "Fingerprint operation failed: %s (%d)\n",
			strerror(-result), result);
	return result ? 1 : 0;
}
