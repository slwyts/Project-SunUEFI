// SPDX-License-Identifier: BSD-2-Clause-Patent
// SunUEFI storage transactions. Only named project entries and the end of
// userdata may change; plans contain current GPT metadata, never an old layout.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <sha2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define MAX_TABLE 32768u
#define MAX_SECTOR 4096u
#define MIB (1024ull * 1024ull)
#define GIB (1024ull * MIB)
#define SOURCE_STATE "/data/adb/piano-sunuefi/storage"
static char source_root[PATH_MAX] = "/sdcard";
static char selection_dir[PATH_MAX] = SOURCE_STATE;
enum { FLASH = 1, CREATE, RESIZE, DELETE, DELETE_RETURN };
struct gpt {
  uint64_t bytes;
  uint32_t sector, table_bytes;
  unsigned char head[MAX_SECTOR], tail[MAX_SECTOR], entries[MAX_TABLE];
};
struct image {
  uint64_t bytes;
  unsigned char sha[32];
  char path[PATH_MAX];
  uint64_t device, inode;
  int64_t mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
};
struct selection {
  char magic[16];
  struct image image;
};
struct plan {
  unsigned char magic[16];
  uint32_t operation, root_gib, esp_index, root_index, userdata_index;
  uint64_t userdata_bytes, root_bytes;
  struct gpt before, after;
  struct image esp_image, root_image;
  unsigned char digest[32];
};
static const unsigned char magic[16] = "SUNUEFI-STORv2";
static const unsigned char esp_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8,
                                           0xd2, 0x11, 0xba, 0x4b, 0,    0xa0,
                                           0xc9, 0x3e, 0xc9, 0x3b};
static const unsigned char root_type[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84,
                                            0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69,
                                            0xd8, 0x47, 0x7d, 0xe4};
