// SPDX-License-Identifier: BSD-3-Clause
/* Exercise the actual metadata and POSIX I/O implementation, without opening
 * the controller, registering FS listeners or invoking a secure application.
 */
#include "../../linux/userspace/fingerprint/piano-goodix.c"
#include "../../linux/userspace/fingerprint/piano-goodix-storage.c"
#include "../../linux/userspace/fingerprint/piano-goodix-rpmb.c"
#include <assert.h>
#include <stdarg.h>
#include <sys/resource.h>

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...);

static const char *firmware_parameter;
static const char *firmware_tree;
static int firmware_lookup;

int __wrap_open(const char *path, int flags, ...)
{
	char redirected[PATH_MAX];
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list args;

		va_start(args, flags);
		mode = va_arg(args, int);
		va_end(args);
	}
	if (!strcmp(path, "/sys/module/firmware_class/parameters/path")) {
		assert(firmware_lookup && firmware_parameter);
		path = firmware_parameter;
	} else if (!strncmp(path, "/lib/firmware/", 14)) {
		assert(firmware_lookup && firmware_tree);
		int bytes = snprintf(redirected, sizeof(redirected), "%s/%s",
				     firmware_tree, path + 14);

		assert(bytes >= 0 && (size_t)bytes < sizeof(redirected));
		path = redirected;
	}
	return __real_open(path, flags, mode);
}

