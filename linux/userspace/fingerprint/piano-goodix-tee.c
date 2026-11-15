// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 slwyts <slwyts@foxmail.com>
 * QTEE object flags and callback meta fields follow the upstream qcomtee
 * driver. No GlobalPlatform session or private kernel interface is used.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/tee.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <qcbor/qcbor_encode.h>

#include "piano-goodix-tee.h"

enum { REF_TEE = 1, REF_USER = 2, REF_MEMORY = 4, PARAMS = 60, CALLBACK_BYTES = 4096 };
enum object_kind { ROOT, REMOTE, MEMORY };
struct tee_context;

struct tee_object {
	atomic_uint references;
	struct tee_context *context;
	enum object_kind kind;
	uint64_t id;
	int fd;
	void *address;
	size_t bytes;
	struct tee_object *next;
};

struct callback {
	uint64_t id;
	Object object;
	struct callback *next;
};

struct tee_context {
	atomic_uint references;
	atomic_int stopping, failed;
	int fd;
	pthread_t thread;
	pthread_mutex_t mutex;
	uint64_t next_callback;
	struct callback *callbacks;
	struct tee_object *memories;
};

static int32_t object_invoke(ObjectCxt, ObjectOp, ObjectArg *, ObjectCounts);
static void context_put(struct tee_context *context);

static void context_shutdown(struct tee_context *context)
{
	if (atomic_exchange(&context->stopping, 1))
		return;
	/* ROOT is held by the operation entry, never by a callback. No invocation
	 * may still be active when its last ROOT reference is released.
	 */
	pthread_cancel(context->thread);
	pthread_join(context->thread, NULL);
	close(context->fd);
	context->fd = -1;
	atomic_store(&context->failed, 1);
	/* Closing the standard TEE context stops user-object requests. Kernel
	 * memory-object references keep shared pages alive until QTEE releases
	 * them; unmapping a local fd cannot free those pages prematurely.
	 */
	while (context->callbacks) {
		struct callback *callback = context->callbacks;

		context->callbacks = callback->next;
		Object_release(callback->object);
		free(callback);
		context_put(context);
	}
}

static void context_get(struct tee_context *context)
{
	atomic_fetch_add(&context->references, 1);
}

static int tee_call(struct tee_context *context, unsigned long operation, void *argument)
{
	if (atomic_load(&context->failed))
		return -ENODEV;
	return ioctl(context->fd, operation, argument) < 0 ? -errno : 0;
}

static Object object_new(struct tee_context *context, enum object_kind kind, uint64_t id)
{
	struct tee_object *object = calloc(1, sizeof(*object));

	if (!object)
		return Object_NULL;
	atomic_init(&object->references, 1);
	object->context = context;
	object->kind = kind;
	object->id = id;
	object->fd = -1;
	context_get(context);
	return (Object){object_invoke, object};
}

static void remote_release(struct tee_context *context, uint64_t id)
{
	struct tee_ioctl_object_invoke_arg argument = { .id = id, .op = Object_OP_release };
	struct tee_ioctl_buf_data data = { .buf_ptr = (uintptr_t)&argument, .buf_len = sizeof(argument) };

	/* Never replay an invocation, including release, after a transport error. */
	if (tee_call(context, TEE_IOC_OBJECT_INVOKE, &data))
		atomic_store(&context->failed, 1);
}

static void callback_remove(struct tee_context *context, uint64_t id)
{
	struct callback *removed = NULL;

	pthread_mutex_lock(&context->mutex);
	for (struct callback **entry = &context->callbacks; *entry; entry = &(*entry)->next) {
		if ((*entry)->id == id) {
			removed = *entry;
			*entry = removed->next;
			break;
		}
	}
	pthread_mutex_unlock(&context->mutex);
	if (removed) {
		Object_release(removed->object);
		free(removed);
		context_put(context);
	}
}