static char disk_path[4096];
static int offline;
static uint32_t offline_sector;
static void need(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "piano-storage: %s\n", message);
    exit(1);
  }
}
static uint32_t u32(const unsigned char *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p) {
  return u32(p) | (uint64_t)u32(p + 4) << 32;
}
static void p32(unsigned char *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (unsigned char)(v >> (i * 8));
}
static void p64(unsigned char *p, uint64_t v) {
  p32(p, (uint32_t)v);
  p32(p + 4, (uint32_t)(v >> 32));
}
static int zero(const unsigned char *p, size_t n) {
  while (n--)
    if (*p++)
      return 0;
  return 1;
}
static uint32_t crc(const void *data, size_t n) {
  const unsigned char *p = data;
  uint32_t c = ~0u;
  while (n--) {
    c ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      c = (c >> 1) ^ (0xedb88320u & -(c & 1));
  }
  return ~c;
}
static int transfer(int fd, void *data, size_t n, uint64_t offset,
                    int writing) {
  unsigned char *p = data;
  while (n) {
    ssize_t r = writing ? pwrite(fd, p, n, (off_t)offset)
                        : pread(fd, p, n, (off_t)offset);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      return -1;
    p += r;
    n -= (size_t)r;
    offset += (uint64_t)r;
  }
  return 0;
}
static void io(int fd, void *data, size_t n, uint64_t offset, int writing) {
  need(!transfer(fd, data, n, offset, writing),
       writing ? "写入存储失败。" : "读取存储失败。");
}
static void hash(int fd, uint64_t n, uint64_t offset,
                 unsigned char output[32]) {
  unsigned char buf[1024 * 1024];
  SHA2_CTX ctx;
  SHA256Init(&ctx);
  while (n) {
    size_t take = n > sizeof(buf) ? sizeof(buf) : (size_t)n;
    io(fd, buf, take, offset, 0);
    SHA256Update(&ctx, buf, take);
    n -= take;
    offset += take;
  }
  SHA256Final(output, &ctx);
}
static int disk_open(int writing) {
  struct stat st;
  char path[4096];
  if (!offline) {
    need(realpath("/dev/block/by-name/userdata", path) != NULL,
         "找不到 Android 数据分区。");
    size_t n = strlen(path);
    while (n && path[n - 1] >= '0' && path[n - 1] <= '9')
      n--;
    need(n == 14 && !strncmp(path, "/dev/block/sd", 13) && path[13] >= 'a' &&
             path[13] <= 'z',
         "userdata 不在物理 UFS 分区上。");
    path[n] = 0;
    snprintf(disk_path, sizeof(disk_path), "%s", path);
  }
  int fd =
      open(disk_path, (writing ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
  need(fd >= 0 && !fstat(fd, &st), "无法打开 UFS 磁盘。");
  need(offline ? S_ISREG(st.st_mode) : S_ISBLK(st.st_mode), "磁盘类型不匹配。");
  return fd;
}
static void header(unsigned char *p, const struct gpt *g, uint64_t location) {
  need(!memcmp(p, "EFI PART", 8) && u32(p + 8) == 0x10000 &&
           u32(p + 12) >= 92 && u32(p + 12) <= g->sector,
       "GPT 头无效。");
  uint32_t expected = u32(p + 16);
  p32(p + 16, 0);
  uint32_t actual = crc(p, u32(p + 12));
  p32(p + 16, expected);
  need(actual == expected && u64(p + 24) == location &&
           u64(p + 32) < g->bytes / g->sector,
       "GPT 校验失败。");
}
static int named(const unsigned char *e, const char *name) {
  if (zero(e, 16))
    return 0;
  for (size_t i = 0; i < 36; i++) {
    unsigned c = (unsigned)e[56 + i * 2] | (unsigned)e[57 + i * 2] << 8;
    if (!*name)
      return c == 0;
    if (c != (unsigned char)*name++)
      return 0;
  }
  return !*name;
}
static unsigned count(const struct gpt *g) { return u32(g->head + 80); }
static unsigned char *entry(struct gpt *g, unsigned i) {
  return g->entries + (size_t)i * 128;
}
static const unsigned char *centry(const struct gpt *g, unsigned i) {
  return g->entries + (size_t)i * 128;
}
static unsigned find(const struct gpt *g, const char *name) {
  unsigned result = UINT32_MAX;
  for (unsigned i = 0; i < count(g); i++)
    if (named(centry(g, i), name)) {
      need(result == UINT32_MAX, "发现重名项目分区。");
      result = i;
    }
  return result;
}
static void read_gpt(int fd, struct gpt *g) {
  memset(g, 0, sizeof(*g));
  struct stat st;
  need(!fstat(fd, &st), "读取磁盘属性失败。");
  if (offline) {
    g->bytes = (uint64_t)st.st_size;
    g->sector = offline_sector;
  } else
    need(!ioctl(fd, BLKGETSIZE64, &g->bytes) &&
             !ioctl(fd, BLKSSZGET, &g->sector),
         "读取磁盘容量失败。");
  need((g->sector == 512 || g->sector == 4096) && g->bytes % g->sector == 0 &&
           g->bytes >= MIB,
       "磁盘扇区或容量无效。");
  io(fd, g->head, g->sector, g->sector, 0);
  header(g->head, g, 1);
  uint64_t last = g->bytes / g->sector - 1;
  io(fd, g->tail, g->sector, last * g->sector, 0);
  header(g->tail, g, last);
  need(u64(g->head + 32) == last && u64(g->tail + 32) == 1 &&
           u32(g->head + 84) == 128 && count(g) > 0 &&
           count(g) <= MAX_TABLE / 128,
       "GPT 条目尺寸无效。");
  need(!memcmp(g->head + 40, g->tail + 40, 32) &&
           !memcmp(g->head + 80, g->tail + 80, 12),
       "两份 GPT 描述不一致。");
  g->table_bytes = count(g) * 128;
  uint64_t sectors = (g->table_bytes + g->sector - 1) / g->sector;
  need(u64(g->head + 72) >= 2 &&
           u64(g->head + 72) + sectors <= u64(g->head + 40) &&
           u64(g->tail + 72) > u64(g->head + 48) &&
           u64(g->tail + 72) + sectors <= last,
       "GPT 表越界。");
  io(fd, g->entries, g->table_bytes, u64(g->head + 72) * g->sector, 0);
  unsigned char backup[MAX_TABLE];
  io(fd, backup, g->table_bytes, u64(g->tail + 72) * g->sector, 0);
  need(crc(g->entries, g->table_bytes) == u32(g->head + 88) &&
           !memcmp(g->entries, backup, g->table_bytes),
       "GPT 分区表校验失败。");
  for (unsigned i = 0; i < count(g); i++) {
    const unsigned char *e = centry(g, i);
    if (zero(e, 16))
      continue;
    uint64_t a = u64(e + 32), b = u64(e + 40);
    need(a >= u64(g->head + 40) && b <= u64(g->head + 48) && a <= b,
         "分区范围超出磁盘。");
    for (unsigned j = 0; j < i; j++) {
      const unsigned char *f = centry(g, j);
      if (zero(f, 16))
        continue;
      need(b < u64(f + 32) || a > u64(f + 40), "发现重叠分区。");
    }
  }
}
static uint64_t part_bytes(const struct gpt *g, unsigned i) {
  const unsigned char *e = centry(g, i);
  return (u64(e + 40) - u64(e + 32) + 1) * g->sector;
}
static int image_open(const char *path, struct image *out, const char *kind,
                      uint64_t capacity) {
  char resolved[PATH_MAX];
  memset(out, 0, sizeof(*out));
  need(realpath(path, resolved) != NULL, "所选镜像不存在，请重新选择文件。");
  int fd = open(resolved, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat st;
  need(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 8192,
       "镜像不是有效的普通文件。");
  out->bytes = (uint64_t)st.st_size;
  snprintf(out->path, sizeof(out->path), "%s", resolved);
  out->device = (uint64_t)st.st_dev;
  out->inode = (uint64_t)st.st_ino;
  out->mtime_sec = st.st_mtim.tv_sec;
  out->mtime_nsec = st.st_mtim.tv_nsec;
  out->ctime_sec = st.st_ctim.tv_sec;
  out->ctime_nsec = st.st_ctim.tv_nsec;
  need(out->bytes <= capacity, "镜像大于目标分区。");
  unsigned char b[4096];
  io(fd, b, sizeof(b), 0, 0);
  need(u32(b) != 0xed26ff3a,
       "请使用原始镜像，不能直接刷写 Android sparse 文件。");
  if (!strcmp(kind, "esp")) {
    unsigned sector = (unsigned)b[11] | (unsigned)b[12] << 8;
    uint64_t sectors = u32(b + 32);
    need((sector == 512 || sector == 4096) && b[510] == 0x55 &&
             b[511] == 0xaa && !memcmp(b + 82, "FAT32   ", 8) &&
             !memcmp(b + 71, "SUNUEFI_ESP", 11) && sectors > 0 &&
             sectors * sector <= out->bytes,
         "ESP 镜像必须是 SUNUEFI_ESP 标签的 FAT32。");
  } else {
    const unsigned char *s = b + 1024;
    unsigned log = u32(s + 24);
    uint64_t blocks = u32(s + 4);
    if (u32(s + 96) & 0x80)
      blocks |= (uint64_t)u32(s + 336) << 32;
    need(s[56] == 0x53 && s[57] == 0xef && log <= 6 && blocks > 0 &&
             !memcmp(s + 120, "PIANOROOT\0", 10) &&
             blocks <= out->bytes / (1024ull << log),
         "root 镜像必须是 PIANOROOT 标签的 ext4。");
  }
  hash(fd, out->bytes, 0, out->sha);
  struct stat after;
  need(!fstat(fd, &after) && after.st_size == st.st_size &&
           after.st_mtim.tv_sec == st.st_mtim.tv_sec &&
           after.st_mtim.tv_nsec == st.st_mtim.tv_nsec &&
           after.st_ctim.tv_sec == st.st_ctim.tv_sec &&
           after.st_ctim.tv_nsec == st.st_ctim.tv_nsec,
       "校验期间镜像发生变化，请重新选择。");
  return fd;
}
static void random_guid(unsigned char *p) {
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  need(fd >= 0, "随机标识生成失败。");
  io(fd, p, 16, 0, 0);
  close(fd);
  p[7] = (p[7] & 15) | 64;
  p[8] = (p[8] & 63) | 128;
}
static void new_entry(struct gpt *g, unsigned i, const unsigned char *type,
                      const char *name, uint64_t start, uint64_t end) {
  unsigned char *e = entry(g, i);
  memset(e, 0, 128);
  memcpy(e, type, 16);
  random_guid(e + 16);
  p64(e + 32, start);
  p64(e + 40, end);
  for (unsigned j = 0; name[j]; j++)
    e[56 + j * 2] = (unsigned char)name[j];
}
static void finish_gpt(struct gpt *g) {
  unsigned char *heads[] = {g->head, g->tail};
  for (unsigned i = 0; i < 2; i++) {
    p32(heads[i] + 88, crc(g->entries, g->table_bytes));
    p32(heads[i] + 16, 0);
    p32(heads[i] + 16, crc(heads[i], u32(heads[i] + 12)));
  }
}
static uint64_t align_up(uint64_t n, uint64_t alignment) {
  return (n + alignment - 1) / alignment * alignment;
}
static int selected_image(const char *kind, struct image *out,
                          uint64_t capacity);
static void make_plan(struct plan *p, unsigned op, unsigned gib,
                      uint64_t userdata_section_bytes) {
  memset(p, 0, sizeof(*p));
  memcpy(p->magic, magic, 16);
  p->operation = op;
  p->root_gib = gib;
  int fd = disk_open(0);
  read_gpt(fd, &p->before);
  close(fd);
  p->after = p->before;
  struct gpt *g = &p->after;
  p->esp_index = find(g, "sunuefi_esp");
  p->root_index = find(g, "sunuefi_root");
  p->userdata_index = find(g, "userdata");
  need(p->userdata_index != UINT32_MAX, "找不到 Android userdata GPT 条目。");
  int pair = p->esp_index != UINT32_MAX && p->root_index != UINT32_MAX;
  need(pair || (p->esp_index == UINT32_MAX && p->root_index == UINT32_MAX),
       "SunUEFI 分区不完整，不能自动修改。");
  if (pair)
    need(!memcmp(centry(g, p->esp_index), esp_type, 16) &&
             !memcmp(centry(g, p->root_index), root_type, 16),
         "SunUEFI 分区类型与 ESP/root 不匹配。");
  if (op == FLASH) {
    need(pair, "需要先建立 SunUEFI 分区。");
    fd = selected_image("esp", &p->esp_image, part_bytes(g, p->esp_index));
    close(fd);
    fd = selected_image("root", &p->root_image, part_bytes(g, p->root_index));
    close(fd);
  } else if (op == DELETE || op == DELETE_RETURN) {
    need(pair, "没有可删除的 SunUEFI 分区。");
    if (op == DELETE_RETURN) {
      unsigned char *u = entry(g, p->userdata_index);
      const unsigned char *a = centry(g, p->esp_index);
      const unsigned char *b = centry(g, p->root_index);
      if (u64(a + 32) > u64(b + 32)) {
        const unsigned char *swap = a;
        a = b;
        b = swap;
      }
      uint64_t userdata_start = u64(u + 32), tail = u64(b + 40);
      need(u64(u + 40) + 1 == u64(a + 32) && u64(a + 40) + 1 == u64(b + 32),
           "ESP、root 与 Android 数据分区不连续，不能直接归还空间。");
      for (unsigned i = 0; i < count(g); i++) {
        const unsigned char *other = centry(g, i);
        if (i == p->userdata_index || i == p->esp_index || i == p->root_index ||
            zero(other, 16))
          continue;
        need(u64(other + 40) < userdata_start || u64(other + 32) > tail,
             "归还范围内存在其他分区，不能覆盖。");
      }
      // Keep userdata's beginning, GUID and encryption metadata unchanged.
      // The original Android pre-mount path expands F2FS after normal reboot;
      // GPT success by itself must not be reported as filesystem completion.
      p64(u + 40, tail);
      p->userdata_bytes = (tail - userdata_start + 1) * g->sector;
    }
    memset(entry(g, p->esp_index), 0, 128);
    memset(entry(g, p->root_index), 0, 128);
  } else if (op == RESIZE) {
    need(pair, "没有可调整的 root 分区。");
    p->root_bytes = (uint64_t)gib * GIB;
    unsigned char *e = entry(g, p->root_index);
    uint64_t start = u64(e + 32), end = start + p->root_bytes / g->sector - 1;
    need(end <= u64(g->head + 48), "目标容量超出磁盘末尾；root 起点保持不变。");
    for (unsigned i = 0; i < count(g); i++) {
      const unsigned char *f = centry(g, i);
      if (i == p->root_index || zero(f, 16))
        continue;
      need(end < u64(f + 32) || start > u64(f + 40),
           "root 后没有足够相邻空闲空间；不会移动其他分区。");
    }
    need(part_bytes(g, p->root_index) != p->root_bytes,
         "root 已是此容量，无需调整。");
    p64(e + 40, end);
  } else if (op == CREATE) {
    need(!pair, "SunUEFI 分区已存在，无需重复建立。");
    uint64_t needed = (512 * MIB + (uint64_t)gib * GIB) / g->sector,
             alignment = MIB / g->sector;
    uint64_t candidate = align_up(u64(g->head + 40), alignment), start = 0,
             last = u64(g->head + 48);
    // Search existing free intervals before considering an online userdata
    // shrink.
    while (candidate <= last) {
      uint64_t next = last + 1, occupied_end = 0;
      for (unsigned i = 0; i < count(g); i++) {
        const unsigned char *e = centry(g, i);
        if (zero(e, 16))
          continue;
        uint64_t a = u64(e + 32), b = u64(e + 40);
        if (a <= candidate && b >= candidate)
          occupied_end = b + 1;
        else if (a > candidate && a < next)
          next = a;
      }
      if (occupied_end) {
        candidate = align_up(occupied_end, alignment);
        continue;
      }
      if (next - candidate >= needed) {
        start = candidate;
        break;
      }
      candidate = align_up(next, alignment);
    }
    if (!start) {
      unsigned char *u = entry(g, p->userdata_index);
      uint64_t oldend = u64(u + 40), ustart = u64(u + 32);
      for (unsigned i = 0; i < count(g); i++) {
        const unsigned char *e = centry(g, i);
        if (i == p->userdata_index || zero(e, 16))
          continue;
        need(u64(e + 40) < ustart, "userdata 后存在其他分区，不能从尾部划分。");
      }
      need(last + 1 > needed, "磁盘容量不足。");
      need(userdata_section_bytes >= 2 * MIB &&
               userdata_section_bytes % 4096 == 0,
           "需要先读取当前 Android F2FS section 大小，才能缩小 userdata。");
      uint64_t section_sectors = userdata_section_bytes / g->sector;
      uint64_t latest_start = last + 1 - needed;
      need(latest_start > ustart, "Android 数据分区没有足够空间可划分。");
      // F2FS sections are relative to the beginning of userdata, not LBA 0.
      // Align its new length; the verified resizer rechecks the actual section
      // size immediately before issuing F2FS_IOC_RESIZE_FS.
      start =
          ustart + (latest_start - ustart) / section_sectors * section_sectors;
      need(start > ustart && start - 1 < oldend,
           "Android 数据分区没有足够空间可划分。");
      p64(u + 40, start - 1);
      p->userdata_bytes = (start - ustart) * g->sector;
    }
    for (unsigned i = 0; i < count(g); i++)
      if (zero(centry(g, i), 16)) {
        if (p->esp_index == UINT32_MAX)
          p->esp_index = i;
        else if (p->root_index == UINT32_MAX) {
          p->root_index = i;
          break;
        }
      }
    need(p->root_index != UINT32_MAX, "GPT 没有两个空闲条目。");
    uint64_t rootstart = start + 512 * MIB / g->sector;
    new_entry(g, p->esp_index, esp_type, "sunuefi_esp", start, rootstart - 1);
    new_entry(g, p->root_index, root_type, "sunuefi_root", rootstart,
              rootstart + (uint64_t)gib * GIB / g->sector - 1);
    p->root_bytes = (uint64_t)gib * GIB;
  }
  finish_gpt(g);
}
static void plan_hash(const struct plan *p, unsigned char out[32]) {
  SHA2_CTX ctx;
  SHA256Init(&ctx);
  SHA256Update(&ctx, (const unsigned char *)p, sizeof(*p) - 32);
  SHA256Final(out, &ctx);
}
static void sync_parent(const char *path) {
  char copy[4096];
  need(strlen(path) < sizeof(copy), "元数据路径过长。");
  snprintf(copy, sizeof(copy), "%s", path);
  char *slash = strrchr(copy, '/');
  if (slash) {
    *slash = 0;
    if (!copy[0])
      snprintf(copy, sizeof(copy), "/");
  } else
    snprintf(copy, sizeof(copy), ".");
  int fd = open(copy, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  need(fd >= 0 && !fsync(fd), "同步元数据目录失败。");
  close(fd);
}
// Browser File objects reveal a display name and length, not a trusted path.
// Resolve only a unique local file under shared storage. Keep its identity in
// a root-owned selection record; a flash plan later adds its content hash.
static void json_string(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') {
      putchar('\\');
      putchar(*p);
    } else if (*p < 32)
      printf("\\u%04x", *p);
    else
      putchar(*p);
  }
  putchar('"');
}
static int identity_matches(const struct image *i, const struct stat *s) {
  return S_ISREG(s->st_mode) && (uint64_t)s->st_size == i->bytes &&
         (uint64_t)s->st_dev == i->device && (uint64_t)s->st_ino == i->inode &&
         s->st_mtim.tv_sec == i->mtime_sec &&
         s->st_mtim.tv_nsec == i->mtime_nsec &&
         s->st_ctim.tv_sec == i->ctime_sec &&
         s->st_ctim.tv_nsec == i->ctime_nsec;
}
static void selected_path(const char *kind, char path[PATH_MAX]) {
  need(!strcmp(kind, "esp") || !strcmp(kind, "root"),
       "请选择 ESP 或 root 镜像。");
  need(snprintf(path, PATH_MAX, "%s/selected-%s.bin", selection_dir, kind) <
           PATH_MAX,
       "选择记录路径过长。");
}
static int read_selection(const char *kind, struct selection *selection) {
  char path[PATH_MAX];
  selected_path(kind, path);
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 && errno == ENOENT)
    return 0;
  struct stat st;
  need(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) &&
           st.st_uid == getuid() && (st.st_mode & 077) == 0 &&
           st.st_size == (off_t)sizeof(*selection),
       "镜像选择记录无效，请重新选择。");
  io(fd, selection, sizeof(*selection), 0, 0);
  close(fd);
  need(!memcmp(selection->magic, "SUNUEFI-SRCv1", 14) &&
           memchr(selection->image.path, 0, PATH_MAX) != NULL &&
           selection->image.path[0] == '/',
       "镜像选择记录无效，请重新选择。");
  return 1;
}
static void image_selection_json(const char *kind,
                                 const struct selection *selection,
                                 int selected) {
  struct stat st;
  int available = selected && !lstat(selection->image.path, &st) &&
                  identity_matches(&selection->image, &st);
  printf("{\"kind\":\"%s\",\"selected\":%s,\"available\":%s,\"name\":", kind,
         selected ? "true" : "false", available ? "true" : "false");
  const char *name = selected ? strrchr(selection->image.path, '/') : NULL;
  json_string(name ? name + 1 : "");
  printf(",\"bytes\":%" PRIu64 ",\"reason\":",
         selected ? selection->image.bytes : 0);
  json_string(!selected    ? "请选择设备上的镜像文件。"
              : !available ? "所选文件已移动或发生变化，请重新选择。"
                           : "");
  putchar('}');
}
static void sources_status(void) {
  struct selection selection;
  printf("{\"sources\":[");
  const char *kinds[] = {"esp", "root"};
  for (unsigned i = 0; i < 2; i++) {
    if (i)
      putchar(',');
    memset(&selection, 0, sizeof(selection));
    int selected = read_selection(kinds[i], &selection);
    image_selection_json(kinds[i], &selection, selected);
  }
  puts("],\"local_file_picker\":true}");
}
struct search {
  const char *name;
  uint64_t bytes;
  unsigned matched, visited;
  struct image image;
};
static void search_dir(int dirfd, const char *path, unsigned depth,
                       struct search *search) {
  need(depth <= 32, "文件目录过深，无法确认唯一来源。请选择浅层目录中的文件。");
  DIR *dir = fdopendir(dup(dirfd));
  need(dir != NULL, "无法读取本地文件目录。");
  struct dirent *e;
  while ((e = readdir(dir))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    need(++search->visited <= 500000, "本地文件过多，无法确认唯一来源。");
    struct stat st;
    if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW))
      continue;
    char child[PATH_MAX];
    need(snprintf(child, sizeof(child), "%s/%s", path, e->d_name) <
             (int)sizeof(child),
         "本地文件路径过长。");
    if (S_ISDIR(st.st_mode)) {
      int fd = openat(dirfd, e->d_name,
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (fd >= 0) {
        search_dir(fd, child, depth + 1, search);
        close(fd);
      }
    } else if (S_ISREG(st.st_mode) && !strcmp(e->d_name, search->name) &&
               (uint64_t)st.st_size == search->bytes) {
      int fd = openat(dirfd, e->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      struct stat opened;
      need(fd >= 0 && !fstat(fd, &opened) && S_ISREG(opened.st_mode) &&
               opened.st_dev == st.st_dev && opened.st_ino == st.st_ino &&
               opened.st_size == st.st_size,
           "所选文件在读取期间发生变化，请重新选择。");
      close(fd);
      need(++search->matched == 1, "有多个同名且同大小的文件，无法确定所选来源"
                                   "。请重命名其中一个文件再选择。");
      snprintf(search->image.path, sizeof(search->image.path), "%s", child);
      search->image.bytes = (uint64_t)st.st_size;
      search->image.device = (uint64_t)st.st_dev;
      search->image.inode = (uint64_t)st.st_ino;
      search->image.mtime_sec = st.st_mtim.tv_sec;
      search->image.mtime_nsec = st.st_mtim.tv_nsec;
      search->image.ctime_sec = st.st_ctim.tv_sec;
      search->image.ctime_nsec = st.st_ctim.tv_nsec;
    }
  }
  closedir(dir);
}
static int hex_digit(unsigned char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
static int utf8_name(const unsigned char *s) {
  while (*s) {
    uint32_t value = *s++;
    unsigned following, minimum;
    if (value < 0x80)
      continue;
    if (value >= 0xc2 && value <= 0xdf) {
      following = 1;
      minimum = 0x80;
      value &= 31;
    } else if (value >= 0xe0 && value <= 0xef) {
      following = 2;
      minimum = 0x800;
      value &= 15;
    } else if (value >= 0xf0 && value <= 0xf4) {
      following = 3;
      minimum = 0x10000;
      value &= 7;
    } else
      return 0;
    while (following--) {
      if ((*s & 0xc0) != 0x80)
        return 0;
      value = value << 6 | (*s++ & 63);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      return 0;
  }
  return 1;
}
static void select_image(const char *kind, const char *hex, const char *size) {
  char name[256], endpath[PATH_MAX], temporary[PATH_MAX], root[PATH_MAX];
  selected_path(kind, endpath);
  size_t n = strlen(hex);
  need(n > 0 && !(n & 1) && n / 2 < sizeof(name), "所选文件名无效。");
  for (size_t i = 0; i < n / 2; i++) {
    int hi = hex_digit((unsigned char)hex[i * 2]);
    int lo = hex_digit((unsigned char)hex[i * 2 + 1]);
    need(hi >= 0 && lo >= 0, "所选文件名编码无效。");
    name[i] = (char)((hi << 4) | lo);
    need((unsigned char)name[i] >= 32 && name[i] != '/' && name[i] != '\\',
         "所选文件名无效。");
  }
  name[n / 2] = 0;
  need(utf8_name((const unsigned char *)name), "所选文件名不是有效的 UTF-8。");
  char *end;
  errno = 0;
  uint64_t bytes = strtoull(size, &end, 10);
  need(*size >= '0' && *size <= '9' && !*end && !errno && bytes >= 8192,
       "镜像容量无效，请选择未压缩的镜像文件。");
  need(realpath(source_root, root) != NULL, "无法访问本地共享存储。");
  int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  need(fd >= 0, "无法访问本地共享存储。");
  struct search search = {.name = name, .bytes = bytes};
  search_dir(fd, root, 0, &search);
  close(fd);
  need(search.matched == 1, "无法找到所选本地文件。请在文件选择器中选择设备存储"
                            "中的镜像；暂不支持云盘文件。");
  struct stat st;
  need(!stat(selection_dir, &st) && S_ISDIR(st.st_mode) &&
           st.st_uid == getuid() && (st.st_mode & 077) == 0,
       "选择记录目录不可用。");
  struct selection selection = {0};
  memcpy(selection.magic, "SUNUEFI-SRCv1", 14);
  selection.image = search.image;
  need(snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", endpath,
                (long)getpid()) < (int)sizeof(temporary),
       "选择记录路径过长。");
  fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600);
  need(fd >= 0, "无法保存镜像选择。");
  io(fd, &selection, sizeof(selection), 0, 1);
  need(!fsync(fd), "同步镜像选择失败。");
  close(fd);
  need(!rename(temporary, endpath), "保存镜像选择失败。");
  sync_parent(endpath);
  printf("{\"operation\":\"storage-select\",\"source\":");
  image_selection_json(kind, &selection, 1);
  puts(",\"read_only\":true}");
}
static int selected_image(const char *kind, struct image *out,
                          uint64_t capacity) {
  struct selection selection;
  need(read_selection(kind, &selection), "请先选择 ESP 和 root 镜像文件。");
  struct stat st;
  need(!lstat(selection.image.path, &st) &&
           identity_matches(&selection.image, &st),
       "所选镜像已变化，请重新选择文件。");
  int fd = image_open(selection.image.path, out, kind, capacity);
  need(out->device == selection.image.device &&
           out->inode == selection.image.inode &&
           out->bytes == selection.image.bytes &&
           out->mtime_sec == selection.image.mtime_sec &&
           out->mtime_nsec == selection.image.mtime_nsec &&
           out->ctime_sec == selection.image.ctime_sec &&
           out->ctime_nsec == selection.image.ctime_nsec,
       "所选镜像已变化，请重新选择文件。");
  return fd;
}
static void save_plan(const char *path, struct plan *p) {
  plan_hash(p, p->digest);
  int fd =
      open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  need(fd >= 0, "计划已存在或无法保存。");
  io(fd, (void *)p, sizeof(*p), 0, 1);
  need(!fsync(fd), "保存计划失败。");
  close(fd);
  sync_parent(path);
}
static void load_plan(const char *path, struct plan *p) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat st;
  need(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) &&
           st.st_size == (off_t)sizeof(*p) && st.st_uid == getuid() &&
           (st.st_mode & 077) == 0,
       "存储计划无效。");
  io(fd, p, sizeof(*p), 0, 0);
  close(fd);
  need(!memcmp(p->magic, magic, 16) && p->operation >= FLASH &&
           p->operation <= DELETE_RETURN,
       "不支持此存储计划。");
  unsigned char digest[32];
  plan_hash(p, digest);
  need(!memcmp(digest, p->digest, 32), "计划文件损坏，请重新预览操作。");
}
static void describe(const struct plan *p) {
  const char *op = p->operation == FLASH           ? "flash"
                   : p->operation == CREATE        ? "create"
                   : p->operation == RESIZE        ? "resize"
                   : p->operation == DELETE_RETURN ? "delete-return"
                                                   : "delete";
  uint64_t old = p->root_index != UINT32_MAX && p->operation != CREATE
                     ? part_bytes(&p->before, p->root_index)
                     : 0;
  printf("{\"operation\":\"%s\",\"root_gib\":%u,\"requires_userdata_shrink\":%"
         "s,\"userdata_target_bytes\":%" PRIu64
         ",\"current_root_bytes\":%" PRIu64 ",\"target_root_bytes\":%" PRIu64
         ",\"esp_image_bytes\":%" PRIu64 ",\"root_image_bytes\":%" PRIu64
         ",\"destructive\":%s,\"read_only\":true",
         op, p->root_gib,
         p->operation == CREATE && p->userdata_bytes ? "true" : "false",
         p->userdata_bytes, old, p->root_bytes, p->esp_image.bytes,
         p->root_image.bytes,
         p->operation == DELETE || p->operation == DELETE_RETURN ||
                 p->operation == FLASH
             ? "true"
             : "false");
  if (p->operation == DELETE_RETURN)
    printf(
        ",\"returns_space_to_android\":true,\"userdata_current_bytes\":%" PRIu64
        ",\"returned_bytes\":%" PRIu64
        ",\"pending_android_expansion\":true,\"restart_required\":true",
        part_bytes(&p->before, p->userdata_index),
        p->userdata_bytes - part_bytes(&p->before, p->userdata_index));
  if (p->operation == FLASH) {
    struct selection selection = {0};
    printf(",\"sources\":[");
    selection.image = p->esp_image;
    image_selection_json("esp", &selection, 1);
    putchar(',');
    selection.image = p->root_image;
    image_selection_json("root", &selection, 1);
    putchar(']');
  }
  puts("}");
}
static int current(const struct plan *p, int writing) {
  int fd = disk_open(writing);
  struct gpt g;
  read_gpt(fd, &g);
  need(!memcmp(&g, &p->before, sizeof(g)),
       "分区布局已变化，请重新生成操作计划。");
  return fd;
}
static int write_gpt(int fd, struct gpt *g) {
  if (transfer(fd, g->entries, g->table_bytes, u64(g->tail + 72) * g->sector,
               1) ||
      transfer(fd, g->tail, g->sector, u64(g->tail + 24) * g->sector, 1) ||
      fsync(fd))
    return -1;
  if (transfer(fd, g->entries, g->table_bytes, u64(g->head + 72) * g->sector,
               1) ||
      transfer(fd, g->head, g->sector, g->sector, 1) || fsync(fd))
    return -1;
  return 0;
}
static int equal_gpt(int fd, const struct gpt *g) {
  unsigned char table[MAX_TABLE], h[MAX_SECTOR];
  return !transfer(fd, table, g->table_bytes, u64(g->head + 72) * g->sector,
                   0) &&
         !memcmp(table, g->entries, g->table_bytes) &&
         !transfer(fd, table, g->table_bytes, u64(g->tail + 72) * g->sector,
                   0) &&
         !memcmp(table, g->entries, g->table_bytes) &&
         !transfer(fd, h, g->sector, g->sector, 0) &&
         !memcmp(h, g->head, g->sector) &&
         !transfer(fd, h, g->sector, u64(g->tail + 24) * g->sector, 0) &&
         !memcmp(h, g->tail, g->sector);
}
static void apply(const struct plan *p, const char *backup) {
  need(p->operation != FLASH, "刷写计划不能改变 GPT。");
  int fd = current(p, 1);
  int b =
      open(backup, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  need(b >= 0, "无法保存本次 GPT 元数据。");
  io(b, (void *)&p->before, sizeof(p->before), 0, 1);
  need(!fsync(b), "无法同步 GPT 元数据备份。");
  close(b);
  sync_parent(backup);
  struct gpt target = p->after;
  if (write_gpt(fd, &target) || !equal_gpt(fd, &p->after)) {
    struct gpt restore = p->before;
    need(!write_gpt(fd, &restore) && equal_gpt(fd, &restore),
         "GPT 写入失败，恢复也未完成；请勿重启，保留本次 GPT 元数据用于修复。");
    need(0, "GPT 写入或读回失败，已恢复原分区表。");
  }
  int reread = offline ? 0 : ioctl(fd, BLKRRPART, 0);
  close(fd);
  printf("{\"gpt_written\":true,\"metadata_readback_equal\":true,\"reboot_"
         "required\":%s}\n",
         reread ? "true" : "false");
}
static int project_open(const struct plan *p, unsigned index, int writing) {
  need(!offline, "离线模式不打开真实分区。");
  const char *name = index == p->esp_index ? "sunuefi_esp" : "sunuefi_root";
  char alias[128], path[4096], sys[4096], text[64];
  snprintf(alias, sizeof(alias), "/dev/block/by-name/%s", name);
  need(realpath(alias, path) != NULL,
       "项目分区节点尚未出现，需要正常重启后继续。");
  char expected[4096];
  need(snprintf(expected, sizeof(expected), "%s%u", disk_path, index + 1) <
           (int)sizeof(expected),
       "分区节点太长。");
  need(!strcmp(path, expected), "项目分区节点不匹配当前 GPT。");
  int fd = open(path, (writing ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
  struct stat st;
  uint64_t bytes;
  need(fd >= 0 && !fstat(fd, &st) && S_ISBLK(st.st_mode) &&
           !ioctl(fd, BLKGETSIZE64, &bytes) &&
           bytes == part_bytes(&p->before, index),
       "内核分区容量与 GPT 不一致，需要重启。");
  snprintf(sys, sizeof(sys), "/sys/dev/block/%u:%u/start", major(st.st_rdev),
           minor(st.st_rdev));
  FILE *f = fopen(sys, "r");
  need(f && fgets(text, sizeof(text), f), "无法读取分区起点。");
  fclose(f);
  need(strtoull(text, NULL, 10) * 512 ==
           u64(centry(&p->before, index) + 32) * p->before.sector,
       "内核分区起点与 GPT 不一致。");
  snprintf(sys, sizeof(sys), "/sys/dev/block/%u:%u/holders", major(st.st_rdev),
           minor(st.st_rdev));
  DIR *holders = opendir(sys);
  need(holders != NULL, "无法读取分区使用状态。");
  struct dirent *held;
  while ((held = readdir(holders)))
    need(held->d_name[0] == '.', "目标分区仍被其他块设备使用。");
  closedir(holders);
  f = fopen("/proc/self/mountinfo", "r");
  need(f != NULL, "无法读取挂载状态。");
  char line[8192];
  while (fgets(line, sizeof(line), f)) {
    unsigned a, b, ma, mi;
    if (sscanf(line, "%u %u %u:%u", &a, &b, &ma, &mi) == 4)
      need(ma != major(st.st_rdev) || mi != minor(st.st_rdev),
           "目标分区仍被挂载，请先卸载。");
  }
  fclose(f);
  return fd;
}
static void flash(const struct plan *p) {
  need(p->operation == FLASH, "计划不是镜像刷写。");
  int disk = current(p, 0);
  close(disk);
  struct image info[2];
  const struct image *wanted[2] = {&p->esp_image, &p->root_image};
  const char *paths[2] = {p->esp_image.path, p->root_image.path};
  const char *kinds[2] = {"esp", "root"};
  unsigned indexes[2] = {p->esp_index, p->root_index};
  int src[2], dst[2];
  // Validate both sources and both destinations before either partition
  // changes.
  for (unsigned i = 0; i < 2; i++) {
    src[i] = image_open(paths[i], &info[i], kinds[i],
                        part_bytes(&p->before, indexes[i]));
    need(!memcmp(&info[i], wanted[i], sizeof(info[i])),
         "镜像内容已变化，请重新生成计划。");
    dst[i] = project_open(p, indexes[i], 1);
  }
  unsigned char *buf = malloc(4 * MIB);
  need(buf != NULL, "分配刷写缓冲区失败。");
  for (unsigned i = 0; i < 2; i++) {
    uint64_t done = 0;
    while (done < info[i].bytes) {
      size_t n = info[i].bytes - done > 4 * MIB
                     ? 4 * MIB
                     : (size_t)(info[i].bytes - done);
      io(src[i], buf, n, done, 0);
      io(dst[i], buf, n, done, 1);
      done += n;
      fprintf(stderr, "progress:%s:%" PRIu64 ":%" PRIu64 "\n", kinds[i], done,
              info[i].bytes);
    }
    need(!fsync(dst[i]), "镜像写入同步失败。");
    unsigned char got[32];
    hash(dst[i], info[i].bytes, 0, got);
    need(!memcmp(got, wanted[i]->sha, 32), "镜像读回校验失败，请重新刷写。");
    hash(src[i], info[i].bytes, 0, got);
    need(!memcmp(got, wanted[i]->sha, 32),
         "刷写过程中源镜像发生变化，请重新刷写。");
    close(src[i]);
    close(dst[i]);
  }
  free(buf);
  puts("{\"images_written\":true,\"readback_equal\":true,\"reboot_required\":"
       "false}");
}
static unsigned number(const char *text) {
  char *end;
  unsigned long v = strtoul(text, &end, 10);
  need(*text && !*end && v <= UINT32_MAX, "参数不是有效整数。");
  return (unsigned)v;
}
int main(int argc, char **argv) {
  uint64_t userdata_section_bytes = 0;
  if (argc >= 4 && !strcmp(argv[argc - 3], "--offline")) {
    offline = 1;
    need(realpath(argv[argc - 2], disk_path) != NULL, "找不到离线磁盘文件。");
    offline_sector = number(argv[argc - 1]);
    argc -= 3;
  }
  if (argc >= 6 && !strcmp(argv[argc - 4], "--source-root") &&
      !strcmp(argv[argc - 2], "--selection-dir")) {
    need(offline, "自定义来源目录仅用于离线验证。");
    need(snprintf(source_root, sizeof(source_root), "%s", argv[argc - 3]) <
                 PATH_MAX &&
             snprintf(selection_dir, sizeof(selection_dir), "%s",
                      argv[argc - 1]) < PATH_MAX,
         "离线来源路径过长。");
    argc -= 4;
  }
  if (argc >= 4 && !strcmp(argv[argc - 2], "--userdata-section-bytes")) {
    char *end;
    errno = 0;
    userdata_section_bytes = strtoull(argv[argc - 1], &end, 10);
    need(argv[argc - 1][0] >= '0' && argv[argc - 1][0] <= '9' && !*end &&
             !errno && userdata_section_bytes >= 2 * MIB &&
             userdata_section_bytes % 4096 == 0,
         "F2FS section 大小无效。");
    argc -= 2;
  }
  need(getuid() == 0 || offline, "需要 Root 权限。");
  need(argc >= 2, "缺少操作参数。");
  if (!strcmp(argv[1], "select")) {
    need(argc == 5, "Usage: select esp|root FILE_NAME_UTF8_HEX FILE_BYTES");
    select_image(argv[2], argv[3], argv[4]);
    return 0;
  }
  if (!strcmp(argv[1], "sources")) {
    need(argc == 2, "Usage: sources");
    sources_status();
    return 0;
  }
  struct plan *p = calloc(1, sizeof(*p));
  need(p != NULL, "分配计划缓冲区失败。");
  if (!strcmp(argv[1], "plan")) {
    need(argc == 5, "Usage: plan OP ROOT_GIB PLANFILE");
    unsigned op = !strcmp(argv[2], "flash")           ? FLASH
                  : !strcmp(argv[2], "create")        ? CREATE
                  : !strcmp(argv[2], "resize")        ? RESIZE
                  : !strcmp(argv[2], "delete")        ? DELETE
                  : !strcmp(argv[2], "delete-return") ? DELETE_RETURN
                                                      : 0;
    need(op != 0, "不支持此存储操作。");
    unsigned gib = number(argv[3]);
    need((op == CREATE || op == RESIZE) ? gib == 32 || gib == 64 || gib == 128
                                        : gib == 0,
         "容量必须是 32、64 或 128 GiB。");
    make_plan(p, op, gib, userdata_section_bytes);
    save_plan(argv[4], p);
    describe(p);
  } else {
    need(argc >= 3, "缺少计划文件。");
    load_plan(argv[2], p);
    if (!strcmp(argv[1], "describe")) {
      need(argc == 3, "无效 describe 参数。");
      describe(p);
    } else if (!strcmp(argv[1], "validate")) {
      need(argc == 3, "无效 validate 参数。");
      int fd = current(p, 0);
      close(fd);
      puts("{\"plan_current\":true}");
    } else if (!strcmp(argv[1], "facts")) {
      need(argc == 3, "无效 facts 参数。");
      printf("%u %u %u %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %u\n",
             p->operation, p->esp_index + 1, p->root_index + 1,
             p->userdata_index + 1, p->userdata_bytes, p->root_bytes,
             p->root_index != UINT32_MAX && p->operation != CREATE
                 ? part_bytes(&p->before, p->root_index)
                 : 0,
             p->before.sector);
    } else if (!strcmp(argv[1], "apply")) {
      need(argc == 5 && !strcmp(argv[4], "--execute"),
           "必须显式执行 GPT 操作。");
      apply(p, argv[3]);
    } else if (!strcmp(argv[1], "flash")) {
      need(argc == 4 && !strcmp(argv[3], "--execute"),
           "必须显式执行镜像刷写。");
      flash(p);
    } else if (!strcmp(argv[1], "check-project")) {
      need(argc == 3, "无效 check-project 参数。");
      int fd = current(p, 0);
      close(fd);
      fd = project_open(p, p->esp_index, 0);
      close(fd);
      fd = project_open(p, p->root_index, 0);
      close(fd);
      puts("{\"unmounted\":true}");
    } else if (!strcmp(argv[1], "check-root")) {
      need(argc == 3, "无效 check-root 参数。");
      int fd = current(p, 0);
      close(fd);
      fd = project_open(p, p->root_index, 0);
      unsigned char super[1024];
      io(fd, super, sizeof(super), 1024, 0);
      close(fd);
      need(super[56] == 0x53 && super[57] == 0xef &&
               !memcmp(super + 120, "PIANOROOT\0", 10),
           "root 不是 PIANOROOT ext4 文件系统。");
      puts("{\"filesystem\":\"ext4\",\"label\":\"PIANOROOT\",\"unmounted\":"
           "true}");
    } else
      need(0, "不支持此存储命令。");
  }
  free(p);
  return 0;
}