static void firmware_file(int directory, const char *path, const char *contents)
{
	int fd = openat(directory, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	assert(fd >= 0);
	assert(write(fd, contents, strlen(contents)) == (ssize_t)strlen(contents));
	assert(!close(fd));
}

static void firmware_contents(const char *name, const char *expected)
{
	char contents[32] = {0};
	int fd = open_firmware(name);

	assert(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
	assert(read(fd, contents, sizeof(contents)) == (ssize_t)strlen(expected));
	assert(!strcmp(contents, expected));
	close(fd);
}

static void firmware_paths(int directory, const char *path)
{
	char parameter[PATH_MAX], custom[PATH_MAX], tree[PATH_MAX], version[PATH_MAX];
	struct utsname system;

	assert(!uname(&system));
	assert(snprintf(parameter, sizeof(parameter), "%s/parameter", path) > 0);
	assert(snprintf(custom, sizeof(custom), "%s/custom", path) > 0);
	assert(snprintf(tree, sizeof(tree), "%s/firmware", path) > 0);
	assert(!mkdirat(directory, "custom", 0700));
	assert(!mkdirat(directory, "firmware", 0700));
	assert(!mkdirat(directory, "firmware/updates", 0700));
	assert(snprintf(version, sizeof(version), "firmware/%s", system.release) > 0);
	assert(!mkdirat(directory, version, 0700));
	assert(snprintf(version, sizeof(version), "firmware/updates/%s", system.release) > 0);
	assert(!mkdirat(directory, version, 0700));
	firmware_parameter = parameter;
	firmware_tree = tree;
	firmware_lookup = 1;

	firmware_file(directory, "firmware/gfpia.elf", "default");
	firmware_file(directory, "firmware/updates/gfpia.elf", "updates");
	strcat(version, "/gfpia.elf");
	firmware_file(directory, version, "versioned-updates");
	assert(snprintf(custom, sizeof(custom), "%s/custom\n", path) > 0);
	firmware_file(directory, "parameter", custom);
	firmware_file(directory, "custom/gfpia.elf", "custom");
	firmware_contents("gfpia.elf", "custom");
	assert(!unlinkat(directory, "custom/gfpia.elf", 0));
	firmware_contents("gfpia.elf", "versioned-updates");
	assert(!unlinkat(directory, version, 0));
	firmware_contents("gfpia.elf", "updates");
	assert(!unlinkat(directory, "firmware/updates/gfpia.elf", 0));
	assert(snprintf(version, sizeof(version), "firmware/%s/gfpia.elf", system.release) > 0);
	firmware_file(directory, version, "versioned");
	firmware_contents("gfpia.elf", "versioned");
	assert(!unlinkat(directory, version, 0));
	firmware_contents("gfpia.elf", "default");
	firmware_file(directory, "parameter", "\n");
	firmware_contents("gfpia.elf", "default");
	assert(!unlinkat(directory, "parameter", 0));
	firmware_contents("gfpia.elf", "default");

	firmware_file(directory, "parameter", custom);
	assert(!symlinkat("../firmware/gfpia.elf", directory, "custom/gfpia.elf"));
	assert(open_firmware("gfpia.elf") == -ELOOP);
	assert(!unlinkat(directory, "custom/gfpia.elf", 0));
	assert(!mkfifoat(directory, "custom/piano-goodix-cap-s.bin", 0600));
	uint8_t bytes[8];
	assert(read_model("piano-goodix-cap-s.bin", bytes, sizeof(bytes)) == -EMSGSIZE);
	assert(open_firmware("../gfpia.elf") == -EINVAL);
	assert(open_firmware("/gfpia.elf") == -EINVAL);
	assert(open_firmware(".") == -EINVAL && open_firmware("..") == -EINVAL);
	firmware_file(directory, "parameter", "relative\n");
	assert(open_firmware("gfpia.elf") == -EINVAL);
	char excessive[257];
	memset(excessive, '/', sizeof(excessive) - 1);
	excessive[256] = 0;
	firmware_file(directory, "parameter", excessive);
	assert(open_firmware("gfpia.elf") == -ENAMETOOLONG);
	assert(!unlinkat(directory, "parameter", 0));
	assert(open_firmware("missing-model.bin") == -ENOENT);
	firmware_lookup = 0;
}

static struct linux_profile example(void)
{
	return (struct linux_profile){
		.linux_uid = 1000, .group = 0x400003e8,
		.sid = UINT64_C(0x123456789abcdef0), .templates = {11, 22},
	};
}

static void check_same(int directory, const struct linux_profile *expected)
{
	struct linux_profile loaded = { .linux_uid = expected->linux_uid, .group = expected->group };

	assert(!profile_load(directory, &loaded));
	assert(!memcmp(&loaded, expected, sizeof(loaded)));
}

static void roundtrip(int directory)
{
	struct linux_profile profile = example();

	profile.pending_finger = 3;
	profile.pending_id = 33;
	assert(!profile_save(directory, &profile));
	check_same(directory, &profile);
	struct stat st;
	assert(!fstatat(directory, "profile", &st, AT_SYMLINK_NOFOLLOW));
	assert(S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600 && st.st_nlink == 1);

	/* The format is fixed-width little endian, independent of structure padding. */
	uint8_t bytes[PROFILE_RECORD_BYTES];
	int fd = openat(directory, "profile", O_RDONLY | O_CLOEXEC);
	assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == sizeof(bytes));
	close(fd);
	assert(bytes[8] == 0xe8 && bytes[9] == 3 && bytes[12] == 0xe8 && bytes[15] == 0x40);
	assert(bytes[16] == 0xf0 && bytes[23] == 0x12 && bytes[24] == 3 && bytes[28] == 33);
}

static void recovery(int directory)
{
	struct profile_session session = { .directory = directory, .profile = example() };
	session.profile.pending_finger = 3;
	session.profile.pending_id = 33;
	assert(!profile_save(directory, &session.profile));
	struct template_list list = { .count = 4, .entries = {
		{0x400003e8, 11}, {0x400003e8, 33},
		{0x400003e8, 999}, /* Unknown templates remain unregistered. */
		{0, 22},         /* Another group cannot preserve our stale ID. */
	} };
	assert(!profile_reconcile_list(&session, &list));
	assert(session.profile.templates[0] == 11 && !session.profile.templates[1]);
	assert(session.profile.templates[2] == 33 && !session.profile.pending_id &&
	       !session.profile.pending_finger);
	for (size_t i = 3; i < 10; ++i)
		assert(!session.profile.templates[i]);
	check_same(directory, &session.profile);

	/* A save interrupted before 1012 clears only that exact pending record. */
	session.profile.pending_finger = 4;
	session.profile.pending_id = 44;
	assert(!profile_save(directory, &session.profile));
	assert(!profile_reconcile_list(&session, &list));
	assert(!session.profile.templates[3] && !session.profile.pending_id);
	check_same(directory, &session.profile);
}

static void boundaries(int directory)
{
	struct linux_profile profile = example(), loaded = example();

	assert(!profile_save(directory, &profile));
	assert(!fchmodat(directory, "profile", 0644, 0));
	assert(profile_load(directory, &loaded) == -EACCES);
	assert(!fchmodat(directory, "profile", 0600, 0));
	assert(!linkat(directory, "profile", directory, "linked", 0));
	assert(profile_load(directory, &loaded) == -EACCES);
	assert(!unlinkat(directory, "linked", 0));
	assert(!renameat(directory, "profile", directory, "original"));
	assert(!symlinkat("original", directory, "profile"));
	assert(profile_load(directory, &loaded) == -ELOOP);
	assert(!unlinkat(directory, "profile", 0));
	assert(!mkfifoat(directory, "profile", 0600));
	assert(profile_load(directory, &loaded) == -EACCES);
	assert(!unlinkat(directory, "profile", 0));
	assert(!renameat(directory, "original", directory, "profile"));
	loaded.linux_uid++;
	assert(profile_load(directory, &loaded) == -EBADMSG);

	profile.templates[1] = profile.templates[0];
	assert(!profile_save(directory, &profile));
	loaded = example();
	assert(profile_load(directory, &loaded) == -EBADMSG);
	profile = example();
	profile.pending_id = 33;
	assert(!profile_save(directory, &profile));
	assert(profile_load(directory, &loaded) == -EBADMSG);
	profile = example();
	profile.sid = 0;
	assert(!profile_save(directory, &profile));
	assert(profile_load(directory, &loaded) == -EBADMSG);
}

static void failures(int directory)
{
	struct linux_profile original = example(), replacement = example();
	struct rlimit previous, limit;

	assert(!profile_save(directory, &original));
	replacement.templates[2] = 33;
	assert(!getrlimit(RLIMIT_FSIZE, &previous));
	limit = previous;
	limit.rlim_cur = 16;
	signal(SIGXFSZ, SIG_IGN);
	assert(!setrlimit(RLIMIT_FSIZE, &limit));
	assert(profile_save(directory, &replacement) == -EFBIG);
	assert(!setrlimit(RLIMIT_FSIZE, &previous));
	check_same(directory, &original);
}

static void initialization_failure(int directory)
{
	struct linux_profile empty = { .linux_uid = 1000, .group = 0x400003e8 };
	struct profile_session session = { .directory = directory, .profile = empty };
	struct rlimit previous, limit;

	/* Exercise a real failed first metadata write after both private
	 * subdirectories exist. No controller or secure application is opened.
	 */
	assert(!getrlimit(RLIMIT_FSIZE, &previous));
	limit = previous;
	limit.rlim_cur = 16;
	signal(SIGXFSZ, SIG_IGN);
	assert(!setrlimit(RLIMIT_FSIZE, &limit));
	assert(profile_open_files(&session, 1) == -EFBIG);
	assert(!setrlimit(RLIMIT_FSIZE, &previous));
	assert(session.gatekeeper_directory >= 0);
	close(session.gatekeeper_directory);
	assert(profile_load(directory, &session.profile) == -ENOENT);

	/* Retrying initializes the existing directories and leaves a readable
	 * empty profile even if the subsequent sensor or Gatekeeper step fails.
	 */
	assert(!profile_open_files(&session, 1));
	close(session.gatekeeper_directory);
	check_same(directory, &empty);
	assert(!profile_open_files(&session, 0));
	close(session.gatekeeper_directory);
	check_same(directory, &empty);

	/* Opening for another enrollment never replaces a saved identity or
	 * interrupted template transaction with the initial empty record.
	 */
	struct linux_profile saved = example();
	saved.pending_finger = 3;
	saved.pending_id = 33;
	assert(!profile_save(directory, &saved));
	session.profile = empty;
	assert(!profile_open_files(&session, 1));
	close(session.gatekeeper_directory);
	assert(!memcmp(&session.profile, &saved, sizeof(saved)));
	check_same(directory, &saved);
}

static int queried(void *context)
{
	return *(int *)context;
}

static void cancellation(void)
{
	volatile sig_atomic_t signal_flag = 0;
	int thread_flag = 0;
	struct piano_goodix_options options = {
		.cancelled = &signal_flag, .should_cancel = queried, .context = &thread_flag,
	};
	assert(!cancellation_requested(NULL) && !cancellation_requested(&options));
	thread_flag = 1;
	assert(cancellation_requested(&options));
	thread_flag = 0;
	signal_flag = 1;
	assert(cancellation_requested(&options));
}

struct keymaster_fixture {
	struct goodix_storage storage;
	int calls, object_error, reply_bytes;
	int32_t status;
};

static int32_t native_keymaster(ObjectCxt context, ObjectOp operation,
				ObjectArg *args, ObjectCounts counts)
{
	struct keymaster_fixture *fixture = context;

	assert(operation == 0 && counts == ObjectCounts_pack(1, 1, 0, 0));
	assert(fixture->storage.allow_rpmb);
	fixture->calls++;
	if (fixture->object_error)
		return fixture->object_error;
	assert(args[1].b.size >= sizeof(fixture->status));
	memcpy(args[1].b.ptr, &fixture->status, sizeof(fixture->status));
	args[1].b.size = fixture->reply_bytes;
	return Object_OK;
}

static size_t gatekeeper_request(uint8_t bytes[32], uint32_t opcode, uint32_t group)
{
	QCBOREncodeContext encoder;
	UsefulBufC encoded;

	opcode = htole32(opcode);
	memcpy(bytes, &opcode, sizeof(opcode));
	QCBOREncode_Init(&encoder, (UsefulBuf){bytes + 4, 28});
	QCBOREncode_OpenMap(&encoder);
	QCBOREncode_AddUInt64ToMapN(&encoder, 3, group);
	QCBOREncode_CloseMap(&encoder);
	assert(!QCBOREncode_Finish(&encoder, &encoded));
	return encoded.len + 4;
}

static void gatekeeper_window(void)
{
	struct keymaster_fixture fixture = {
		.storage.mutex = PTHREAD_MUTEX_INITIALIZER, .reply_bytes = 4,
	};
	struct profile_session session = {
		.storage = &fixture.storage, .profile = example(),
		.keymaster = {native_keymaster, &fixture},
	};
	uint8_t request[32] = {0}, response[32] = {0};
	size_t actual;
	int32_t status;
	const uint32_t allowed[] = {0x21001, 0x21002};
	const uint32_t refused[] = {0x21003, 0x21004, 0x205};

	for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
		size_t length = gatekeeper_request(request, allowed[i], session.profile.group);
		assert(!gatekeeper_command(&session, request, length, response,
					   sizeof(response), &actual, &status));
		assert(!fixture.storage.allow_rpmb && actual == 4 && status == 0);
	}
	int calls = fixture.calls;
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
		size_t length = gatekeeper_request(request, refused[i], session.profile.group);
		assert(gatekeeper_command(&session, request, length, response,
					  sizeof(response), &actual, &status) == -EPERM);
		assert(!fixture.storage.allow_rpmb && fixture.calls == calls);
	}
	size_t length = gatekeeper_request(request, 0x21002, session.profile.group + 1);
	assert(gatekeeper_command(&session, request, length, response,
				  sizeof(response), &actual, &status) == -EPERM);
	session.profile.group = 0;
	length = gatekeeper_request(request, 0x21002, 0);
	assert(gatekeeper_command(&session, request, length, response,
				  sizeof(response), &actual, &status) == -EPERM);
	assert(!fixture.storage.allow_rpmb && fixture.calls == calls);
	session.profile = example();
	length = gatekeeper_request(request, 0x21002, session.profile.group);
	fixture.status = -30;
	assert(!gatekeeper_command(&session, request, length, response,
				   sizeof(response), &actual, &status));
	assert(status == -30 && !fixture.storage.allow_rpmb);
	fixture.object_error = Object_ERROR;
	assert(gatekeeper_command(&session, request, length, response,
				  sizeof(response), &actual, &status) == -EIO);
	assert(!fixture.storage.allow_rpmb);
	fixture.object_error = 0;
	fixture.reply_bytes = 3;
	assert(gatekeeper_command(&session, request, length, response,
				  sizeof(response), &actual, &status) == -EMSGSIZE);
	assert(!fixture.storage.allow_rpmb);
	pthread_mutex_destroy(&fixture.storage.mutex);
}