static Object callback_find(struct tee_context *context, uint64_t id)
{
	Object object = Object_NULL;

	pthread_mutex_lock(&context->mutex);
	for (struct callback *entry = context->callbacks; entry; entry = entry->next) {
		if (entry->id == id) {
			object = entry->object;
			Object_retain(object);
			break;
		}
	}
	pthread_mutex_unlock(&context->mutex);
	return object;
}

static int object_encode(struct tee_context *context, Object object,
			 struct tee_ioctl_param *parameter, uint64_t *exported)
{
	parameter->a = TEE_OBJREF_NULL;
	if (Object_isNull(object))
		return 0;
	if (object.invoke == object_invoke) {
		struct tee_object *handle = object.context;

		if (handle->context != context || handle->kind == ROOT)
			return -EINVAL;
		parameter->a = handle->id;
		parameter->b = handle->kind == MEMORY ? REF_MEMORY : REF_TEE;
		return 0;
	}
	struct callback *callback = calloc(1, sizeof(*callback));

	if (!callback)
		return -ENOMEM;
	if (Object_retain(object)) {
		free(callback);
		return -EINVAL;
	}
	callback->object = object;
	context_get(context);
	pthread_mutex_lock(&context->mutex);
	if (context->next_callback == UINT64_MAX) {
		pthread_mutex_unlock(&context->mutex);
		Object_release(object);
		free(callback);
		context_put(context);
		return -EOVERFLOW;
	}
	callback->id = ++context->next_callback;
	callback->next = context->callbacks;
	context->callbacks = callback;
	pthread_mutex_unlock(&context->mutex);
	parameter->a = callback->id;
	parameter->b = REF_USER;
	*exported = callback->id;
	return 0;
}

static Object object_decode(struct tee_context *context, const struct tee_ioctl_param *parameter)
{
	if (parameter->a == TEE_OBJREF_NULL)
		return Object_NULL;
	if (parameter->b == REF_TEE)
		return object_new(context, REMOTE, parameter->a);
	if (parameter->b == REF_USER)
		return callback_find(context, parameter->a);
	Object object = Object_NULL;

	if (parameter->b != REF_MEMORY)
		return object;
	pthread_mutex_lock(&context->mutex);
	for (struct tee_object *memory = context->memories; memory; memory = memory->next) {
		if (memory->id == parameter->a) {
			atomic_fetch_add(&memory->references, 1);
			object = (Object){object_invoke, memory};
			break;
		}
	}
	pthread_mutex_unlock(&context->mutex);
	return object;
}

