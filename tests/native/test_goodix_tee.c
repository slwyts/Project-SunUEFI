// SPDX-License-Identifier: BSD-3-Clause
/* Drive the actual ioctl framing and reference code at its syscall boundary.
 * File descriptors are ordinary memfds; no TEE, sensor or storage is opened.
 */
#include "../../linux/userspace/fingerprint/piano-goodix-tee.c"
#include <assert.h>
#include <stdarg.h>
#include <semaphore.h>

int __wrap_ioctl(int fd, unsigned long operation, ...);
static int mode, invokes, releases, sends;
static int response_error;
static int test_fd;
static sem_t receiving;
static uint64_t callback_id;
static struct tee_context *fixture;
static uint64_t input_value = UINT64_C(0x1020304050607080);
int __wrap_open(const char *path, int flags, ...);
int __real_open(const char *path, int flags, ...);

int __wrap_open(const char *path, int flags, ...)
{
	assert(!strcmp(path, "/dev/tee0") && (flags & O_CLOEXEC) && (flags & O_NOFOLLOW));
	test_fd = memfd_create("tee-open-test", MFD_CLOEXEC);
	return test_fd;
}

int __wrap_ioctl(int fd, unsigned long operation, ...)
{
	va_list arguments;
	va_start(arguments, operation);
	void *pointer = va_arg(arguments, void *);
	va_end(arguments);
	if (operation == TEE_IOC_VERSION) {
		struct tee_ioctl_version_data *version = pointer;

		assert(fd == test_fd);
		*version = (struct tee_ioctl_version_data){
			.impl_id = TEE_IMPL_ID_QTEE,
			.gen_caps = mode == 5 ? 0 : TEE_GEN_CAP_OBJREF,
		};
		return 0;
	}
	if (mode == 6 && operation == TEE_IOC_SUPPL_RECV) {
		assert(fd == test_fd);
		assert(!sem_post(&receiving));
		pause();
		return -1;
	}
	assert(fd == fixture->fd);
	if (operation == TEE_IOC_SHM_ALLOC) {
		struct tee_ioctl_shm_alloc_data *allocation = pointer;
		assert(!allocation->flags && allocation->size == 4096);
		int shared = memfd_create("tee-memory-test", MFD_CLOEXEC);
		assert(shared >= 0 && !ftruncate(shared, 4096));
		allocation->id = 9;
		return shared;
	}
	struct tee_ioctl_buf_data *data = pointer;
	if (operation == TEE_IOC_OBJECT_INVOKE) {
		struct tee_ioctl_object_invoke_arg *argument = (void *)(uintptr_t)data->buf_ptr;
		assert(data->buf_len == sizeof(*argument) + argument->num_params * sizeof(argument->params[0]));
		if (argument->op == Object_OP_release) {
			assert(!argument->num_params && argument->id == 42);
			releases++;
			return 0;
		}
		invokes++;
		if (mode == 1) {
			assert(argument->id == TEE_OBJREF_NULL && argument->op == 7 && argument->num_params == 4);
			assert(argument->params[0].attr == TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT);
			assert(argument->params[0].b == 8 && *(uint64_t *)(uintptr_t)argument->params[0].a == input_value);
			assert(argument->params[1].attr == TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT);
			assert(argument->params[2].b == REF_MEMORY && argument->params[2].a == 9);
			memcpy((void *)(uintptr_t)argument->params[1].a, "reply", 5);
			argument->params[1].b = 5;
			argument->params[3].a = 42;
			argument->params[3].b = REF_TEE;
		} else if (mode == 7) {
			assert(argument->num_params == 2);
			argument->params[0].b++;
			argument->params[1].a = 42;
			argument->params[1].b = REF_TEE;
		} else if (mode == 8) {
			argument->ret = 30000;
		} else {
			assert(argument->num_params == 1 && argument->params[0].b == REF_USER);
			callback_id = argument->params[0].a;
			if (mode == 4) {
				errno = EIO;
				return -1;
			}
		}
		return 0;
	}
	union {
		struct tee_iocl_supp_recv_arg receive;
		struct tee_iocl_supp_send_arg send;
	} *header = (void *)(uintptr_t)data->buf_ptr;
	struct tee_ioctl_param *parameters = (void *)(header + 1);
	if (operation == TEE_IOC_SUPPL_RECV) {
		assert(parameters[0].attr == (TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT | TEE_IOCTL_PARAM_ATTR_META));
		uint8_t *input = (void *)(uintptr_t)parameters[0].a;
		parameters[0].a = callback_id;
		parameters[0].b = 17;
		if (mode == 3) {
			header->receive.func = Object_OP_release;
			header->receive.num_params = 1;
		} else {
			header->receive.func = 11;
			header->receive.num_params = 3;
			memcpy(input, &input_value, sizeof(input_value));
			parameters[1] = (struct tee_ioctl_param){
				.attr = TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT,
				.a = (uintptr_t)input, .b = 8,
			};
			parameters[2] = (struct tee_ioctl_param){
				.attr = TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT,
				.b = mode == 9 ? CALLBACK_BYTES + 1 : 16,
			};
		}
		return 0;
	}
	assert(operation == TEE_IOC_SUPPL_SEND);
	assert(parameters[0].attr == (TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT | TEE_IOCTL_PARAM_ATTR_META));
	assert(parameters[0].a == 17);
	if (mode == 9) {
		assert(header->send.ret == Object_ERROR_INVALID && header->send.num_params == 1);
	} else {
		assert(!header->send.ret && header->send.num_params == 3);
		assert(parameters[2].b == 6 && !memcmp((void *)(uintptr_t)parameters[2].a, "answer", 6));
	}
	sends++;
	if (response_error) {
		errno = EIO;
		return -1;
	}
	return 0;
}

