// SPDX-License-Identifier: BSD-2-Clause-Patent
// Source-only Android helper. No GPT/block writer, formatting, zeroing or force.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#define F2FS_MAGIC 0xf2f52010U
#define F2FS_IOC_RESIZE_FS _IOW(0xf5, 16, uint64_t)

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p) {
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}
static int supers(int fd, uint64_t *count, uint64_t *section) {
    unsigned char a[64], b[64];
    if (pread(fd, a, sizeof(a), 1024) != (ssize_t)sizeof(a) ||
        pread(fd, b, sizeof(b), 4096 + 1024) != (ssize_t)sizeof(b)) return -1;
    if (memcmp(a, b, sizeof(a)) || le32(a) != F2FS_MAGIC || le32(a + 16) != 12 ||
        le32(a + 8) < 9 || le32(a + 8) > 12 || le32(a + 12) != 12 - le32(a + 8) ||
        le32(a + 20) != 9 || le32(a + 24) != 1) {
        errno = EINVAL; return -1;
    }
    *count = le64(a + 36); *section = 512;
    return *count ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[1], "--limit-blocks") || strcmp(argv[3], "--execute")) {
        fprintf(stderr, "Usage: piano_resize_f2fs --limit-blocks NEW_PARTITION_4K_BLOCKS --execute\n");
        return 2;
    }
    char *end = NULL; errno = 0;
    unsigned long long parsed = strtoull(argv[2], &end, 10);
    if (errno || !end || *end || argv[2][0] < '0' || argv[2][0] > '9' || parsed <= 1024 || getuid() != 0) {
        fprintf(stderr, "Invalid limit or non-root caller\n"); return 2;
    }
    uint64_t limit = (uint64_t)parsed;
    char resolved[4096];
    if (!realpath("/dev/block/by-name/userdata", resolved) || strncmp(resolved, "/dev/block/", 11)) {
        perror("userdata identity"); return 1;
    }
    int raw = open(resolved, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int data = open("/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat block_stat, data_stat; struct statfs filesystem;
    if (raw < 0 || data < 0 || fstat(raw, &block_stat) || fstat(data, &data_stat) ||
        fstatfs(data, &filesystem) || !S_ISBLK(block_stat.st_mode) ||
        (uint32_t)filesystem.f_type != F2FS_MAGIC) {
        perror("F2FS /data/plaintext userdata"); return 1;
    }
    // A dm/encryption mapping requires its own verified provenance; never force.
    if (data_stat.st_dev != block_stat.st_rdev) {
        fprintf(stderr, "No validated direct /data-to-userdata mapping; refusing ioctl\n"); return 1;
    }
    uint64_t before, section;
    if (supers(raw, &before, &section) || limit >= before || limit % section) {
        fprintf(stderr, "Plaintext superblocks/section-aligned shrink limit invalid\n"); return 1;
    }
    // Kernel metadata is section based; keep one section of margin and verify
    // both on-disk counts rather than treating ioctl success as completion.
    uint64_t requested = limit - section;
    if (ioctl(data, F2FS_IOC_RESIZE_FS, &requested) != 0) {
        perror("F2FS_IOC_RESIZE_FS"); return 1;
    }
    if (fsync(data) != 0) { perror("sync /data"); return 1; }
    uint64_t after, after_section;
    if (supers(raw, &after, &after_section) || after_section != section || after > limit || after >= before) {
        fprintf(stderr, "Shrink readback invalid; GPT must remain unchanged\n"); return 1;
    }
    printf("{\"ioctl_success\":true,\"requested_blocks\":%" PRIu64
           ",\"partition_limit_blocks\":%" PRIu64 ",\"f2fs_block_count\":%" PRIu64
           ",\"both_superblocks_equal\":true,\"gpt_written\":false}\n", requested, limit, after);
    close(data);close(raw);return 0;
}
