// SPDX-License-Identifier: BSD-2-Clause-Patent
#define _GNU_SOURCE
#include "piano_f2fs_grow.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#define JSMN_STATIC
#include "../../android/native/jsmn.h"

#define EARLY_DIR "/mnt/vendor/persist/sunuefi/storage"
#define EARLY_TOOL EARLY_DIR "/piano-resize-f2fs"
#define EARLY_CHECK EARLY_DIR "/fsck.f2fs"
#define EARLY_RESIZE EARLY_DIR "/resize.f2fs"
#define EARLY_REQUEST EARLY_DIR "/grow.pending"
#define EARLY_ATTEMPT EARLY_DIR "/grow.attempt"
#define EARLY_LOG EARLY_DIR "/grow.log"
#define EARLY_RC "/metadata/sunuefi/storage-early.rc"
#define RECEIPT "/data/adb/piano-sunuefi/storage/android-expansion.json"
#define RC_TEXT \
    "on late-fs\n" \
    "    exec u:r:ksu:s0 root -- " EARLY_TOOL " early-grow --execute\n" \
    "\non post-fs-data\n" \
    "    write /dev/.sunuefi-storage-late 1\n"
#define PATH_BYTES 4096

struct grow_request {
    char magic[16];
    uint64_t expected;
    unsigned char uuid[16];
    char job[33];
    char boot[37];
};

static int secure_dir(const char *path) {
    struct stat st;
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid || chmod(path, 0700)) return -1;
    return 0;
}

static int sync_parent(const char *path) {
    char parent[PATH_BYTES];
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent)) return -1;
    char *slash = strrchr(parent, '/');
    if (!slash) return -1;
    *slash = 0;
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int ret = fd < 0 ? -1 : fsync(fd);
    if (fd >= 0) close(fd);
    return ret;
}

static int write_all(int fd, const void *bytes, size_t size) {
    const char *at = bytes;
    while (size) {
        ssize_t count = write(fd, at, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        at += count;
        size -= (size_t)count;
    }
    return 0;
}

static int atomic_file(const char *path, const void *bytes, size_t size, mode_t mode) {
    char temporary[PATH_BYTES];
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) return -1;
    int fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) return -1;
    int ret = fchmod(fd, mode) || write_all(fd, bytes, size) || fsync(fd);
    close(fd);
    if (!ret) ret = rename(temporary, path) || sync_parent(path);
    if (ret) unlink(temporary);
    return ret ? -1 : 0;
}