static int invoke_remote(struct tee_object *object, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	_Alignas(8) uint8_t message[sizeof(struct tee_ioctl_object_invoke_arg) +
		PARAMS * sizeof(struct tee_ioctl_param)] = {0};
	struct tee_ioctl_object_invoke_arg *argument = (void *)message;
	struct tee_ioctl_param *parameters = argument->params;
	struct tee_context *context = object->context;
	uint64_t exported[PARAMS] = {0};
	unsigned int bi = ObjectCounts_numBI(counts), bo = ObjectCounts_numBO(counts);
	unsigned int oi = ObjectCounts_numOI(counts), total = ObjectCounts_total(counts);
	int result = -EINVAL;

	if (counts & ~65535U || total > PARAMS || (total && !args) || op >= Object_OP_retain)
		return -EINVAL;
	argument->id = object->id;
	argument->op = op;
	argument->num_params = total;
	for (unsigned int i = 0; i < total; i++) {
		struct tee_ioctl_param *parameter = &parameters[i];

		if (i < bi + bo) {
			if (args[i].b.size && !args[i].b.ptr) {
				result = -EINVAL;
				goto rollback;
			}
			parameter->attr = i < bi ? TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT :
				TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT;
			parameter->a = (uintptr_t)args[i].b.ptr;
			parameter->b = args[i].b.size;
		} else if (i < bi + bo + oi) {
			parameter->attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT;
			result = object_encode(context, args[i].o, parameter, &exported[i]);
			if (result)
				goto rollback;
		} else {
			parameter->attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT;
			args[i].o = Object_NULL;
		}
	}
	struct tee_ioctl_buf_data data = {
		.buf_ptr = (uintptr_t)message,
		.buf_len = sizeof(*argument) + total * sizeof(parameters[0]),
	};
	result = tee_call(context, TEE_IOC_OBJECT_INVOKE, &data);
	if (result) {
		/* A failed syscall may follow QTEE retaining an exported callback.
		 * Keep it until context shutdown; never guess that it was unused.
		 */
		atomic_store(&context->failed, 1);
		return result;
	}
	if (argument->ret)
		return (int32_t)argument->ret;
	result = 0;
	for (unsigned int i = bi; i < bi + bo; i++) {
		if (parameters[i].b > args[i].b.size)
			result = -EMSGSIZE;
		else
			args[i].b.size = parameters[i].b;
	}
	for (unsigned int i = bi + bo + oi; i < total; i++) {
		args[i].o = object_decode(context, &parameters[i]);
		if (parameters[i].a != TEE_OBJREF_NULL && Object_isNull(args[i].o)) {
			if (parameters[i].b == REF_TEE)
				remote_release(context, parameters[i].a);
			result = -ENOMEM;
		}
	}
	if (result)
		for (unsigned int i = bi + bo + oi; i < total; i++) {
			Object_RELEASE_IF(args[i].o);
			args[i].o = Object_NULL;
		}
	return result;
rollback:
	for (unsigned int i = 0; i < total; i++)
		if (exported[i])
			callback_remove(context, exported[i]);
	return result;
}

static int32_t object_invoke(ObjectCxt data, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	struct tee_object *object = data;
	struct tee_context *context = object->context;

	if (op == Object_OP_retain) {
		atomic_fetch_add(&object->references, 1);
		return 0;
	}
	if (op != Object_OP_release)
		return object->kind == MEMORY ? -EINVAL : invoke_remote(object, op, args, counts);
	pthread_mutex_lock(&context->mutex);
	if (atomic_fetch_sub(&object->references, 1) != 1) {
		pthread_mutex_unlock(&context->mutex);
		return 0;
	}
	if (object->kind == MEMORY) {
		for (struct tee_object **entry = &context->memories; *entry; entry = &(*entry)->next)
			if (*entry == object) {
				*entry = object->next;
				break;
			}
	}
	pthread_mutex_unlock(&context->mutex);
	if (object->kind == ROOT)
		context_shutdown(context);
	if (object->kind == REMOTE)
		remote_release(context, object->id);
	if (object->kind == MEMORY) {
		munmap(object->address, object->bytes);
		close(object->fd);
	}
	free(object);
	context_put(context);
	return 0;
}