struct local { int references, calls; };
static int32_t local_invoke(ObjectCxt data, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	struct local *local = data;
	if (op == Object_OP_retain) { local->references++; return 0; }
	if (op == Object_OP_release) { local->references--; return 0; }
	assert(op == 11 && counts == ObjectCounts_pack(1, 1, 0, 0));
	assert(args[0].b.size == 8 && !memcmp(args[0].b.ptr, &input_value, 8));
	assert(args[1].b.size == 16);
	memcpy(args[1].b.ptr, "answer", 6);
	args[1].b.size = 6;
	local->calls++;
	return 0;
}

int main(void)
{
	Object opened;
	mode = 5;
	assert(goodix_tee_open(&opened) == -EOPNOTSUPP && Object_isNull(opened));
	assert(fcntl(test_fd, F_GETFD) == -1 && errno == EBADF);
	struct tee_context context = {.fd = memfd_create("tee-test", MFD_CLOEXEC)};
	assert(context.fd >= 0 && !pthread_mutex_init(&context.mutex, NULL));
	atomic_init(&context.references, 1);
	fixture = &context;
	Object root = object_new(&context, ROOT, TEE_OBJREF_NULL), memory;
	assert(!goodix_tee_memory(root, 4096, &memory));
	void *address; size_t bytes;
	assert(!goodix_tee_memory_info(memory, &address, &bytes) && bytes == 4096);
	memset(address, 0x5a, bytes);
	char output[16];
	ObjectArg args[] = {{.bi = {&input_value, 8}}, {.b = {output, sizeof(output)}},
		{.o = memory}, {.o = Object_NULL}};
	mode = 1;
	assert(!Object_invoke(root, 7, args, ObjectCounts_pack(1, 1, 1, 1)));
	assert(args[1].b.size == 5 && !memcmp(output, "reply", 5));
	Object_release(args[3].o);
	assert(releases == 1);
	ObjectArg oversized[] = {{.b = {output, sizeof(output)}}, {.o = Object_NULL}};
	mode = 7;
	assert(Object_invoke(root, 7, oversized, ObjectCounts_pack(0, 1, 0, 1)) == -EMSGSIZE);
	assert(Object_isNull(oversized[1].o) && releases == 2);
	mode = 8;
	assert(Object_invoke(root, 7, NULL, 0) == 30000);

	struct local local = {.references = 1};
	Object callback = {local_invoke, &local};
	ObjectArg input[] = {{.o = callback}};
	mode = 2;
	assert(!Object_invoke(root, 3, input, ObjectCounts_pack(0, 0, 1, 0)));
	assert(local.references == 2 && callback_id && context.callbacks);
	assert(!callback_request(&context, 0));
	assert(local.calls == 1 && sends == 1 && local.references == 2);
	mode = 9;
	assert(!callback_request(&context, 0));
	assert(local.calls == 1 && sends == 2 && local.references == 2);
	mode = 3;
	assert(!callback_request(&context, 0));
	assert(local.references == 1 && !context.callbacks && sends == 2);

	/* An ioctl error does not prove that QTEE failed to retain its input. */
	mode = 4;
	assert(Object_invoke(root, 3, input, ObjectCounts_pack(0, 0, 1, 0)) == -EIO);
	assert(local.references == 2 && context.callbacks && atomic_load(&context.failed));
	callback_remove(&context, callback_id);
	assert(local.references == 1 && !context.callbacks);
	Object_release(memory);
	assert(!context.memories && invokes == 5);
	/* No worker was created for this syscall-boundary test. */
	free(root.context);
	assert(atomic_fetch_sub(&context.references, 1) == 2);
	close(context.fd);
	pthread_mutex_destroy(&context.mutex);
	/* Exercise actual worker creation, cancellation in blocked RECV and join. */
	mode = 6;
	assert(!sem_init(&receiving, 0, 0));
	assert(!goodix_tee_open(&opened));
	assert(!sem_wait(&receiving));
	struct tee_object *opened_root = opened.context;
	fixture = opened_root->context;
	struct tee_ioctl_param parameter = {0};
	uint64_t exported = 0;
	assert(!object_encode(fixture, callback, &parameter, &exported));
	assert(local.references == 2 && exported);
	assert(!Object_release(opened));
	assert(local.references == 1);
	assert(fcntl(test_fd, F_GETFD) == -1 && errno == EBADF);
	assert(!sem_destroy(&receiving));
	return 0;
}