static int copy_file(const char *from, const char *to, mode_t mode) {
    int input = open(from, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (input < 0 || fstat(input, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 32 << 20) {
        if (input >= 0) close(input);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    char *data = malloc(size ? size : 1);
    if (!data) { close(input); return -1; }
    size_t done = 0;
    while (done < size) {
        ssize_t count = read(input, data + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += (size_t)count;
    }
    close(input);
    int ret = done != size ? -1 : atomic_file(to, data, size, mode);
    free(data);
    return ret;
}

static int label_file(const char *path, const char *context) {
    return setxattr(path, "security.selinux", context, strlen(context) + 1, 0);
}

static int sibling_tool(const char *name, char source[PATH_BYTES]) {
    ssize_t size = readlink("/proc/self/exe", source, PATH_BYTES - 1);
    if (size < 0 || size >= PATH_BYTES - 1) return -1;
    source[size] = 0;
    char *slash = strrchr(source, '/');
    if (!slash || snprintf(slash + 1, PATH_BYTES - (size_t)(slash + 1 - source), "%s", name) >=
        (int)(PATH_BYTES - (size_t)(slash + 1 - source))) return -1;
    return 0;
}

static int copy_f2fs_tool(const char *name, const char *target) {
    char source[PATH_BYTES];
    if (sibling_tool(name, source)) return -1;
    if (strcmp(source, target) && copy_file(source, target, 0700)) return -1;
    return label_file(target, "u:object_r:system_file:s0");
}

int piano_f2fs_install_early(void) {
    if (secure_dir("/mnt/vendor/persist/sunuefi") || secure_dir(EARLY_DIR) ||
        secure_dir("/metadata/sunuefi") || copy_file("/proc/self/exe", EARLY_TOOL, 0700) ||
        copy_f2fs_tool("fsck.f2fs", EARLY_CHECK) || copy_f2fs_tool("resize.f2fs", EARLY_RESIZE) ||
        label_file(EARLY_TOOL, "u:object_r:system_file:s0") ||
        atomic_file(EARLY_RC, RC_TEXT, sizeof(RC_TEXT) - 1, 0600) ||
        label_file("/metadata/sunuefi", "u:object_r:metadata_file:s0") ||
        label_file(EARLY_RC, "u:object_r:metadata_file:s0")) return -1;
    puts("{\"operation\":\"f2fs-early-install\",\"installed\":true,\"writes_gpt\":false}");
    return 0;
}

static int regular_root_file(const char *path, off_t size) {
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) &&
        (size < 0 || st.st_size == size);
}

static int same_file_contents(const char *left, const char *right) {
    int a = open(left, O_RDONLY | O_CLOEXEC), b = open(right, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char first[65536], second[65536];
    int equal = a >= 0 && b >= 0;
    while (equal) {
        ssize_t size = read(a, first, sizeof(first));
        if (size < 0) { equal = 0; break; }
        size_t done = 0;
        while (done < (size_t)size) {
            ssize_t n = read(b, second + done, (size_t)size - done);
            if (n <= 0) break;
            done += (size_t)n;
        }
        if (done != (size_t)size || memcmp(first, second, done)) { equal = 0; break; }
        if (!size) {
            equal = read(b, second, 1) == 0;
            break;
        }
    }
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    return equal;
}

int piano_f2fs_early_ready(void) {
    char check[PATH_BYTES], resize[PATH_BYTES];
    if (!regular_root_file(EARLY_TOOL, -1) || access(EARLY_TOOL, X_OK) ||
        !regular_root_file(EARLY_CHECK, -1) || access(EARLY_CHECK, X_OK) ||
        !regular_root_file(EARLY_RESIZE, -1) || access(EARLY_RESIZE, X_OK) ||
        sibling_tool("fsck.f2fs", check) || sibling_tool("resize.f2fs", resize) ||
        !same_file_contents(check, EARLY_CHECK) || !same_file_contents(resize, EARLY_RESIZE) ||
        !same_file_contents("/proc/self/exe", EARLY_TOOL) ||
        !regular_root_file(EARLY_RC, sizeof(RC_TEXT) - 1)) return -1;
    int fd = open(EARLY_RC, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char contents[sizeof(RC_TEXT)];
    ssize_t n = fd < 0 ? -1 : read(fd, contents, sizeof(contents));
    if (fd >= 0) close(fd);
    return n == sizeof(RC_TEXT) - 1 && !memcmp(contents, RC_TEXT, sizeof(RC_TEXT) - 1) ? 0 : -1;
}

static int json_field(const char *json, const jsmntok_t *tokens, int count,
                      const char *key, jsmntype_t type, char *value, size_t capacity) {
    int found = 0;
    for (int i = 1; i + 1 < count; i += 2) {
        const jsmntok_t *name = &tokens[i], *data = &tokens[i + 1];
        if (name->type != JSMN_STRING || data->type == JSMN_OBJECT || data->type == JSMN_ARRAY) return -1;
        if ((size_t)(name->end - name->start) != strlen(key) || memcmp(json + name->start, key, strlen(key))) continue;
        size_t size = (size_t)(data->end - data->start);
        if (found || data->type != type || size >= capacity) return -1;
        memcpy(value, json + data->start, size);
        value[size] = 0;
        found = 1;
    }
    return found ? 0 : -1;
}

static int receipt_request(const char *json, size_t size, uint64_t expected, struct grow_request *request) {
    char value[96];
    jsmn_parser parser; jsmntok_t tokens[64];
    jsmn_init(&parser);
    int count = jsmn_parse(&parser, json, size, tokens, 64);
    if (count < 1 || !(count & 1) || tokens[0].size * 2 + 1 != count ||
        tokens[0].type != JSMN_OBJECT || tokens[0].end < 0 ||
        (size_t)tokens[0].end > size ||
        json_field(json, tokens, count, "schema_version", JSMN_PRIMITIVE, value, sizeof(value)) || strcmp(value, "1") ||
        json_field(json, tokens, count, "gpt_written", JSMN_PRIMITIVE, value, sizeof(value)) || strcmp(value, "true") ||
        json_field(json, tokens, count, "pending_android_expansion", JSMN_PRIMITIVE, value, sizeof(value)) || strcmp(value, "true") ||
        json_field(json, tokens, count, "target_userdata_bytes", JSMN_PRIMITIVE, value, sizeof(value))) return -1;
    for (size_t i = (size_t)tokens[0].end; i < size; ++i)
        if (!strchr(" \t\r\n", json[i])) return -1;
    char *end;
    errno = 0;
    uint64_t target = strtoull(value, &end, 10);
    if (!value[0] || strspn(value, "0123456789") != strlen(value) || errno || *end || target != expected ||
        json_field(json, tokens, count, "job_id", JSMN_STRING, request->job, sizeof(request->job)) ||
        strlen(request->job) != 32 || strspn(request->job, "0123456789abcdef") != 32) return -1;
    return 0;
}

int piano_f2fs_prepare_grow(const char *mapper, const char *userdata, uint64_t expected) {
    if (piano_f2fs_early_ready() || !regular_root_file(RECEIPT, -1)) return -1;
    int fd = open(RECEIPT, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char json[4096];
    ssize_t size = fd < 0 ? -1 : read(fd, json, sizeof(json));
    if (fd >= 0) close(fd);
    if (size <= 0 || size == sizeof(json)) return -1;
    struct grow_request request = { .magic = "SUNUEFI-GROWv1", .expected = expected };
    if (receipt_request(json, (size_t)size, expected, &request)) return -1;
    fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, request.boot, 36);
    if (fd >= 0) close(fd);
    uint64_t blocks, section;
    /* The current mapper may still have the pre-GPT length until reboot. */
    if (n != 36 || piano_f2fs_geometry(mapper, userdata, 0, request.uuid, &blocks, &section)) return -1;
    if (expected < blocks * 4096 || expected % 4096) return -1;
    if (atomic_file(EARLY_REQUEST, &request, sizeof(request), 0600)) return -1;
    puts("{\"operation\":\"f2fs-grow\",\"status\":\"prepared\",\"offline\":true,\"reboot_required\":true}");
    return 0;
}

static int command(char *const args[], int confirmed) {
    int input[2] = {-1, -1};
    if (confirmed && pipe2(input, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        if (input[0] >= 0) { close(input[0]); close(input[1]); }
        return -1;
    }
    if (!pid) {
        if (confirmed) {
            close(input[1]);
            if (dup2(input[0], STDIN_FILENO) < 0) _exit(127);
            close(input[0]);
        }
        execv(args[0], args);
        _exit(127);
    }
    if (confirmed) {
        close(input[0]);
        /* The original user action already confirmed this exact expansion. */
        struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
        sigaction(SIGPIPE, &ignore, &previous);
        (void)write(input[1], "y\n", 2);
        sigaction(SIGPIPE, &previous, NULL);
        close(input[1]);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int stage_tools(char directory[PATH_BYTES]) {
    strcpy(directory, "/dev/sunuefi-f2fs.XXXXXX");
    if (!mkdtemp(directory)) return -1;
    char path[PATH_BYTES];
    const char *files[] = {EARLY_CHECK, EARLY_RESIZE};
    const char *names[] = {"fsck.f2fs", "resize.f2fs"};
    for (size_t i = 0; i < 2; ++i) {
        if (snprintf(path, sizeof(path), "%s/%s", directory, names[i]) >= (int)sizeof(path) ||
            copy_file(files[i], path, 0700)) return -1;
    }
    return 0;
}

static int data_mount(char mapper[PATH_BYTES], char options[8192], unsigned long *flags) {
    FILE *stream = fopen("/proc/mounts", "r");
    if (!stream) return -1;
    char device[PATH_BYTES], path[PATH_BYTES], type[64], settings[8192], line[16384];
    int found = 0;
    while (fgets(line, sizeof(line), stream)) {
        if (sscanf(line, "%4095s %4095s %63s %8191s", device, path, type, settings) != 4) continue;
        if (strcmp(path, "/data")) continue;
        if (found || strcmp(type, "f2fs") || !realpath(device, mapper)) { found = -1; break; }
        found = 1;
        *flags = 0;
        options[0] = 0;
        char *save = NULL;
        for (char *token = strtok_r(settings, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
            if (!strcmp(token, "rw") || !strcmp(token, "seclabel")) continue;
            if (!strcmp(token, "ro")) { found = -1; break; }
            unsigned long bit = !strcmp(token, "nosuid") ? MS_NOSUID : !strcmp(token, "nodev") ? MS_NODEV :
                !strcmp(token, "noexec") ? MS_NOEXEC : !strcmp(token, "noatime") ? MS_NOATIME :
                !strcmp(token, "nodiratime") ? MS_NODIRATIME : !strcmp(token, "relatime") ? MS_RELATIME :
                !strcmp(token, "lazytime") ? MS_LAZYTIME : 0;
            if (bit) { *flags |= bit; continue; }
            if (options[0]) strcat(options, ",");
            strcat(options, token);
        }
    }
    fclose(stream);
    return found == 1 ? 0 : -1;
}

static _Noreturn void reboot_android(void) {
    fsync(STDERR_FILENO);
    sync();
    reboot(RB_AUTOBOOT);
    fprintf(stderr, "SunUEFI: Android reboot did not complete (%s). Hold the power button to restart; /data remains offline.\n", strerror(errno));
    fsync(STDERR_FILENO);
    /* Do not let init create data directories on an unmounted /data. */
    for (;;) pause();
}

int piano_f2fs_early_grow(void) {
    if (access(EARLY_REQUEST, F_OK)) return errno == ENOENT ? 0 : 1;
    if (getuid() || getppid() != 1 || !regular_root_file(EARLY_REQUEST, sizeof(struct grow_request))) return 1;
    int log = open(EARLY_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (log < 0) return 1;
    dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); close(log);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "SunUEFI: checking confirmed Android expansion in late-fs.\n");
    int fd = open(EARLY_REQUEST, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct grow_request request;
    ssize_t n = fd < 0 ? -1 : read(fd, &request, sizeof(request));
    if (fd >= 0) close(fd);
    if (n != sizeof(request) || memcmp(request.magic, "SUNUEFI-GROWv1", 15) || !request.expected ||
        request.expected % 4096 || request.job[32] || request.boot[36] ||
        strspn(request.job, "0123456789abcdef") != 32) return 1;
    /* Consume before maintenance: neither failures nor reboots repeat writes. */
    if (rename(EARLY_REQUEST, EARLY_ATTEMPT) || sync_parent(EARLY_REQUEST)) return 1;
    char mapper[PATH_BYTES], userdata[PATH_BYTES], options[8192], stage[PATH_BYTES];
    unsigned long flags = 0;
    uint64_t before, section, after;
    unsigned char uuid[16];
    struct timespec uptime;
    char boot[37] = {0};
    fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    n = fd < 0 ? -1 : read(fd, boot, 36);
    if (fd >= 0) close(fd);
    if (clock_gettime(CLOCK_BOOTTIME, &uptime) || uptime.tv_sec >= 60 ||
        n != 36 || !strcmp(boot, request.boot) ||
        !access("/dev/.sunuefi-storage-late", F_OK) ||
        !realpath("/dev/block/by-name/userdata", userdata) || data_mount(mapper, options, &flags) ||
        piano_f2fs_geometry(mapper, userdata, request.expected, uuid, &before, &section) ||
        piano_f2fs_layout_capacity(mapper, request.expected) ||
        memcmp(uuid, request.uuid, sizeof(uuid))) {
        errno = EINVAL;
        goto refused;
    }
    if (request.expected - before * 4096 < section * 4096) {
        fprintf(stderr, "SunUEFI: Android capacity already covers the requested space.\n");
        return 0;
    }
    if (stage_tools(stage)) goto refused;
    char tool[PATH_BYTES], check[PATH_BYTES];
    if (snprintf(tool, sizeof(tool), "%s/resize.f2fs", stage) >= (int)sizeof(tool) ||
        snprintf(check, sizeof(check), "%s/fsck.f2fs", stage) >= (int)sizeof(check)) goto refused;
    char *probe[] = {tool, "-V", NULL};
    if (command(probe, 0)) goto refused;
    sync();
    if (chdir("/dev") || umount2("/data", 0)) goto refused;
    fd = open(mapper, O_RDWR | O_EXCL | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "SunUEFI: plaintext device is still busy; no resize started.\n");
        goto restore;
    }
    close(fd);
    fprintf(stderr, "SunUEFI: userdata is offline; checking the existing filesystem.\n");
    char *check_args[] = {check, "--dry-run", "--no-kernel-check", "-N", mapper, NULL};
    int result = command(check_args, 0);
    if (result) {
        fprintf(stderr, "SunUEFI: filesystem check failed (%d); no expansion started.\n", result);
        goto restore;
    }
    char *plan_args[] = {tool, "-P", mapper, NULL};
    if (command(plan_args, 1)) {
        fprintf(stderr, "SunUEFI: the existing metadata cannot cover the requested capacity; no expansion started.\n");
        goto restore;
    }
    fprintf(stderr, "SunUEFI: expanding within the existing layout; file blocks will not move.\n");
    char *args[] = {tool, "-k", mapper, NULL};
    result = command(args, 1);
    sync();
    if (result || piano_f2fs_geometry(mapper, userdata, request.expected, uuid, &after, &section) ||
        memcmp(uuid, request.uuid, sizeof(uuid)) || after < before ||
        request.expected - after * 4096 >= section * 4096) {
        fprintf(stderr, "SunUEFI: expansion or readback failed (%d); returning through Android's filesystem check.\n", result);
        reboot_android();
    }
    result = command(check_args, 0);
    if (result) {
        fprintf(stderr, "SunUEFI: expanded filesystem check failed (%d); returning through Android's filesystem check.\n", result);
        reboot_android();
    }
    fprintf(stderr, "SunUEFI: both F2FS superblocks cover %" PRIu64 " bytes; UUID preserved.\n", after * 4096);
restore:
    if (mount(mapper, "/data", "f2fs", flags, options)) {
        fprintf(stderr, "SunUEFI: cannot restore the original /data mount: %s. Rebooting Android once.\n", strerror(errno));
        reboot_android();
    }
    fprintf(stderr, "SunUEFI: original Android data mount restored; continuing boot.\n");
    fsync(STDERR_FILENO);
    return 0;
refused:
    fprintf(stderr, "SunUEFI: expansion not started (%s); Android data remains mounted.\n", strerror(errno));
    fsync(STDERR_FILENO);
    return 1;
}
