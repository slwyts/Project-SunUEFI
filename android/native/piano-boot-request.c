// SPDX-License-Identifier: BSD-2-Clause-Patent
#define _GNU_SOURCE
#include "BootRequest.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE 4096u
#define BOOT_MAX (96u * 1024u * 1024u)
static const unsigned char split_magic[16] = "SUNUEFI-SPLITv1";
static const unsigned char request_magic[16] = "SUNUEFI-NEXTv1";
struct record {
  int valid;
  uint64_t sequence;
  uint32_t target;
  unsigned char page[PAGE];
};
static uint32_t le32(const unsigned char *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p) {
  return le32(p) | (uint64_t)le32(p + 4) << 32;
}
static void put32(unsigned char *p, uint32_t n) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (unsigned char)(n >> (8 * i));
}
static void put64(unsigned char *p, uint64_t n) {
  for (unsigned i = 0; i < 8; i++)
    p[i] = (unsigned char)(n >> (8 * i));
}
static int transfer(int fd, void *buffer, size_t bytes, off_t offset,
                    int writing) {
  unsigned char *p = buffer;
  while (bytes) {
    ssize_t n =
        writing ? pwrite(fd, p, bytes, offset) : pread(fd, p, bytes, offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    p += n;
    bytes -= (size_t)n;
    offset += n;
  }
  return 0;
}
static int fail(const char *message) {
  fprintf(stderr, "piano-boot-request: %s\n", message);
  return 1;
}
static int parse_generation(const char *text, unsigned char generation[16]) {
  if (strlen(text) != 32)
    return -1;
  for (unsigned i = 0; i < 16; i++) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; j++) {
      unsigned char c = (unsigned char)text[2 * i + j];
      unsigned digit;
      if (c >= '0' && c <= '9')
        digit = c - '0';
      else if (c >= 'a' && c <= 'f')
        digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        digit = c - 'A' + 10;
      else
        return -1;
      value = (value << 4) | digit;
    }
    generation[i] = (unsigned char)value;
  }
  return 0;
}
static int locate(int fd, uint64_t *offset, unsigned char generation[16]) {
  unsigned char header[PAGE], image[64], data[65536];
  if (transfer(fd, header, sizeof(header), 0, 0) ||
      memcmp(header, "ANDROID!", 8))
    return -1;
  uint32_t version = le32(header + 40), bytes = le32(header + 8);
  if ((version != 3 && version != 4) || bytes < 8192 ||
      bytes > BOOT_MAX - PAGE || transfer(fd, image, sizeof(image), PAGE, 0) ||
      memcmp(image + 56, "ARMd", 4))
    return -1;
  uint32_t branch = le32(image + 4);
  if (branch >> 26 != 5)
    return -1;
  int64_t displacement = (int64_t)(branch & 0x3ffffffu);
  if (displacement & 0x2000000u)
    displacement -= 0x4000000u;
  int64_t selected = 4 + displacement * 4;
  if (selected <= 8192 || selected >= (int64_t)bytes)
    return -1;
  uint64_t selector = (uint64_t)selected;
  size_t size =
      bytes - selector < sizeof(data) ? bytes - selector : sizeof(data);
  if (transfer(fd, data, size, (off_t)(PAGE + selector), 0))
    return -1;
  unsigned found = 0;
  for (size_t i = 0; i + 128 <= size; i++) {
    const unsigned char *p = data + i;
    if (memcmp(p, split_magic, 16))
      continue;
    uint64_t span = le64(p + 32), app = le64(p + 72), app_bytes = le64(p + 80),
             total = le64(p + 88);
    if (span > BOOT_MAX || le32(p + 16) != 1 || le32(p + 20) != 128 ||
        le64(p + 24) != selector)
      continue;
    uint64_t page = (span + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    if (page + 2 * PAGE != selector || app > total || app_bytes > total - app ||
        total > bytes)
      continue;
    *offset = PAGE + page;
    memcpy(generation, p + 96, 16);
    found++;
  }
  return found == 1 ? 0 : -1;
}
static void read_record(struct record *r, const unsigned char generation[16]) {
  unsigned char *p = r->page;
  unsigned nonzero = 0, reserved = 0;
  for (unsigned i = 0; i < 16; i++)
    nonzero |= generation[i];
  for (unsigned i = 56; i < 64; i++)
    reserved |= p[i];
  r->sequence = le64(p + 24);
  r->target = le32(p + 32);
  r->valid = nonzero && !reserved && !memcmp(p, request_magic, 16) &&
             !memcmp(p + 40, generation, 16) && le32(p + 16) == 1 &&
             le32(p + 20) == 64 && r->target <= 3 &&
             le32(p + 36) == PianoBootRequestCrc32(p) &&
             (r->sequence || !r->target);
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("piano-boot-request status|set|consume --device PATH [--target "
         "android|menu|linux|setup] [--expect-generation HEX32]\n"
         "Generation matching is available for set/consume. "
         "Updates only the two SunUEFI-owned "
         "request pages. Does not repack BOOT or reboot.");
    return 0;
  }
  if (argc < 4)
    return fail("use --help for command syntax");
  const char *action = argv[1], *path = NULL, *target_name = "android",
             *expected_text = NULL;
  unsigned char expected_generation[16];
  uint32_t target = 0;
  int writing = !strcmp(action, "set") || !strcmp(action, "consume");
  if (!writing && strcmp(action, "status"))
    return fail("unknown action");
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--device") && i + 1 < argc && !path)
      path = argv[++i];
    else if (!strcmp(argv[i], "--target") && i + 1 < argc)
      target_name = argv[++i];
    else if (!strcmp(argv[i], "--expect-generation") && i + 1 < argc &&
             !expected_text)
      expected_text = argv[++i];
    else
      return fail("unknown or duplicate option");
  }
  if (!path)
    return fail("--device is required");
  if (expected_text &&
      (!writing || parse_generation(expected_text, expected_generation)))
    return fail("--expect-generation requires set/consume and 32 hex digits");
  const char *names[] = {"android", "menu", "linux", "setup"};
  for (target = 0; target < 4 && strcmp(target_name, names[target]); target++)
    ;
  if (target == 4 || (!strcmp(action, "consume") && target))
    return fail("invalid target");
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return fail(strerror(errno));
  uint64_t offset;
  unsigned char generation[16];
  struct record records[2];
  struct stat info;
  if (fstat(fd, &info) || (!S_ISREG(info.st_mode) && !S_ISBLK(info.st_mode)) ||
      locate(fd, &offset, generation)) {
    close(fd);
    return fail("unsupported BOOT or missing SunUEFI layout");
  }
  if (expected_text && memcmp(expected_generation, generation, 16)) {
    close(fd);
    return fail("APP generation differs from installed configuration");
  }
  for (unsigned i = 0; i < 2; i++) {
    if (transfer(fd, records[i].page, PAGE, (off_t)(offset + i * PAGE), 0)) {
      close(fd);
      return fail("cannot read request page");
    }
    read_record(&records[i], generation);
  }
  if (!records[0].valid && !records[1].valid) {
    close(fd);
    return fail("both request records are invalid");
  }
  if (records[0].valid && records[1].valid &&
      records[0].sequence == records[1].sequence &&
      records[0].target != records[1].target) {
    close(fd);
    return fail("conflicting request records");
  }
  unsigned current = !records[0].valid                           ? 1
                     : !records[1].valid                         ? 0
                     : records[1].sequence > records[0].sequence ? 1
                                                                 : 0;
  uint64_t sequence = records[current].sequence;
  uint32_t chosen = records[current].target;
  int result = 0;
  if (writing && (!strcmp(action, "set") || chosen)) {
    if (sequence == UINT64_MAX) {
      close(fd);
      return fail("request sequence exhausted");
    }
    unsigned page = !records[0].valid                            ? 0
                    : !records[1].valid                          ? 1
                    : records[0].sequence <= records[1].sequence ? 0
                                                                 : 1;
    unsigned char expected[PAGE], verify[PAGE];
    memcpy(expected, records[page].page, PAGE);
    memset(expected, 0, 64);
    memcpy(expected, request_magic, 16);
    put32(expected + 16, 1);
    put32(expected + 20, 64);
    put64(expected + 24, sequence + 1);
    put32(expected + 32, target);
    memcpy(expected + 40, generation, 16);
    put32(expected + 36, PianoBootRequestCrc32(expected));
    int ro = 0, restore = 0;
    if (S_ISBLK(info.st_mode)) {
      if (ioctl(fd, BLKROGET, &ro)) {
        close(fd);
        return fail("cannot read block read-only state");
      }
      if (ro) {
        int rw = 0;
        if (ioctl(fd, BLKROSET, &rw)) {
          close(fd);
          return fail("cannot enable owned-page write");
        }
        restore = 1;
      }
    }
    int write_fd = open(path, O_RDWR | O_CLOEXEC);
    struct stat opened;
    if (write_fd < 0)
      result = fail(strerror(errno));
    else if (fstat(write_fd, &opened) || info.st_dev != opened.st_dev ||
             info.st_ino != opened.st_ino || info.st_rdev != opened.st_rdev)
      result = fail("BOOT device changed during open");
    else {
      unsigned char latest[PAGE];
      uint64_t latest_offset;
      unsigned char latest_generation[16];
      if (locate(write_fd, &latest_offset, latest_generation) ||
          offset != latest_offset || memcmp(generation, latest_generation, 16) ||
          (expected_text && memcmp(expected_generation, latest_generation, 16)))
        result = fail("BOOT layout changed before write");
      for (unsigned i = 0; i < 2 && !result; i++)
        if (transfer(write_fd, latest, PAGE, (off_t)(offset + i * PAGE), 0) ||
            memcmp(latest, records[i].page, PAGE))
          result = fail("request pages changed before write");
      if (!result &&
          (transfer(write_fd, expected, PAGE, (off_t)(offset + page * PAGE),
                    1) ||
           fsync(write_fd) ||
           transfer(write_fd, verify, PAGE, (off_t)(offset + page * PAGE), 0) ||
           memcmp(expected, verify, PAGE)))
        result = fail("owned request page write/readback failed");
    }
    if (write_fd >= 0)
      close(write_fd);
    if (restore && ioctl(fd, BLKROSET, &ro))
      result = fail("cannot restore block read-only state");
    if (!result) {
      sequence++;
      chosen = target;
    }
  }
  close(fd);
  if (!result) {
    char generation_hex[33];
    for (unsigned i = 0; i < 16; i++)
      snprintf(generation_hex + 2 * i, 3, "%02x", generation[i]);
    printf("{\"target\":%u,\"sequence\":%" PRIu64
           ",\"request_pages_offset\":%" PRIu64
           ",\"operation\":\"%s\",\"app_generation\":\"%s\","
           "\"original_kernel_changed\":false}\n",
           chosen, sequence, offset, action, generation_hex);
  }
  return result;
}
