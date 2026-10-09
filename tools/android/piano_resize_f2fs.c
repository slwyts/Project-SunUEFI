// SPDX-License-Identifier: BSD-2-Clause-Patent
// Online F2FS shrinking on Android. Never writes a GPT or a raw block device.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>

#define F2FS_MAGIC 0xf2f52010U
#define F2FS_IOC_RESIZE_FS _IOW(0xf5, 16, uint64_t)
#define BLOCK_BYTES 4096U

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static int supers(int fd, uint64_t *count, uint64_t *section) {
    unsigned char a[64], b[64];
    if (pread(fd, a, sizeof(a), 1024) != (ssize_t)sizeof(a) ||
        pread(fd, b, sizeof(b), BLOCK_BYTES + 1024) != (ssize_t)sizeof(b)) return -1;
    if (memcmp(a, b, sizeof(a)) || le32(a) != F2FS_MAGIC || le32(a + 16) != 12 ||
        le32(a + 8) < 9 || le32(a + 8) > 12 || le32(a + 12) != 12 - le32(a + 8) ||
        le32(a + 20) != 9 || !le32(a + 24)) { errno = EINVAL; return -1; }
    *count = le64(a + 36);
    *section = UINT64_C(512) * le32(a + 24);
    if (!*count) { errno = EINVAL; return -1; }
    return 0;
}
static int path_block(const char *path, struct stat *st) {
    return !stat(path, st) && S_ISBLK(st->st_mode);
}
static int backing_device(const char *text, dev_t expected) {
    unsigned major_id, minor_id; int length = 0;
    if (sscanf(text, "%u:%u%n", &major_id, &minor_id, &length) == 2 && text[length] == 0)
        return makedev(major_id, minor_id) == expected;
    struct stat st;
    return !strncmp(text, "/dev/block/", 11) && path_block(text, &st) && st.st_rdev == expected;
}
// Read the device-mapper table only to prove a full-length, zero-offset mapping.
// Parameters can contain an encryption key: they are never logged or exported.
static int mapper_is_userdata(const char *mapper, dev_t backing, uint64_t bytes) {
    const char *base = strrchr(mapper, '/');
    if (!base || strncmp(++base, "dm-", 3)) return 0;
    char sysfs[256], name[128], slave_path[256];
    snprintf(sysfs, sizeof(sysfs), "/sys/class/block/%s/dm/name", base);
    FILE *file = fopen(sysfs, "r");
    if (!file) return 0;
    int named = fgets(name, sizeof(name), file) != NULL;
    fclose(file);
    if (!named) return 0;
    name[strcspn(name, "\r\n")] = 0;
    if (strcmp(name, "userdata")) return 0;
    snprintf(sysfs, sizeof(sysfs), "/sys/class/block/%s/slaves", base);
    DIR *dir = opendir(sysfs);
    if (!dir) return 0;
    struct dirent *entry; unsigned slaves = 0; int same = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        ++slaves;
        if (strlen(entry->d_name) > 100) continue;
        snprintf(slave_path, sizeof(slave_path), "/dev/block/%.100s", entry->d_name);
        struct stat st;
        same = path_block(slave_path, &st) && st.st_rdev == backing;
    }
    closedir(dir);
    if (slaves != 1 || !same) return 0;

    int control = open("/dev/device-mapper", O_RDONLY | O_CLOEXEC);
    if (control < 0) return 0;
    unsigned char buffer[16384]; memset(buffer, 0, sizeof(buffer));
    struct dm_ioctl *query = (struct dm_ioctl *)buffer;
    query->version[0] = DM_VERSION_MAJOR;
    query->version[1] = DM_VERSION_MINOR;
    query->version[2] = DM_VERSION_PATCHLEVEL;
    query->data_size = sizeof(buffer); query->data_start = sizeof(*query);
    query->flags = DM_STATUS_TABLE_FLAG;
    strcpy(query->name, "userdata");
    int valid = 0;
    if (!ioctl(control, DM_TABLE_STATUS, query) && query->target_count == 1 &&
        !(query->flags & DM_BUFFER_FULL_FLAG) && query->data_start <= sizeof(buffer) - sizeof(struct dm_target_spec)) {
        struct dm_target_spec *target = (struct dm_target_spec *)(buffer + query->data_start);
        char *params = (char *)(target + 1);
        size_t room = sizeof(buffer) - (size_t)((unsigned char *)params - buffer);
        if (target->sector_start == 0 && target->length == bytes / 512 && memchr(params, 0, room)) {
            char *tokens[16], *save = NULL; size_t count = 0;
            for (char *token = strtok_r(params, " \t", &save); token && count < 16; token = strtok_r(NULL, " \t", &save))
                tokens[count++] = token;
            if (!strcmp(target->target_type, "linear") && count == 2)
                valid = backing_device(tokens[0], backing) && !strcmp(tokens[1], "0");
            if (!strcmp(target->target_type, "default-key") && count >= 5)
                valid = backing_device(tokens[3], backing) && !strcmp(tokens[4], "0");
        }
    }
    volatile unsigned char *secret = buffer;
    for (size_t i = 0; i < sizeof(buffer); ++i) secret[i] = 0;
    close(control);
    return valid;
}
static int mounted_data(char result[4096]) {
    FILE *file = fopen("/proc/mounts", "r");
    if (!file) return -1;
    char source[4096], mountpoint[4096], filesystem[64], rest[8192]; int found = 0;
    while (fscanf(file, "%4095s %4095s %63s", source, mountpoint, filesystem) == 3) {
        if (!fgets(rest, sizeof(rest), file)) break;
        if (!strcmp(mountpoint, "/data")) {
            if (!strcmp(filesystem, "f2fs") && !strncmp(source, "/dev/block/", 11) && realpath(source, result)) found = 1;
            break;
        }
    }
    fclose(file);
    return found ? 0 : -1;
}
static int decimal(const char *text, uint64_t *number) {
    char *end; errno = 0;
    if (!text[0] || text[0] < '0' || text[0] > '9') return -1;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || *end || !parsed) return -1;
    *number = parsed; return 0;
}
int main(int argc, char **argv) {
    int execute = 0; uint64_t limit = 0, expected_bytes = 0;
    if (argc == 2 && !strcmp(argv[1], "status")) { /* read-only */ }
    else if (argc == 4 && !strcmp(argv[1], "status") && !strcmp(argv[2], "--expect-bytes")) {
        if (decimal(argv[3], &expected_bytes) || expected_bytes % BLOCK_BYTES) return 2;
    }
    else if (argc == 4 && !strcmp(argv[1], "--limit-blocks") && !strcmp(argv[3], "--execute")) {
        if (decimal(argv[2], &limit)) return 2;
        execute = 1;
    } else if (argc == 5 && !strcmp(argv[1], "shrink") && !strcmp(argv[2], "--target-bytes") && !strcmp(argv[4], "--execute")) {
        uint64_t bytes;
        if (decimal(argv[3], &bytes) || bytes % BLOCK_BYTES) return 2;
        limit = bytes / BLOCK_BYTES; execute = 1;
    } else {
        fprintf(stderr, "Usage: piano-resize-f2fs status [--expect-bytes EXPECTED_PARTITION_BYTES] | shrink --target-bytes NEW_PARTITION_BYTES --execute\n");
        return 2;
    }
    if (getuid()) { fprintf(stderr, "Root permission required\n"); return 1; }
    char mapper_path[4096], userdata_path[4096];
    if (mounted_data(mapper_path) || !realpath("/dev/block/by-name/userdata", userdata_path)) {
        fprintf(stderr, "Cannot resolve mounted F2FS /data and userdata\n"); return 1;
    }
    int raw = open(userdata_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int mapper = open(mapper_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int data = open("/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat physical, mapped, mounted; struct statfs filesystem;
    uint64_t raw_bytes = 0, mapped_bytes = 0;
    if (raw < 0 || mapper < 0 || data < 0 || fstat(raw, &physical) || fstat(mapper, &mapped) ||
        fstat(data, &mounted) || fstatfs(data, &filesystem) || !S_ISBLK(physical.st_mode) ||
        !S_ISBLK(mapped.st_mode) || mounted.st_dev != mapped.st_rdev ||
        (uint32_t)filesystem.f_type != F2FS_MAGIC || filesystem.f_bsize != BLOCK_BYTES ||
        ioctl(raw, BLKGETSIZE64, &raw_bytes) || ioctl(mapper, BLKGETSIZE64, &mapped_bytes) ||
        !mapped_bytes || mapped_bytes > raw_bytes || mapped_bytes % BLOCK_BYTES || raw_bytes % BLOCK_BYTES) {
        fprintf(stderr, "F2FS /data device or userdata capacity differs\n"); return 1;
    }
    int direct = mapped.st_rdev == physical.st_rdev;
    if (!direct && !mapper_is_userdata(mapper_path, physical.st_rdev, mapped_bytes)) {
        fprintf(stderr, "Cannot verify a whole-userdata, zero-offset device-mapper table\n"); return 1;
    }
    uint64_t before, section;
    if (supers(mapper, &before, &section) || before > mapped_bytes / BLOCK_BYTES) {
        fprintf(stderr, "Mounted userdata plaintext F2FS superblocks differ\n"); return 1;
    }
    if (!execute) {
        // GPT growth can be committed while Android still uses its old mapper.
        // On the next normal boot vold recreates the complete plaintext mapping,
        // then the OEM fs_mgr invokes resize.f2fs before mounting /data. Neither
        // a larger GPT entry nor a larger mapper alone proves F2FS has grown.
        uint64_t filesystem_bytes = before * BLOCK_BYTES;
        int mapping_requires_reboot = mapped_bytes != raw_bytes;
        int grow_pending = raw_bytes - filesystem_bytes >= section * BLOCK_BYTES;
        int expansion_verified = expected_bytes && raw_bytes == expected_bytes &&
            mapped_bytes == expected_bytes && !grow_pending;
        printf("{\"operation\":\"f2fs-status\",\"read_only\":true,\"mapping_verified\":true,\"mapped\":%s,\"f2fs_block_count\":%" PRIu64
               ",\"userdata_bytes\":%" PRIu64 ",\"mapped_bytes\":%" PRIu64 ",\"f2fs_bytes\":%" PRIu64
               ",\"section_blocks\":%" PRIu64 ",\"both_superblocks_equal\":true,\"free_blocks\":%" PRIu64
               ",\"mapping_requires_reboot\":%s,\"grow_pending\":%s,\"expected_bytes\":%" PRIu64
               ",\"expansion_verified\":%s,\"gpt_written\":false}\n",
               direct ? "false" : "true", before, raw_bytes, mapped_bytes, filesystem_bytes,
               section, (uint64_t)filesystem.f_bfree, mapping_requires_reboot ? "true" : "false",
               grow_pending ? "true" : "false", expected_bytes, expansion_verified ? "true" : "false");
        close(data); close(mapper); close(raw); return expected_bytes && !expansion_verified ? 3 : 0;
    }
    if (mapped_bytes != raw_bytes) {
        fprintf(stderr, "Reboot Android to recreate the full userdata mapper before resizing\n"); return 1;
    }
    if (limit <= section * 2 || limit % section || limit > raw_bytes / BLOCK_BYTES) {
        fprintf(stderr, "New userdata size must fit the device and align to F2FS sections\n"); return 1;
    }
    uint64_t requested = limit - section, after = before, after_section = section;
    int performed = 0;
    if (before > limit) {
        if ((uint64_t)filesystem.f_bfree < before - requested) {
            fprintf(stderr, "Not enough F2FS free blocks for the requested shrink\n"); return 1;
        }
        if (ioctl(data, F2FS_IOC_RESIZE_FS, &requested)) { perror("F2FS_IOC_RESIZE_FS"); return 1; }
        performed = 1;
        if (syncfs(data)) { perror("sync F2FS /data"); return 1; }
        if (supers(mapper, &after, &after_section) || after_section != section || after > limit || after >= before) {
            fprintf(stderr, "Shrink superblock readback invalid; do not change GPT\n"); return 1;
        }
    }
    printf("{\"ioctl_success\":true,\"ioctl_performed\":%s,\"requested_blocks\":%" PRIu64
           ",\"partition_limit_blocks\":%" PRIu64 ",\"f2fs_block_count\":%" PRIu64
           ",\"both_superblocks_equal\":true,\"mapping_verified\":true,\"gpt_written\":false}\n",
           performed ? "true" : "false", requested, limit, after);
    close(data); close(mapper); close(raw); return 0;
}