/* These allocations are used only when uploading models. The packet test
 * selects the actual no-model initialization path and must never reach them.
 */
int goodix_tee_memory(Object root, size_t size, Object *memory)
{
	(void)root; (void)size; (void)memory;
	assert(0);
	return Object_ERROR;
}

int goodix_tee_memory_info(Object memory, void **address, size_t *size)
{
	(void)memory; (void)address; (void)size;
	assert(0);
	return Object_ERROR;
}

struct initialization_fixture {
	uint8_t payload[SHARED_SIZE];
	unsigned int calls;
	uint8_t policy;
};

static int32_t initialization_command(ObjectCxt context, ObjectOp operation,
				      ObjectArg *args, ObjectCounts counts)
{
	struct initialization_fixture *fixture = context;
	const struct goodix_request *request = args[0].bi.ptr;
	const uint32_t commands[] = {1001, 1005, 1109};
	const size_t sizes[] = {0x168, 0x74, 0xc0};

	assert(operation == 0 && counts == ObjectCounts_pack(4, 2, 4, 0));
	assert(fixture->calls < sizeof(commands) / sizeof(commands[0]));
	assert(request->command == commands[fixture->calls] && request->route == 1);
	assert(request->payload_size == sizes[fixture->calls]);
	if (request->command == 1001) {
		/* INIT replaces the old device-info buffer, including its policy. */
		memset(fixture->payload, 0, sizeof(fixture->payload));
		fixture->payload[0x20c] = fixture->policy ^ 0xff;
		uint32_t rows = 112, columns = 88;
		memcpy(fixture->payload + 0x70, &rows, sizeof(rows));
		memcpy(fixture->payload + 0x74, &columns, sizeof(columns));
	} else if (request->command == 1109) {
		assert(fixture->payload[0x6c] == fixture->policy);
		assert(!fixture->payload[0x20c]);
		for (size_t i = 0x6d; i < request->payload_size; ++i)
			assert(!fixture->payload[i]);
	}
	memset(args[5].b.ptr, 0, args[5].b.size);
	fixture->calls++;
	return Object_OK;
}