static int callback_request(struct tee_context *context, int cancellable)
{
	union {
		struct tee_iocl_supp_recv_arg receive;
		struct tee_iocl_supp_send_arg send;
	} *header;
	_Alignas(8) uint8_t message[sizeof(*header) + (PARAMS + 1) * sizeof(struct tee_ioctl_param)] = {0};
	_Alignas(8) uint8_t input[CALLBACK_BYTES] = {0};
	ObjectArg args[PARAMS] = {0};
	void *allocated[PARAMS] = {0};
	size_t capacities[PARAMS] = {0};
	uint64_t exported[PARAMS] = {0};
	unsigned int bi = 0, bo = 0, oi = 0, oo = 0, total;
	int result;
	Object callback = Object_NULL;

	header = (void *)message;
	struct tee_ioctl_param *parameters = (void *)(message + sizeof(*header));
	struct tee_ioctl_buf_data data = { .buf_ptr = (uintptr_t)message, .buf_len = sizeof(message) };

	header->receive.num_params = PARAMS + 1;
	parameters[0] = (struct tee_ioctl_param){
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT | TEE_IOCTL_PARAM_ATTR_META,
		.a = (uintptr_t)input, .b = sizeof(input),
	};
	/* ROOT shutdown only cancels RECV, outside callback dispatch and all
	 * userspace locks. Closing the context afterwards terminates any request
	 * dequeued exactly as cancellation arrives.
	 */
	if (cancellable) {
		pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	}
	result = ioctl(context->fd, TEE_IOC_SUPPL_RECV, &data);
	if (cancellable)
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	if (result < 0)
		return -errno;
	if (!header->receive.num_params || header->receive.num_params > PARAMS + 1)
		return -EPROTO;
	if (parameters[0].attr !=
	    (TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT | TEE_IOCTL_PARAM_ATTR_META))
		return -EPROTO;
	uint64_t id = parameters[0].a, request = parameters[0].b;
	uint32_t op = header->receive.func;
	total = header->receive.num_params - 1;
	if (op == Object_OP_release) {
		callback_remove(context, id);
		return 0; /* Kernel release notifications do not need SUPPL_SEND. */
	}
	callback = callback_find(context, id);
	result = Object_isNull(callback) ? Object_ERROR_BADOBJ : 0;
	for (unsigned int i = 0; i < total; i++) {
		struct tee_ioctl_param *parameter = &parameters[i + 1];
		switch (parameter->attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT:
			if (bo || oi || oo || ++bi > 15 || parameter->a < (uintptr_t)input ||
			    parameter->a - (uintptr_t)input > sizeof(input) ||
			    parameter->b > sizeof(input) - (parameter->a - (uintptr_t)input)) {
				result = Object_ERROR_INVALID;
				break;
			}
			args[i].b.ptr = (void *)(uintptr_t)parameter->a;
			args[i].b.size = parameter->b;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT:
			if (oi || oo || ++bo > 15 || parameter->b > CALLBACK_BYTES) {
				result = Object_ERROR_INVALID;
				break;
			}
			allocated[i] = calloc(1, parameter->b ? parameter->b : 1);
			capacities[i] = parameter->b;
			args[i].b.ptr = allocated[i];
			args[i].b.size = parameter->b;
			if (!allocated[i])
				result = Object_ERROR_MEM;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT:
			if (oo || ++oi > 15)
				result = Object_ERROR_INVALID;
			args[i].o = object_decode(context, parameter);
			if (parameter->a != TEE_OBJREF_NULL && Object_isNull(args[i].o)) {
				if (parameter->b == REF_TEE)
					remote_release(context, parameter->a);
				result = Object_ERROR_MEM;
			}
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT:
			if (++oo > 15)
				result = Object_ERROR_INVALID;
			break;
		default:
			result = Object_ERROR_INVALID;
		}
	}
	if (!result)
		result = Object_invoke(callback, op, args, ObjectCounts_pack(bi, bo, oi, oo));
	if (!result)
		for (unsigned int i = 0; i < total; i++) {
			struct tee_ioctl_param *parameter = &parameters[i + 1];
			if (parameter->attr == TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT) {
				if (args[i].b.size > parameter->b || (args[i].b.size && !args[i].b.ptr)) {
					result = Object_ERROR_INVALID;
					break;
				}
				parameter->a = (uintptr_t)args[i].b.ptr;
				parameter->b = args[i].b.size;
			} else if (parameter->attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT) {
				if (object_encode(context, args[i].o, parameter, &exported[i])) {
					result = Object_ERROR_MEM;
					break;
				}
			}
		}
	header->send.ret = result;
	header->send.num_params = result ? 1 : total + 1;
	parameters[0] = (struct tee_ioctl_param){
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT | TEE_IOCTL_PARAM_ATTR_META,
		.a = request,
	};
	data.buf_len = sizeof(*header) + header->send.num_params * sizeof(parameters[0]);
	int sent = ioctl(context->fd, TEE_IOC_SUPPL_SEND, &data);
	if (sent < 0)
		atomic_store(&context->failed, 1);
	for (unsigned int i = 0; i < total; i++) {
		if (exported[i] && result)
			callback_remove(context, exported[i]);
		if (parameters[i + 1].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT)
			Object_RELEASE_IF(args[i].o);
		if (parameters[i + 1].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT)
			Object_RELEASE_IF(args[i].o);
		if (allocated[i]) {
			explicit_bzero(allocated[i], capacities[i]);
			free(allocated[i]);
		}
	}
	Object_RELEASE_IF(callback);
	explicit_bzero(input, sizeof(input));
	return 0;
}