static void initialization_policy(void)
{
	const uint8_t policies[] = {0, 0x5a};

	for (size_t i = 0; i < sizeof(policies); ++i) {
		struct initialization_fixture fixture = {.policy = policies[i]};
		struct client client = {
			.app = {initialization_command, &fixture},
			.shared = fixture.payload, .shared_size = sizeof(fixture.payload),
			.architecture = 2,
		};
		struct sensor_geometry geometry = {.sensor_type = 5};
		fixture.payload[0x20c] = fixture.policy;
		assert(!initialize_sensor(&client, &geometry));
		assert(fixture.calls == 3 && client.sensor_started);
		assert(geometry.rows == 112 && geometry.columns == 88);
		assert(geometry.interaction_policy == fixture.policy);
	}
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	int directory = open(argv[2], O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	assert(directory >= 0);
	if (!strcmp(argv[1], "roundtrip")) roundtrip(directory);
	else if (!strcmp(argv[1], "recovery")) recovery(directory);
	else if (!strcmp(argv[1], "boundaries")) boundaries(directory);
	else if (!strcmp(argv[1], "failures")) failures(directory);
	else if (!strcmp(argv[1], "initialization-failure")) initialization_failure(directory);
	else if (!strcmp(argv[1], "cancellation")) cancellation();
	else if (!strcmp(argv[1], "gatekeeper-window")) gatekeeper_window();
	else if (!strcmp(argv[1], "initialization-policy")) initialization_policy();
	else if (!strcmp(argv[1], "firmware-paths")) firmware_paths(directory, argv[2]);
	else assert(0);
	close(directory);
	return 0;
}