static void context_destroy(struct tee_context *context)
{
	if (context->fd >= 0)
		close(context->fd);
	pthread_mutex_destroy(&context->mutex);
	free(context);
}

static void *callback_worker(void *data)
{
	struct tee_context *context = data;

	/* The operation thread keeps its caller's signal mask. Only this private
	 * callback worker is excluded from process-level cancellation signals.
	 */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	while (!atomic_load(&context->stopping)) {
		int result = callback_request(context, 1);

		if (result && result != -EINTR) {
			atomic_store(&context->failed, 1);
			break;
		}
	}
	return NULL;
}

static void context_put(struct tee_context *context)
{
	if (atomic_fetch_sub(&context->references, 1) != 1)
		return;
	/* ROOT initiates shutdown before releasing its context reference. */
	context_destroy(context);
}

int goodix_tee_open(Object *output)
{
	struct tee_ioctl_version_data version = {0};
	struct tee_context *context = calloc(1, sizeof(*context));
	int result = -ENOMEM;

	*output = Object_NULL;
	if (!context)
		return result;
	context->fd = open("/dev/tee0", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (context->fd < 0) {
		result = -errno;
		goto failed;
	}
	if (ioctl(context->fd, TEE_IOC_VERSION, &version) < 0) {
		result = -errno;
		goto failed;
	}
	if (version.impl_id != TEE_IMPL_ID_QTEE || !(version.gen_caps & TEE_GEN_CAP_OBJREF)) {
		result = -EOPNOTSUPP;
		goto failed;
	}
	result = pthread_mutex_init(&context->mutex, NULL);
	if (result) {
		result = -result;
		goto failed;
	}
	atomic_init(&context->references, 0);
	atomic_init(&context->stopping, 0);
	atomic_init(&context->failed, 0);
	Object root = object_new(context, ROOT, TEE_OBJREF_NULL);
	if (Object_isNull(root)) {
		result = -ENOMEM;
		pthread_mutex_destroy(&context->mutex);
		goto failed;
	}
	sigset_t blocked, previous;
	sigfillset(&blocked);
	result = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
	if (result) {
		free(root.context);
		pthread_mutex_destroy(&context->mutex);
		result = -result;
		goto failed;
	}
	result = pthread_create(&context->thread, NULL, callback_worker, context);
	pthread_sigmask(SIG_SETMASK, &previous, NULL);
	if (result) {
		free(root.context);
		pthread_mutex_destroy(&context->mutex);
		result = -result;
		goto failed;
	}
	*output = root;
	return 0;
failed:
	if (context->fd >= 0)
		close(context->fd);
	free(context);
	return result;
}

struct credentials {
	atomic_uint references;
	uint8_t encoded[64];
	size_t bytes;
};

static int32_t credentials_invoke(ObjectCxt data, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	struct credentials *credentials = data;

	if (op == Object_OP_retain) {
		atomic_fetch_add(&credentials->references, 1);
		return 0;
	}
	if (op == Object_OP_release) {
		if (atomic_fetch_sub(&credentials->references, 1) == 1) {
			explicit_bzero(credentials, sizeof(*credentials));
			free(credentials);
		}
		return 0;
	}
	if (!op && counts == ObjectCounts_pack(0, 1, 0, 0) && args[0].b.size >= sizeof(size_t)) {
		memcpy(args[0].b.ptr, &credentials->bytes, sizeof(size_t));
		args[0].b.size = sizeof(size_t);
		return 0;
	}
	if (op == 1 && counts == ObjectCounts_pack(1, 1, 0, 0) && args[0].b.size == sizeof(uint64_t)) {
		uint64_t offset;
		memcpy(&offset, args[0].b.ptr, sizeof(offset));
		if (offset > credentials->bytes)
			return Object_ERROR_INVALID;
		size_t bytes = credentials->bytes - offset;
		if (bytes > args[1].b.size)
			bytes = args[1].b.size;
		memcpy(args[1].b.ptr, credentials->encoded + offset, bytes);
		args[1].b.size = bytes;
		return 0;
	}
	return Object_ERROR_INVALID;
}

int goodix_tee_client(Object root, Object *environment)
{
	struct credentials *credentials = calloc(1, sizeof(*credentials));
	struct timeval time;
	QCBOREncodeContext encoder;
	UsefulBufC encoded;

	*environment = Object_NULL;
	if (root.invoke != object_invoke || ((struct tee_object *)root.context)->kind != ROOT) {
		free(credentials);
		return -EINVAL;
	}
	if (!credentials)
		return -ENOMEM;
	atomic_init(&credentials->references, 1);
	Object callback = {credentials_invoke, credentials};
	int result = -EIO;
	if (gettimeofday(&time, NULL))
		goto done;
	QCBOREncode_Init(&encoder, (UsefulBuf){credentials->encoded, sizeof(credentials->encoded)});
	QCBOREncode_OpenMap(&encoder);
	QCBOREncode_AddUInt64ToMapN(&encoder, 1, getuid());
	QCBOREncode_AddInt64ToMapN(&encoder, 6, (int64_t)time.tv_sec * 1000 + time.tv_usec / 1000);
	QCBOREncode_CloseMap(&encoder);
	if (QCBOREncode_Finish(&encoder, &encoded))
		goto done;
	credentials->bytes = encoded.len;
	ObjectArg args[] = {{.o = callback}, {.o = Object_NULL}};
	result = Object_invoke(root, 2, args, ObjectCounts_pack(0, 0, 1, 1));
	if (!result)
		*environment = args[1].o;
done:
	Object_release(callback);
	return result;
}

int goodix_tee_memory(Object root, size_t bytes, Object *output)
{
	*output = Object_NULL;
	if (root.invoke != object_invoke || !bytes)
		return -EINVAL;
	struct tee_object *owner = root.context;
	struct tee_context *context = owner->context;
	if (owner->kind != ROOT || atomic_load(&context->failed))
		return -ENODEV;
	struct tee_ioctl_shm_alloc_data allocation = { .size = bytes };
	int fd = ioctl(context->fd, TEE_IOC_SHM_ALLOC, &allocation);

	if (fd < 0)
		return -errno;
	int result = -EMSGSIZE;
	if (allocation.size < bytes || allocation.size > SIZE_MAX || allocation.id < 0)
		goto failed;
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		result = -errno;
		goto failed;
	}
	void *address = mmap(NULL, allocation.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (address == MAP_FAILED) {
		result = -errno;
		goto failed;
	}
	Object object = object_new(context, MEMORY, allocation.id);
	if (Object_isNull(object)) {
		result = -ENOMEM;
		munmap(address, allocation.size);
		goto failed;
	}
	struct tee_object *memory = object.context;
	memory->fd = fd;
	memory->address = address;
	memory->bytes = allocation.size;
	pthread_mutex_lock(&context->mutex);
	memory->next = context->memories;
	context->memories = memory;
	pthread_mutex_unlock(&context->mutex);
	*output = object;
	return 0;
failed:
	close(fd);
	return result;
}

int goodix_tee_memory_info(Object object, void **address, size_t *bytes)
{
	if (object.invoke != object_invoke)
		return -EINVAL;
	struct tee_object *memory = object.context;
	if (memory->kind != MEMORY)
		return -EINVAL;
	*address = memory->address;
	*bytes = memory->bytes;
	return 0;
}
