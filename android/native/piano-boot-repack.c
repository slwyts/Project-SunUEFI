// SPDX-License-Identifier: BSD-2-Clause-Patent
/* SPLITv1 file repacking and explicit current-slot Android BOOT operations.
 * BOOT4 and AVB fields follow the AOSP bootimg/libavb wire structures.
 * Recovery data follows APP; the original GKI is kept once, in place.
 */
#define _GNU_SOURCE
#include "BootRequest.h"
#define JSMN_STRICT
#include "jsmn.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <sha2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096u
#define BOOT_BYTES (96u * 1024u * 1024u)
#define CATALOG_HEADER 320u
#define CATALOG_LIMIT (2u * 1024u * 1024u)
static const unsigned char split_magic[16] = "SUNUEFI-SPLITv1";
static const unsigned char catalog_magic[16] = "SUNUEFI-RSTRv1";
static const unsigned char request_magic[16] = "SUNUEFI-NEXTv1";
static const unsigned char app_magic[16] = "SUNUEFI-APPv1";
struct blob { unsigned char *data; size_t size; };
struct avb {
  const unsigned char *descriptors;
  uint64_t descriptor_bytes, original_bytes, vbmeta_offset, vbmeta_bytes, rollback;
  int hash_matches;
  char fingerprint[1024];
};
struct boot { uint32_t kernel_bytes; uint64_t span; struct avb avb; };

static void fail(const char *message) {
  fprintf(stderr, "piano-boot-repack: %s\n", message);
  exit(1);
}
static void require(int ok, const char *message) { if (!ok) fail(message); }
static uint32_t le32(const unsigned char *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t be32(const unsigned char *p) {
  return p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static uint64_t be64(const unsigned char *p) { return be32(p + 4) | (uint64_t)be32(p) << 32; }
static void put32(unsigned char *p, uint32_t n) {
  for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (8 * i));
}
static void put64(unsigned char *p, uint64_t n) {
  for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(n >> (8 * i));
}
static void putbe32(unsigned char *p, uint32_t n) {
  for (unsigned i = 0; i < 4; i++) p[3 - i] = (unsigned char)(n >> (8 * i));
}
static void putbe64(unsigned char *p, uint64_t n) {
  for (unsigned i = 0; i < 8; i++) p[7 - i] = (unsigned char)(n >> (8 * i));
}
static uint64_t align_to(uint64_t n, uint64_t unit) { return (n + unit - 1) & ~(unit - 1); }
static int range(uint64_t off, uint64_t n, uint64_t end) { return off <= end && n <= end - off; }
static int nonzero(const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i]) return 1;
  return 0;
}
static void sha(const void *p, size_t n, unsigned char hash[32]) {
  SHA2_CTX ctx;
  SHA256Init(&ctx); SHA256Update(&ctx, p, n); SHA256Final(hash, &ctx);
}
static void hex(const unsigned char hash[32], char text[65]) {
  static const char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; i++) {
    text[2 * i] = digits[hash[i] >> 4]; text[2 * i + 1] = digits[hash[i] & 15];
  }
  text[64] = 0;
}
static void hash_hex(const void *p, size_t n, char text[65]) {
  unsigned char hash[32]; sha(p, n, hash); hex(hash, text);
}
static struct blob load(const char *path, size_t limit) {
  require(path != NULL, "missing input path");
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) fail(strerror(errno));
  struct stat st;
  require(!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
          (uint64_t)st.st_size <= limit, "input must be a bounded regular file; device mode is unavailable");
  struct blob b = { .size = (size_t)st.st_size };
  b.data = malloc(b.size); require(b.data != NULL, "out of memory");
  size_t done = 0;
  while (done < b.size) {
    ssize_t n = read(fd, b.data + done, b.size - done);
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "could not read the complete input"); done += (size_t)n;
  }
  close(fd); return b;
}
static void save(const char *path, const struct blob *b) {
  require(path != NULL, "--output is required");
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) fail(strerror(errno));
  size_t done = 0;
  while (done < b->size) {
    ssize_t n = write(fd, b->data + done, b->size - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); unlink(path); fail("output write failed"); }
    done += (size_t)n;
  }
  if (fsync(fd)) { close(fd); unlink(path); fail("output fsync failed"); }
  require(!close(fd), "output close failed");
}
static int64_t branch_target(uint32_t code) {
  if (code >> 26 != 5) return -1;
  int64_t delta = code & 0x3ffffffu;
  if (delta & 0x2000000u) delta -= 0x4000000u;
  return 4 + delta * 4;
}

static struct avb parse_avb(const struct blob *b, int require_hash) {
  require(b->size >= PAGE + 64, "BOOT is too small");
  const unsigned char *footer = b->data + b->size - 64;
  require(!memcmp(footer, "AVBf", 4) && be32(footer + 4) == 1 && !be32(footer + 8) &&
          !nonzero(footer + 36, 28), "unsupported AVB footer");
  struct avb a = { .original_bytes = be64(footer + 12), .vbmeta_offset = be64(footer + 20),
                  .vbmeta_bytes = be64(footer + 28) };
  require(a.original_bytes >= PAGE && a.original_bytes <= a.vbmeta_offset &&
          a.vbmeta_bytes >= 256 && a.vbmeta_bytes <= 65536 &&
          range(a.vbmeta_offset, a.vbmeta_bytes, b->size - 64), "AVB extent is outside BOOT");
  const unsigned char *v = b->data + a.vbmeta_offset;
  require(!memcmp(v, "AVB0", 4) && be32(v + 4) == 1 && !be32(v + 8), "unsupported vbmeta version");
  uint64_t auth = be64(v + 12), aux = be64(v + 20), off = be64(v + 96);
  a.descriptor_bytes = be64(v + 104); a.rollback = be64(v + 112);
  require(range(256, auth, a.vbmeta_bytes) && range(256 + auth, aux, a.vbmeta_bytes) &&
          256 + auth + aux == a.vbmeta_bytes && range(off, a.descriptor_bytes, aux) &&
          !be32(v + 120) && !be32(v + 124), "unsupported vbmeta layout or top-level policy");
  a.descriptors = v + 256 + auth + off;
  unsigned hashes = 0, fingerprint = 0;
  for (uint64_t at = 0; at < a.descriptor_bytes;) {
    const unsigned char *d = a.descriptors + at;
    require(range(at, 16, a.descriptor_bytes), "truncated AVB descriptor");
    uint64_t bytes = be64(d + 8);
    require(bytes <= a.descriptor_bytes - at - 16 && !(bytes & 7), "invalid AVB descriptor extent");
    bytes += 16;
    if (be64(d) == 2) {
      require(bytes >= 132 && !memcmp(d + 24, "sha256", 7) && !nonzero(d + 31, 25), "only SHA256 boot hash is supported");
      uint32_t name = be32(d + 56), salt = be32(d + 60), hash = be32(d + 64);
      uint64_t image = be64(d + 16), payload = (uint64_t)name + salt + hash;
      require(name == 4 && hash == 32 && !be32(d + 68) && !nonzero(d + 72, 60) &&
              range(132, payload, bytes) && align_to(132 + payload, 8) == bytes &&
              image == a.original_bytes && image <= a.vbmeta_offset && !memcmp(d + 132, "boot", 4),
              "unsupported boot hash descriptor");
      SHA2_CTX ctx; unsigned char calculated[32];
      SHA256Init(&ctx); SHA256Update(&ctx, d + 132 + name, salt);
      SHA256Update(&ctx, b->data, (size_t)image); SHA256Final(calculated, &ctx);
      a.hash_matches = !memcmp(calculated, d + 132 + name + salt, 32); hashes++;
    } else if (be64(d) == 0) {
      require(bytes >= 34, "truncated AVB property");
      uint64_t key = be64(d + 16), value = be64(d + 24);
      require(key <= bytes - 34 && value <= bytes - 34 - key &&
              !d[32 + key] && !d[33 + key + value], "invalid AVB property extent");
      const char *wanted = "com.android.build.boot.fingerprint";
      if (key == strlen(wanted) && !memcmp(d + 32, wanted, key)) {
        require(value >= 13 && value < sizeof(a.fingerprint) &&
                !memchr(d + 33 + key, 0, (size_t)value) &&
                !memcmp(d + 33 + key, "Xiaomi/piano/", 13), "BOOT is not the supported Piano ROM family");
        memcpy(a.fingerprint, d + 33 + key, (size_t)value);
        fingerprint++;
      }
    } else fail("unsupported AVB descriptor; no metadata will be discarded");
    at += bytes;
  }
  require(hashes == 1 && fingerprint == 1, "expected one boot hash and Piano fingerprint");
  require(!require_hash || a.hash_matches, "BOOT hash descriptor does not match the input");
  return a;
}
static struct boot parse_boot(const struct blob *b, int verify_avb) {
  require(b->size >= PAGE && !memcmp(b->data, "ANDROID!", 8) && le32(b->data + 40) == 4 &&
          le32(b->data + 20) == 1584 && !le32(b->data + 12) && !le32(b->data + 1580),
          "expected BOOT4 with separate init_boot and no boot_signature section");
  struct boot s = { .kernel_bytes = le32(b->data + 8) };
  require(s.kernel_bytes >= PAGE && range(PAGE, s.kernel_bytes, b->size - 64), "invalid kernel extent");
  const unsigned char *k = b->data + PAGE;
  s.span = le64(k + 16);
  require(!memcmp(k, "MZ", 2) && !memcmp(k + 56, "ARMd", 4) &&
          !(le64(k + 24) & 1) && !(le64(k + 8) % (2u * 1024u * 1024u)) &&
          s.span >= s.kernel_bytes && s.span < BOOT_BYTES, "unsupported raw AArch64 GKI span or placement");
  int64_t target = branch_target(le32(k + 4));
  require(target >= 64 && (uint64_t)target < s.kernel_bytes, "unsupported GKI branch");
  s.avb = parse_avb(b, verify_avb);
  require(PAGE + (uint64_t)s.kernel_bytes <= s.avb.original_bytes, "kernel exceeds the AVB original image");
  return s;
}
static const unsigned char *find_split(const struct blob *b, const struct boot *s) {
  uint64_t selector = (uint64_t)branch_target(le32(b->data + PAGE + 4));
  uint64_t end = selector + 65536 < s->kernel_bytes ? selector + 65536 : s->kernel_bytes;
  const unsigned char *found = NULL;
  for (uint64_t at = selector; range(at, 128, end); at++) {
    const unsigned char *p = b->data + PAGE + at;
    if (memcmp(p, split_magic, 16)) continue;
    if (le32(p + 16) != 1 || le32(p + 20) != 128 || le64(p + 24) != selector ||
        le64(p + 88) != s->kernel_bytes) continue;
    require(found == NULL, "multiple SPLIT metadata records"); found = p;
  }
  return found;
}
static void initial_request(unsigned char *p, const unsigned char app_sha[32]) {
  memcpy(p, request_magic, 16); put32(p + 16, 1); put32(p + 20, 64);
  memcpy(p + 40, app_sha, 16); put32(p + 36, PianoBootRequestCrc32(p));
}
static void check_app(const struct blob *app) {
  unsigned char hash[32];
  require(app->size >= 64 && !memcmp(app->data, app_magic, 16) &&
          le32(app->data + 16) == 1 && le32(app->data + 20) == 64 &&
          le64(app->data + 24) == app->size - 64, "invalid APPv1 envelope");
  sha(app->data + 64, app->size - 64, hash);
  require(!memcmp(hash, app->data + 32, 32), "APPv1 payload hash mismatch");
}
static size_t sparse_tail(const struct blob *source, size_t from, unsigned char *dst, uint64_t *count) {
  size_t used = 0; *count = 0;
  for (size_t off = from; off < source->size;) {
    size_t bytes = source->size - off < PAGE ? source->size - off : PAGE;
    if (nonzero(source->data + off, bytes)) {
      require(used + 16 + bytes <= CATALOG_LIMIT - CATALOG_HEADER - PAGE - 64,
              "nonzero original padding does not fit the bounded restore catalog");
      if (dst) { put64(dst + used, off); put64(dst + used + 8, bytes); memcpy(dst + used + 16, source->data + off, bytes); }
      used += 16 + bytes; (*count)++;
    }
    off += bytes;
  }
  return used;
}
static void add_avb(struct blob *output, uint64_t original, const struct avb *stock) {
  uint64_t properties = 0;
  for (uint64_t at = 0; at < stock->descriptor_bytes;) {
    const unsigned char *p = stock->descriptors + at; uint64_t size = 16 + be64(p + 8);
    if (!be64(p)) properties += size;
    at += size;
  }
  const uint64_t hash_bytes = 168, desc_bytes = properties + hash_bytes;
  uint64_t aux = align_to(desc_bytes, 64), meta_bytes = 256 + aux, offset = align_to(original, PAGE);
  require(range(offset, align_to(meta_bytes, PAGE), output->size - PAGE), "wrapped AVB does not fit BOOT");
  unsigned char *v = output->data + offset, *d = v + 256;
  memcpy(v, "AVB0", 4); putbe32(v + 4, 1); putbe64(v + 20, aux);
  putbe64(v + 104, desc_bytes); putbe64(v + 112, stock->rollback);
  memcpy(v + 128, "piano-boot-repack file-mode 1", 28);
  putbe64(d, 2); putbe64(d + 8, hash_bytes - 16); putbe64(d + 16, original);
  memcpy(d + 24, "sha256", 6); putbe32(d + 56, 4); putbe32(d + 64, 32);
  memcpy(d + 132, "boot", 4); sha(output->data, (size_t)original, d + 136);
  d += hash_bytes;
  for (uint64_t at = 0; at < stock->descriptor_bytes;) {
    const unsigned char *p = stock->descriptors + at; uint64_t size = 16 + be64(p + 8);
    if (!be64(p)) { memcpy(d, p, size); d += size; }
    at += size;
  }
  unsigned char *f = output->data + output->size - 64;
  memcpy(f, "AVBf", 4); putbe32(f + 4, 1); putbe64(f + 12, original);
  putbe64(f + 20, offset); putbe64(f + 28, meta_bytes);
}

static struct blob repack(const struct blob *source, const struct boot *s,
                          const struct blob *selector, uint64_t memory, uint64_t metadata,
                          const struct blob *shim, const struct blob *fd, const struct blob *app) {
  require(!memmem(source->data + PAGE, s->kernel_bytes, split_magic, 16), "input is already SPLIT wrapped");
  require(selector->size >= 4 && !(selector->size & 3) && selector->size <= memory &&
          memory > 8192 && memory < 65536 && !(metadata & 15) && range(metadata, 128, selector->size) &&
          !nonzero(selector->data + metadata, 128), "unsupported selector file/memory/metadata layout");
  require(fd->size == 0x300000 && shim->size >= 64 && !memcmp(shim->data + 56, "ARMd", 4), "invalid FD or BootShim");
  check_app(app);
  uint64_t request = align_to(s->span, PAGE), sel = request + 2 * PAGE;
  uint64_t sh = align_to(sel + memory, 16), ap = align_to(sh + shim->size + fd->size, 16);
  uint64_t catalog = align_to(ap + app->size, 16), records = 0;
  size_t tail = sparse_tail(source, PAGE + s->kernel_bytes, NULL, &records);
  uint64_t catalog_bytes = CATALOG_HEADER + PAGE + 64 + tail, total = align_to(catalog + catalog_bytes, PAGE);
  require(total < 0x08000000 && total + PAGE + 69632 < BOOT_BYTES, "payload does not fit the 96MiB BOOT carrier");
  struct blob out = { .data = calloc(1, BOOT_BYTES), .size = BOOT_BYTES };
  require(out.data != NULL, "out of memory");
  memcpy(out.data, source->data, PAGE + s->kernel_bytes); put32(out.data + 8, (uint32_t)total);
  unsigned char *k = out.data + PAGE;
  put32(k + 4, 0x14000000u | (uint32_t)((sel - 4) / 4)); put64(k + 16, total);
  unsigned char app_hash[32]; sha(app->data, app->size, app_hash);
  initial_request(k + request, app_hash); initial_request(k + request + PAGE, app_hash);
  memcpy(k + sel, selector->data, selector->size);
  unsigned char *m = k + sel + metadata;
  memcpy(m, split_magic, 16); put32(m + 16, 1); put32(m + 20, 128);
  put64(m + 24, sel); put64(m + 32, s->span);
  put32(m + 40, le32(source->data + PAGE)); put32(m + 44, le32(source->data + PAGE + 4));
  put64(m + 48, sh); put64(m + 56, shim->size); put64(m + 64, fd->size);
  put64(m + 72, ap); put64(m + 80, app->size); put64(m + 88, total); memcpy(m + 96, app_hash, 32);
  memcpy(k + sh, shim->data, shim->size); memcpy(k + sh + shim->size, fd->data, fd->size);
  memcpy(k + ap, app->data, app->size);
  unsigned char *c = k + catalog;
  memcpy(c, catalog_magic, 16); put32(c + 16, 1); put32(c + 20, CATALOG_HEADER);
  put64(c + 24, catalog_bytes); put64(c + 32, source->size); put64(c + 40, s->kernel_bytes);
  put64(c + 48, s->span); put64(c + 56, PAGE + s->kernel_bytes); put64(c + 64, records);
  put64(c + 72, selector->size); put64(c + 80, memory); put64(c + 88, metadata);
  sha(source->data, source->size, c + 96); sha(source->data + PAGE, s->kernel_bytes, c + 128);
  sha(selector->data, selector->size, c + 160); sha(shim->data, shim->size, c + 192);
  sha(fd->data, fd->size, c + 224); memcpy(c + 256, app_hash, 32);
  memcpy(c + CATALOG_HEADER, source->data, PAGE);
  memcpy(c + CATALOG_HEADER + PAGE, source->data + PAGE, 64);
  sparse_tail(source, PAGE + s->kernel_bytes, c + CATALOG_HEADER + PAGE + 64, &records);
  sha(c + CATALOG_HEADER, (size_t)(catalog_bytes - CATALOG_HEADER), c + 288);
  add_avb(&out, PAGE + total, &s->avb);
  return out;
}
static struct blob restore(const struct blob *wrapped, const struct boot *s, const unsigned char *m,
                           uint64_t *catalog_size, uint64_t *tail_records) {
  require(m != NULL, "SPLITv1 metadata not found");
  uint64_t selector = le64(m + 24), span = le64(m + 32), shim = le64(m + 48);
  uint64_t shim_bytes = le64(m + 56), fd_bytes = le64(m + 64), app = le64(m + 72), app_bytes = le64(m + 80);
  require(span >= PAGE && span < s->kernel_bytes && selector == align_to(span, PAGE) + 2 * PAGE &&
          range(shim, shim_bytes, s->kernel_bytes) && fd_bytes == 0x300000 &&
          range(shim + shim_bytes, fd_bytes, s->kernel_bytes) && app >= shim + shim_bytes + fd_bytes &&
          range(app, app_bytes, s->kernel_bytes), "invalid SPLITv1 payload extents");
  uint64_t catalog = align_to(app + app_bytes, 16);
  require(range(catalog, CATALOG_HEADER + PAGE + 64, s->kernel_bytes), "no lossless restore catalog; old host carrier cannot be restored as a whole BOOT");
  const unsigned char *k = wrapped->data + PAGE, *c = k + catalog;
  require(!memcmp(c, catalog_magic, 16) && le32(c + 16) == 1 && le32(c + 20) == CATALOG_HEADER,
          "unsupported restore catalog");
  uint64_t bytes = le64(c + 24), original = le64(c + 32), kernel = le64(c + 40), tail = le64(c + 56), count = le64(c + 64);
  uint64_t sel_bytes = le64(c + 72), memory = le64(c + 80), metadata = le64(c + 88);
  require(bytes >= CATALOG_HEADER + PAGE + 64 && bytes <= CATALOG_LIMIT && range(catalog, bytes, s->kernel_bytes) &&
          align_to(catalog + bytes, PAGE) == s->kernel_bytes && original <= BOOT_BYTES &&
          kernel >= PAGE && kernel <= span && tail == PAGE + kernel && tail <= original &&
          le64(c + 48) == span && memory > 8192 && memory < 65536 && sel_bytes <= memory &&
          range(selector, memory, shim) && range(metadata, 128, sel_bytes) &&
          m == k + selector + metadata, "invalid restore catalog dimensions");
  unsigned char hash[32];
  sha(c + CATALOG_HEADER, (size_t)(bytes - CATALOG_HEADER), hash);
  require(!memcmp(hash, c + 288, 32), "restore catalog body hash mismatch");
  struct blob app_blob = { .data = (unsigned char *)k + app, .size = (size_t)app_bytes };
  check_app(&app_blob); sha(k + app, (size_t)app_bytes, hash);
  require(!memcmp(hash, c + 256, 32) && !memcmp(hash, m + 96, 32), "SPLIT APP generation differs from restore catalog");
  sha(k + shim, (size_t)shim_bytes, hash); require(!memcmp(hash, c + 192, 32), "BootShim hash mismatch");
  sha(k + shim + shim_bytes, (size_t)fd_bytes, hash); require(!memcmp(hash, c + 224, 32), "FD hash mismatch");
  unsigned char *sel_copy = malloc((size_t)sel_bytes); require(sel_copy != NULL, "out of memory");
  memcpy(sel_copy, k + selector, (size_t)sel_bytes); memset(sel_copy + metadata, 0, 128);
  sha(sel_copy, (size_t)sel_bytes, hash); free(sel_copy);
  require(!memcmp(hash, c + 160, 32), "selector hash mismatch");
  const unsigned char *header = c + CATALOG_HEADER, *kh = header + PAGE;
  require(le32(header + 8) == kernel && le64(kh + 16) == span &&
          le32(kh) == le32(m + 40) && le32(kh + 4) == le32(m + 44), "original headers disagree with SPLIT metadata");
  unsigned char current_header[PAGE]; memcpy(current_header, header, PAGE); put32(current_header + 8, s->kernel_bytes);
  require(!memcmp(current_header, wrapped->data, PAGE), "current BOOT header is not the owned wrapper header");
  struct blob out = { .data = calloc(1, (size_t)original), .size = (size_t)original };
  require(out.data != NULL, "out of memory");
  memcpy(out.data, header, PAGE); memcpy(out.data + PAGE, k, (size_t)kernel); memcpy(out.data + PAGE, kh, 64);
  sha(out.data + PAGE, (size_t)kernel, hash); require(!memcmp(hash, c + 128, 32), "original GKI cannot be reconstructed exactly");
  uint64_t at = CATALOG_HEADER + PAGE + 64, end = tail;
  for (uint64_t i = 0; i < count; i++) {
    require(range(at, 16, bytes), "truncated original padding record");
    uint64_t offset = le64(c + at), size = le64(c + at + 8); at += 16;
    require(offset >= end && size > 0 && size <= PAGE && range(offset, size, original) &&
            range(at, size, bytes), "invalid original padding record");
    memcpy(out.data + offset, c + at, (size_t)size); end = offset + size; at += size;
  }
  require(at == bytes, "unexpected restore catalog bytes");
  sha(out.data, out.size, hash); require(!memcmp(hash, c + 96, 32), "reconstructed BOOT SHA256 differs from the original");
  (void)parse_boot(&out, 1);
  *catalog_size = bytes; *tail_records = count; return out;
}
static uint64_t number(const char *p) {
  char *end; errno = 0; uint64_t n = strtoull(p, &end, 0);
  require(!errno && p[0] && p[0] != '-' && !*end, "invalid numeric option"); return n;
}
/* The policy/state parser is the pinned MIT jsmn tokenizer, not string matching.
 * Metadata contains hashes and identities only; no full BOOT backup is saved. */
struct json { struct blob text; jsmntok_t tokens[4096]; int count; };
static int token_is(const struct json *j, int t, const char *value) {
  return t >= 0 && t < j->count && j->tokens[t].end - j->tokens[t].start == (int)strlen(value) &&
         !memcmp(j->text.data + j->tokens[t].start, value, strlen(value));
}
static int token_after(const struct json *j, int t) {
  int end = j->tokens[t].end;
  do { t++; } while (t < j->count && j->tokens[t].start < end);
  return t;
}
static int member(const struct json *j, int object, const char *key) {
  require(object >= 0 && object < j->count && j->tokens[object].type == JSMN_OBJECT, "expected JSON object");
  int found = -1;
  for (int t = object + 1; t < j->count && j->tokens[t].start < j->tokens[object].end;) {
    require(j->tokens[t].type == JSMN_STRING && t + 1 < j->count, "invalid JSON member");
    if (token_is(j, t, key)) { require(found < 0, "duplicate JSON member"); found = t + 1; }
    t = token_after(j, t + 1);
  }
  return found;
}
static void string_token(const struct json *j, int t, char *out, size_t capacity) {
  require(t >= 0 && j->tokens[t].type == JSMN_STRING, "missing JSON string");
  size_t n = 0;
  for (int i = j->tokens[t].start; i < j->tokens[t].end; i++) {
    unsigned char c = j->text.data[i];
    if (c == '\\') {
      c = j->text.data[++i];
      switch (c) {
        case '"': case '/': case '\\': break;
        case 'b': c = '\b'; break; case 'f': c = '\f'; break;
        case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
        case 'u': {
          unsigned value = 0;
          for (unsigned k = 0; k < 4; k++) {
            unsigned char digit = j->text.data[++i];
            value = value * 16 + (digit <= '9' ? digit - '0' : (digit | 32) - 'a' + 10);
          }
          require(value > 0 && value < 128, "identity JSON must use ASCII strings"); c = (unsigned char)value; break;
        }
        default: fail("invalid JSON escape");
      }
    }
    require(c > 0 && c < 128 && n + 1 < capacity, "invalid or oversized JSON string"); out[n++] = (char)c;
  }
  out[n] = 0;
}
static void json_string(const struct json *j, int object, const char *key, char *out, size_t capacity) {
  string_token(j, member(j, object, key), out, capacity);
}
static uint64_t json_number(const struct json *j, int object, const char *key) {
  int t = member(j, object, key);
  require(t >= 0 && j->tokens[t].type == JSMN_PRIMITIVE &&
          j->tokens[t].end - j->tokens[t].start < 32, "missing JSON integer");
  char value[32]; int n = j->tokens[t].end - j->tokens[t].start;
  memcpy(value, j->text.data + j->tokens[t].start, (size_t)n); value[n] = 0;
  for (int i = 0; i < n; i++) require(value[i] >= '0' && value[i] <= '9', "invalid JSON integer");
  char *end; errno = 0; uint64_t result = strtoull(value, &end, 10);
  require(n > 0 && !errno && !*end, "invalid JSON integer"); return result;
}
static int json_true(const struct json *j, int object, const char *key) {
  int t = member(j, object, key);
  return t >= 0 && j->tokens[t].type == JSMN_PRIMITIVE && token_is(j, t, "true");
}
static void string_matches(const struct json *j, int object, const char *key, const char *expected) {
  char actual[1024]; json_string(j, object, key, actual, sizeof(actual));
  require(!strcmp(actual, expected), "policy/state/source identity differs");
}
static void read_json(const char *path, struct json *j) {
  j->text = load(path, 256 * 1024); jsmn_parser parser; jsmn_init(&parser);
  j->count = jsmn_parse(&parser, (const char *)j->text.data, j->text.size, j->tokens, 4096);
  require(j->count > 0 && j->tokens[0].type == JSMN_OBJECT && token_after(j, 0) == j->count,
          "invalid or oversized JSON document");
  for (int i = 0; i < j->tokens[0].start; i++)
    require(j->text.data[i] && strchr(" \r\n\t", j->text.data[i]) != NULL, "invalid JSON prefix");
  for (size_t i = (size_t)j->tokens[0].end; i < j->text.size; i++)
    require(j->text.data[i] && strchr(" \r\n\t", j->text.data[i]) != NULL, "trailing JSON content");
  /* Validate all object keys once, including unconsumed members. */
  for (int o = 0; o < j->count; o++) if (j->tokens[o].type == JSMN_OBJECT) {
    for (int t = o + 1; t < j->count && j->tokens[t].start < j->tokens[o].end;) {
      char key[128]; string_token(j, t, key, sizeof(key));
      require(!memchr(j->text.data + j->tokens[t].start, '\\', (size_t)(j->tokens[t].end - j->tokens[t].start)),
              "escaped JSON member names are unsupported");
      (void)member(j, o, key); t = token_after(j, t + 1);
    }
  }
}
static void quote(FILE *f, const char *s) {
  fputc('"', f);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
    else if (*p < 32 || *p >= 127) fprintf(f, "\\u%04x", *p);
    else fputc(*p, f);
  }
  fputc('"', f);
}
struct identity { char slot[16], node[64], rom[1024], bootprop[1024]; };
struct snapshot { struct identity id; struct stat st; int fd; struct blob bytes; struct boot boot; char sha[65]; };
struct options {
  const char *action, *device, *slot, *rom, *bootprop, *policy, *output, *source, *state, *state_output, *payload, *target, *input;
  int execute, preview, reject_wrapped, require_ready, read_only;
};
static void property(const char *name, char *value, size_t capacity) {
  int p[2]; require(!pipe2(p, O_CLOEXEC), "getprop pipe failed");
  pid_t child = fork(); require(child >= 0, "getprop fork failed");
  if (!child) {
    close(p[0]); if (dup2(p[1], STDOUT_FILENO) < 0) _exit(127); close(p[1]);
    execl("/system/bin/getprop", "getprop", name, (char *)NULL); _exit(127);
  }
  close(p[1]); size_t done = 0;
  for (;;) {
    unsigned char c; ssize_t n = read(p[0], &c, 1);
    if (n < 0 && errno == EINTR) continue;
    require(n >= 0, "getprop read failed"); if (!n) break;
    require(c > 0 && c < 128 && done + 1 < capacity, "invalid or oversized Android property"); value[done++] = (char)c;
  }
  close(p[0]); int status;
  while (waitpid(child, &status, 0) < 0) require(errno == EINTR, "getprop wait failed");
  require(WIFEXITED(status) && !WEXITSTATUS(status), "Android /system/bin/getprop is unavailable");
  while (done && (value[done - 1] == '\n' || value[done - 1] == '\r')) done--;
  value[done] = 0;
}
static struct identity current_identity(const struct options *o, int write) {
  struct identity id = {0}; char value[64];
  property("ro.product.device", value, sizeof(value)); require(!strcmp(value, "piano"), "only the current Piano Android is supported");
  property("ro.boot.slot_suffix", id.slot, sizeof(id.slot));
  require(!strcmp(id.slot, "_a") || !strcmp(id.slot, "_b"), "unknown active Android slot");
  snprintf(id.node, sizeof(id.node), "/dev/block/by-name/boot%s", id.slot);
  property("ro.build.fingerprint", id.rom, sizeof(id.rom));
  property("ro.bootimage.build.fingerprint", id.bootprop, sizeof(id.bootprop));
  /* These are independent identities. The actual BOOT footer may describe a
   * newer image than ro.bootimage.build.fingerprint on the same running ROM. */
  require(id.rom[0] && id.bootprop[0], "current ROM/bootimage properties are missing");
  require(!o->slot || !strcmp(o->slot, id.slot), "requested slot is not the current Android slot");
  require(!o->device || !strcmp(o->device, id.node), "only the exact active boot_a/boot_b node is allowed");
  require(!o->rom || !strcmp(o->rom, id.rom), "ROM changed since invocation");
  require(!o->bootprop || !strcmp(o->bootprop, id.bootprop), "bootimage property changed since invocation");
  if (write) {
    property("ro.boot.flash.locked", value, sizeof(value)); require(!strcmp(value, "0"), "BOOT writes require an unlocked bootloader");
    property("sys.boot_completed", value, sizeof(value)); require(!strcmp(value, "1"), "BOOT writes require a normally booted Android ROM");
  }
  return id;
}
static void read_exact(int fd, void *buffer, size_t bytes, uint64_t offset) {
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = pread(fd, (unsigned char *)buffer + done, bytes - done, (off_t)(offset + done));
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "BOOT read failed"); done += (size_t)n;
  }
}
static void fd_hash(int fd, size_t bytes, char result[65]) {
  unsigned char buffer[65536], hash[32]; SHA2_CTX ctx; SHA256Init(&ctx);
  for (size_t at = 0; at < bytes;) {
    size_t n = bytes - at < sizeof(buffer) ? bytes - at : sizeof(buffer);
    read_exact(fd, buffer, n, at); SHA256Update(&ctx, buffer, n); at += n;
  }
  SHA256Final(hash, &ctx); hex(hash, result);
}
static struct snapshot snapshot(const struct options *o) {
  struct snapshot s = { .id = current_identity(o, 0) };
  s.fd = open(s.id.node, O_RDONLY | O_CLOEXEC);
  require(s.fd >= 0 && !fstat(s.fd, &s.st) && S_ISBLK(s.st.st_mode), "active BOOT block device is unavailable");
  require(!flock(s.fd, LOCK_EX | LOCK_NB), "another BOOT operation holds the active node");
  uint64_t capacity = 0;
  require(!ioctl(s.fd, BLKGETSIZE64, &capacity) && capacity == BOOT_BYTES, "active BOOT is not the supported 96MiB carrier");
  s.bytes.size = (size_t)capacity; s.bytes.data = malloc(s.bytes.size); require(s.bytes.data != NULL, "out of memory");
  read_exact(s.fd, s.bytes.data, s.bytes.size, 0); hash_hex(s.bytes.data, s.bytes.size, s.sha);
  s.boot = parse_boot(&s.bytes, 0); return s;
}
static void identity_fields(FILE *f, const struct snapshot *s) {
  fputs("\"active_slot\":", f); quote(f, s->id.slot); fputs(",\"boot_device\":", f); quote(f, s->id.node);
  fputs(",\"rom_fingerprint\":", f); quote(f, s->id.rom); fputs(",\"boot_fingerprint\":", f); quote(f, s->id.bootprop);
  fputs(",\"footer_fingerprint\":", f); quote(f, s->boot.avb.fingerprint);
  fprintf(f, ",\"boot_bytes\":%zu,\"device_rdev\":%ju", s->bytes.size, (uintmax_t)s->st.st_rdev);
}
static void check_identity(const struct json *j, const struct snapshot *s) {
  require(json_number(j, 0, "schema_version") == 1 && json_number(j, 0, "boot_bytes") == s->bytes.size &&
          json_number(j, 0, "device_rdev") == (uintmax_t)s->st.st_rdev, "BOOT metadata/device identity differs");
  string_matches(j, 0, "active_slot", s->id.slot); string_matches(j, 0, "boot_device", s->id.node);
  string_matches(j, 0, "rom_fingerprint", s->id.rom); string_matches(j, 0, "boot_fingerprint", s->id.bootprop);
  string_matches(j, 0, "footer_fingerprint", s->boot.avb.fingerprint);
}
static const unsigned char *catalog(const struct blob *b, const unsigned char *m) {
  return b->data + PAGE + align_to(le64(m + 72) + le64(m + 80), 16);
}
static uint64_t request_offset(const unsigned char *m) { return PAGE + align_to(le64(m + 32), PAGE); }
struct request_state { int valid[2], newest; uint64_t sequence[2]; unsigned target[2]; };
static struct request_state read_requests(const struct blob *b, const unsigned char *m) {
  struct request_state r = {0}; uint64_t offset = request_offset(m);
  require(range(offset, 2 * PAGE, b->size), "request pages outside BOOT");
  for (unsigned i = 0; i < 2; i++) {
    const unsigned char *p = b->data + offset + i * PAGE;
    require(!nonzero(p + 64, PAGE - 64), "unexpected data in owned NEXT page padding");
    r.sequence[i] = le64(p + 24); r.target[i] = le32(p + 32);
    r.valid[i] = !memcmp(p, request_magic, 16) && le32(p + 16) == 1 && le32(p + 20) == 64 &&
      r.target[i] <= PIANO_BOOT_REQUEST_SETUP && (r.sequence[i] || !r.target[i]) &&
      !memcmp(p + 40, m + 96, 16) && !nonzero(p + 56, 8) && le32(p + 36) == PianoBootRequestCrc32(p);
  }
  require(r.valid[0] || r.valid[1], "both owned request CRC/generation records are invalid");
  require(!(r.valid[0] && r.valid[1] && r.sequence[0] == r.sequence[1] && r.target[0] != r.target[1]), "conflicting request records");
  r.newest = !r.valid[0] || (r.valid[1] && r.sequence[1] > r.sequence[0]);
  require(r.target[r.newest] == PianoBootRequestSelect(b->data + offset, m + 96), "request reader/writer contract differs");
  return r;
}
static void wrapper_hash(const struct blob *b, const unsigned char *m, char result[65]) {
  (void)read_requests(b, m);
  uint64_t offset = request_offset(m); unsigned char pages[2 * PAGE] = {0}, hash[32];
  initial_request(pages, m + 96); initial_request(pages + PAGE, m + 96);
  SHA2_CTX ctx; SHA256Init(&ctx); SHA256Update(&ctx, b->data, (size_t)offset);
  SHA256Update(&ctx, pages, sizeof(pages));
  SHA256Update(&ctx, b->data + offset + sizeof(pages), b->size - (size_t)offset - sizeof(pages));
  SHA256Final(hash, &ctx); hex(hash, result);
}
static unsigned target_number(const char *target) {
  require(target != NULL, "--target is required");
  if (!strcmp(target, "android")) return PIANO_BOOT_REQUEST_NONE;
  if (!strcmp(target, "uefi") || !strcmp(target, "menu")) return PIANO_BOOT_REQUEST_UEFI_MENU;
  if (!strcmp(target, "linux")) return PIANO_BOOT_REQUEST_LINUX;
  if (!strcmp(target, "setup")) return PIANO_BOOT_REQUEST_SETUP;
  fail("unknown persistent request target"); return 0;
}
static unsigned update_request(struct blob *b, const unsigned char *m, unsigned target) {
  struct request_state r = read_requests(b, m);
  require(r.sequence[r.newest] < UINT64_MAX, "request sequence exhausted");
  unsigned page = !r.valid[0] ? 0 : (!r.valid[1] ? 1 : (r.sequence[0] <= r.sequence[1] ? 0 : 1));
  unsigned char *p = b->data + request_offset(m) + page * PAGE;
  memset(p, 0, 64); initial_request(p, m + 96); put64(p + 24, r.sequence[r.newest] + 1); put32(p + 32, target);
  put32(p + 36, PianoBootRequestCrc32(p)); r = read_requests(b, m);
  require(r.target[r.newest] == target, "new persistent request CRC/select failed"); return page;
}
static int policy_ready(const struct json *j) {
  static const char *tests[] = {"lossless_roundtrip", "current_source_guard", "active_slot_guard", "nested_wrapper_rejected",
    "ota_restore_refused", "write_readback", "payload_tamper_rejected", "request_owned_wrapper_only",
    "request_persistent_reselection_crc", "missing_request_stock_passthrough"};
  if (!json_true(j, 0, "zip_ready") || !json_true(j, 0, "device_passthrough_verified") ||
      !json_true(j, 0, "request_handling_verified") || !json_true(j, 0, "standard_recovery_preserved")) return 0;
  int object = member(j, 0, "native_tests"); if (object < 0) return 0;
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) if (!json_true(j, object, tests[i])) return 0;
  return 1;
}
static void policy_fields(FILE *f, const struct json *j) {
  fprintf(f, "\"device_passthrough_verified\":%s,\"request_handling_verified\":%s,\"standard_recovery_preserved\":%s,"
          "\"webui_bridge_verified\":%s,\"entry_policy\":\"explicit-request-only\",\"request_bootarg\":\"sunuefi.boot=uefi\"",
          json_true(j, 0, "device_passthrough_verified") ? "true" : "false",
          json_true(j, 0, "request_handling_verified") ? "true" : "false",
          json_true(j, 0, "standard_recovery_preserved") ? "true" : "false",
          json_true(j, 0, "webui_bridge_verified") ? "true" : "false");
}
static void check_policy(const struct json *j) {
  require(json_number(j, 0, "schema_version") == 1 && json_number(j, 0, "interface_version") == 1 &&
          json_number(j, 0, "wrapper_version") == 1 && json_number(j, 0, "app_abi") == 1,
          "unsupported module policy/ABI");
  string_matches(j, 0, "entry_policy", "explicit-request-only");
  string_matches(j, 0, "request_policy", "persistent-until-changed");
  int stock = member(j, 0, "stock_kernel_bundled"), ota = member(j, 0, "ota_automatic");
  require(stock >= 0 && ota >= 0 && j->tokens[stock].type == JSMN_PRIMITIVE && j->tokens[ota].type == JSMN_PRIMITIVE &&
          token_is(j, stock, "false") && token_is(j, ota, "false"), "unsupported module policy");
  int object = member(j, 0, "tool");
  int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC); struct stat st; char hash[65];
  require(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 32 * 1024 * 1024,
          "cannot verify the running native executable");
  fd_hash(fd, (size_t)st.st_size, hash); close(fd);
  require(json_number(j, object, "bytes") == (uint64_t)st.st_size, "policy native executable size differs");
  string_matches(j, object, "sha256", hash);
}
static struct blob payload(const struct json *policy, const char *directory, const char *name, size_t limit) {
  char path[4096], hash[65];
  require(directory && snprintf(path, sizeof(path), "%s/%s", directory, name) < (int)sizeof(path), "payload directory is required");
  struct blob b = load(path, limit); hash_hex(b.data, b.size, hash);
  int object = member(policy, member(policy, 0, "payloads"), name);
  require(json_number(policy, object, "bytes") == b.size, "module payload size differs");
  string_matches(policy, object, "sha256", hash); return b;
}
static void check_owned(const struct json *state, const struct snapshot *s, const unsigned char *m) {
  check_identity(state, s); char hash[65];
  string_matches(state, 0, "wrapper", "SPLITv1+RSTRv1"); wrapper_hash(&s->bytes, m, hash);
  string_matches(state, 0, "wrapper_sha256", hash); hex(catalog(&s->bytes, m) + 96, hash);
  string_matches(state, 0, "source_sha256", hash); hex(m + 96, hash); string_matches(state, 0, "app_sha256", hash);
}
static int ro_fd = -1, restore_ro;
static void reset_ro(void) {
  if (ro_fd >= 0 && restore_ro && ioctl(ro_fd, BLKROSET, &restore_ro))
    fprintf(stderr, "piano-boot-repack: could not restore the original BOOT readonly flag: %s\n", strerror(errno));
}
static void write_guarded(const struct options *o, const struct snapshot *s, const void *data,
                          size_t bytes, uint64_t offset, const char *expected_after) {
  struct identity id = current_identity(o, 1); char before[65], after[65];
  require(!strcmp(id.slot, s->id.slot) && !strcmp(id.node, s->id.node) &&
          !strcmp(id.rom, s->id.rom) && !strcmp(id.bootprop, s->id.bootprop), "Android identity changed before BOOT write");
  fd_hash(s->fd, s->bytes.size, before); require(!strcmp(before, s->sha), "active BOOT source hash changed before write");
  require(range(offset, bytes, s->bytes.size), "write is outside the active BOOT");
  ro_fd = s->fd; require(!ioctl(ro_fd, BLKROGET, &restore_ro), "cannot inspect active BOOT readonly flag");
  if (restore_ro) { int zero = 0; require(!ioctl(ro_fd, BLKROSET, &zero), "cannot temporarily enable active BOOT writes"); }
  int fd = open(id.node, O_RDWR | O_SYNC | O_CLOEXEC); struct stat st; uint64_t capacity = 0;
  require(fd >= 0 && !fstat(fd, &st) && S_ISBLK(st.st_mode) && st.st_rdev == s->st.st_rdev &&
          !ioctl(fd, BLKGETSIZE64, &capacity) && capacity == s->bytes.size, "active BOOT changed on write reopen");
  id = current_identity(o, 1);
  require(!strcmp(id.slot, s->id.slot) && !strcmp(id.rom, s->id.rom) && !strcmp(id.bootprop, s->id.bootprop), "Android identity changed on write reopen");
  fd_hash(fd, s->bytes.size, before); require(!strcmp(before, s->sha), "active BOOT source changed on write reopen");
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = pwrite(fd, (const unsigned char *)data + done, bytes - done, (off_t)(offset + done));
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "active BOOT write failed; do not reboot without inspecting the current partition"); done += (size_t)n;
  }
  require(!fsync(fd), "active BOOT fsync failed"); fd_hash(fd, s->bytes.size, after);
  require(!strcmp(after, expected_after), "active BOOT complete readback SHA256 differs");
  require(!close(fd), "active BOOT write close failed");
  if (restore_ro) require(!ioctl(ro_fd, BLKROSET, &restore_ro), "could not restore original BOOT readonly flag");
  ro_fd = -1; restore_ro = 0;
}
static void sync_parent(const char *path) {
  char directory[4096]; require(strlen(path) < sizeof(directory), "metadata path is too long"); strcpy(directory, path);
  char *slash = strrchr(directory, '/');
  if (slash == directory) slash[1] = 0; else if (slash) *slash = 0; else strcpy(directory, ".");
  int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(fd >= 0 && !fsync(fd), "metadata directory fsync failed"); close(fd);
}
static struct blob installation_state(const struct snapshot *s, const struct blob *wrapped, const unsigned char *m, const char *status) {
  struct blob b = {0}; FILE *f = open_memstream((char **)&b.data, &b.size); require(f != NULL, "metadata allocation failed");
  char wrapper[65], app[65]; wrapper_hash(wrapped, m, wrapper); hex(m + 96, app);
  fputs("{\"schema_version\":1,\"status\":", f); quote(f, status); fputc(',', f); identity_fields(f, s);
  fprintf(f, ",\"wrapper\":\"SPLITv1+RSTRv1\",\"source_sha256\":\"%s\",\"wrapper_sha256\":\"%s\",\"app_sha256\":\"%s\","
          "\"request_policy\":\"persistent-until-changed\",\"full_partition_backup\":false}\n", s->sha, wrapper, app);
  require(!fclose(f), "metadata formatting failed"); return b;
}
static void finish_state(const char *path, const struct blob *b) {
  char temporary[4096];
  require(snprintf(temporary, sizeof(temporary), "%s.complete.%ld", path, (long)getpid()) < (int)sizeof(temporary), "state path is too long");
  save(temporary, b); require(!rename(temporary, path), "state rename failed"); sync_parent(path);
}
static void emit_result(const struct blob *b, const char *path) {
  if (path) { save(path, b); sync_parent(path); }
  require(fwrite(b->data, 1, b->size, stdout) == b->size, "JSON output failed");
}
static int online_main(int argc, char **argv) {
  struct options o = { .action = argv[1] };
  for (int i = 2; i < argc; i++) {
    const char *name = argv[i];
    if (!strcmp(name, "--execute")) { require(!o.execute, "duplicate --execute"); o.execute = 1; continue; }
    if (!strcmp(name, "--preview")) { require(!o.preview, "duplicate --preview"); o.preview = 1; continue; }
    if (!strcmp(name, "--read-only")) { o.read_only = 1; continue; }
    if (!strcmp(name, "--reject-wrapped")) { o.reject_wrapped = 1; continue; }
    if (!strcmp(name, "--require-ready")) { o.require_ready = 1; continue; }
    if (!strcmp(name, "--json")) continue;
    require(i + 1 < argc, "option needs a value"); const char *value = argv[++i]; const char **field = NULL;
    if (!strcmp(name, "--interface-version")) { require(!strcmp(value, "1"), "unsupported interface version"); continue; }
    if (!strcmp(name, "--boot-device")) field = &o.device;
    else if (!strcmp(name, "--active-slot")) field = &o.slot;
    else if (!strcmp(name, "--rom-fingerprint")) field = &o.rom;
    else if (!strcmp(name, "--boot-fingerprint")) field = &o.bootprop;
    else if (!strcmp(name, "--policy")) field = &o.policy;
    else if (!strcmp(name, "--output")) field = &o.output;
    else if (!strcmp(name, "--source-metadata")) field = &o.source;
    else if (!strcmp(name, "--installed-state")) field = &o.state;
    else if (!strcmp(name, "--state-output")) field = &o.state_output;
    else if (!strcmp(name, "--payload-dir")) field = &o.payload;
    else if (!strcmp(name, "--target")) field = &o.target;
    else if (!strcmp(name, "--input")) field = &o.input;
    require(field && !*field, "unknown/duplicate interface option"); *field = value;
  }
  require(!(o.execute && (o.preview || o.read_only)), "write and read-only modes conflict");
  require(!strcmp(o.action, "status") || !strcmp(o.action, "probe") || !strcmp(o.action, "repack") ||
          !strcmp(o.action, "restore") || !strcmp(o.action, "request"), "unknown command");
  /* A real wrapped file can exercise the same persistent CRC writer, without Android or fake block nodes. */
  if (o.input) {
    require(!strcmp(o.action, "request") && !o.device && !o.slot && !o.policy && !o.state && !o.execute,
            "file request uses --input/--output, never --execute or device identities");
    struct blob b = load(o.input, BOOT_BYTES); struct boot boot = parse_boot(&b, 0); const unsigned char *m = find_split(&b, &boot);
    uint64_t bytes, records; struct blob original = restore(&b, &boot, m, &bytes, &records); free(original.data);
    unsigned target = target_number(o.target); if (!o.preview) { (void)update_request(&b, m, target); save(o.output, &b); }
    struct request_state r = read_requests(&b, m);
    char full_sha[65]; hash_hex(b.data, b.size, full_sha);
    printf("{\"status\":\"%s\",\"target\":%u,\"sequence\":%" PRIu64 ",\"requested_target\":%u,"
           "\"crc_valid\":true,\"persistent\":true,\"file_sha256\":\"%s\",\"device_operation_performed\":false}\n",
           o.preview ? "FILE_REQUEST_PREVIEW" : "FILE_REQUEST_SAVED", r.target[r.newest], r.sequence[r.newest], target, full_sha);
    free(b.data); return 0;
  }
  struct json policy = {0}; read_json(o.policy, &policy); check_policy(&policy); int ready = policy_ready(&policy);
  require(!o.require_ready || ready, "module remains unverified: actual online writes, persistent request handling and stock Recovery evidence are required");
  require(!o.execute || !strcmp(o.action, "repack") || !strcmp(o.action, "restore") || !strcmp(o.action, "request"), "this command is read-only");
  if (!strcmp(o.action, "status") && !o.device) {
    printf("{\"status\":\"%s\",\"interface_version\":1,\"module_ready\":%s,\"online_interface_implemented\":true,"
           "\"request_policy\":\"persistent-until-changed\",\"device_operation_performed\":false,",
           ready ? "POLICY_READY" : "POLICY_NOT_DEVICE_VERIFIED", ready ? "true" : "false");
    policy_fields(stdout, &policy); puts("}");
    free(policy.text.data); return 0;
  }
  require(o.device && o.slot, "--boot-device and --active-slot are required for Android operations");
  struct snapshot s = snapshot(&o); const unsigned char *m = find_split(&s.bytes, &s.boot);
  uint64_t catalog_bytes = 0, records = 0; struct blob original = {0};
  if (m) original = restore(&s.bytes, &s.boot, m, &catalog_bytes, &records);
  else require(s.boot.avb.hash_matches, "stock BOOT data hash mismatch");
  require(!o.reject_wrapped || !m, "active BOOT is already wrapped");
  if (!strcmp(o.action, "repack")) {
    require(!m && o.execute && o.source && o.state_output && o.payload && !o.output && !o.state, "repack needs unwrapped source metadata, payloads, state output and --execute");
    struct json source = {0}; read_json(o.source, &source); check_identity(&source, &s); string_matches(&source, 0, "source_sha256", s.sha);
    struct blob sel = payload(&policy, o.payload, "selector.bin", 65536), shim = payload(&policy, o.payload, "shim.bin", 1024 * 1024);
    struct blob fd = payload(&policy, o.payload, "fd.bin", 0x300000), app = payload(&policy, o.payload, "app.bin", 64 * 1024 * 1024);
    int selector_meta = member(&policy, 0, "selector");
    struct blob result = repack(&s.bytes, &s.boot, &sel, json_number(&policy, selector_meta, "memory_bytes"),
                               json_number(&policy, selector_meta, "metadata_offset"), &shim, &fd, &app);
    struct boot check = parse_boot(&result, 1); const unsigned char *rm = find_split(&result, &check);
    struct blob back = restore(&result, &check, rm, &catalog_bytes, &records);
    require(back.size == s.bytes.size && !memcmp(back.data, s.bytes.data, back.size), "internal full BOOT reconstruction differs"); free(back.data);
    struct blob state = installation_state(&s, &result, rm, "PREPARED_NOT_WRITTEN"); save(o.state_output, &state); sync_parent(o.state_output); free(state.data);
    char after[65]; hash_hex(result.data, result.size, after); write_guarded(&o, &s, result.data, result.size, 0, after);
    state = installation_state(&s, &result, rm, "WRITTEN_READBACK_VERIFIED"); finish_state(o.state_output, &state); emit_result(&state, NULL);
    free(state.data); free(result.data); free(sel.data); free(shim.data); free(fd.data); free(app.data); free(source.text.data);
  } else if (!strcmp(o.action, "restore") || !strcmp(o.action, "request")) {
    require(m && o.state && !o.output && !o.source && !o.payload && !o.state_output, "restore/request needs the current installed wrapper state");
    struct json state = {0}; read_json(o.state, &state); check_owned(&state, &s, m);
    if (!strcmp(o.action, "restore")) {
      require(o.execute && !o.target && original.size == s.bytes.size, "restore requires --execute and exact active BOOT size");
      char after[65]; hash_hex(original.data, original.size, after);
      write_guarded(&o, &s, original.data, original.size, 0, after);
      printf("{\"status\":\"RESTORED_READBACK_VERIFIED\",\"source_sha256\":\"%s\",\"active_slot\":\"%s\",\"boot_bytes\":%zu}\n", after, s.id.slot, original.size);
    } else {
      require(o.execute || o.preview, "request requires --preview or explicit --execute");
      unsigned target = target_number(o.target); struct request_state before = read_requests(&s.bytes, m);
      if (o.execute) {
        unsigned page = update_request(&s.bytes, m, target); char after[65]; hash_hex(s.bytes.data, s.bytes.size, after);
        uint64_t offset = request_offset(m) + page * PAGE;
        write_guarded(&o, &s, s.bytes.data + offset, PAGE, offset, after);
      }
      struct request_state r = read_requests(&s.bytes, m);
      printf("{\"status\":\"%s\",\"stored\":%s,\"target\":%u,\"sequence\":%" PRIu64 ",\"previous_target\":%u,"
             "\"requested_target\":%u,\"persistent\":true,\"crc_valid\":true,\"active_slot\":\"%s\"}\n",
             o.execute ? "REQUEST_READBACK_VERIFIED" : "REQUEST_PREVIEW", o.execute ? "true" : "false", r.target[r.newest],
             r.sequence[r.newest], before.target[before.newest], target, s.id.slot);
    }
    free(state.text.data);
  } else {
    require(!o.execute && !o.target && !o.source && !o.payload && !o.state_output, "probe/status is read-only");
    int owned = 0; struct request_state r = {0};
    if (o.state) { require(m != NULL, "installed wrapper is no longer present"); struct json state = {0}; read_json(o.state, &state); check_owned(&state, &s, m); free(state.text.data); owned = 1; }
    if (m) r = read_requests(&s.bytes, m);
    char immutable[65] = "", original_sha[65], app_sha[65] = "";
    strcpy(original_sha, s.sha);
    if (m) { wrapper_hash(&s.bytes, m, immutable); hex(catalog(&s.bytes, m) + 96, original_sha); hex(m + 96, app_sha); }
    struct blob result = {0}; FILE *f = open_memstream((char **)&result.data, &result.size); require(f != NULL, "metadata allocation failed");
    fprintf(f, "{\"schema_version\":1,\"status\":\"%s\",", !strcmp(o.action, "probe") ? "ACTIVE_BOOT_PROBED" : "ACTIVE_BOOT_STATUS"); identity_fields(f, &s);
    fprintf(f, ",\"source_sha256\":\"%s\",\"original_source_sha256\":\"%s\",\"wrapper_sha256\":\"%s\",\"app_sha256\":\"%s\","
            "\"wrapped\":%s,\"owned\":%s,\"avb_hash_matches\":%s,\"module_ready\":%s,"
            "\"restore_catalog_bytes\":%" PRIu64 ",\"request_target\":%u,\"request_sequence\":%" PRIu64 ","
            "\"request_policy\":\"persistent-until-changed\",\"read_only\":true,\"full_partition_backup\":false,",
            s.sha, original_sha, immutable, app_sha, m ? "true" : "false", owned ? "true" : "false",
            s.boot.avb.hash_matches ? "true" : "false", ready ? "true" : "false",
            catalog_bytes, m ? r.target[r.newest] : 0, m ? r.sequence[r.newest] : 0);
    policy_fields(f, &policy); fputs("}\n", f);
    require(!fclose(f), "metadata formatting failed"); emit_result(&result, o.output); free(result.data);
  }
  close(s.fd); free(s.bytes.data); free(original.data); free(policy.text.data); return 0;
}

int main(int argc, char **argv) {
  require(!atexit(reset_ro), "cannot register BOOT readonly cleanup");
  int online = argc >= 2 && !strcmp(argv[1], "request");
  for (int i = 2; i < argc; i++)
    if (!strcmp(argv[i], "--boot-device") || !strcmp(argv[i], "--policy")) online = 1;
  if (online) return online_main(argc, argv);
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("piano-boot-repack status\n"
         "piano-boot-repack probe --input BOOT.img\n"
         "piano-boot-repack repack --input STOCK.img --output NEW.img --selector FILE\n"
         "  --selector-memory-bytes N --selector-metadata-offset N --shim FILE --fd FILE --app FILE\n"
         "piano-boot-repack restore --input WRAPPED.img --output RESTORED.img\n"
         "piano-boot-repack request --input WRAPPED.img --output NEW.img --target android|uefi|linux|setup\n"
         "Android: probe/status --boot-device /dev/block/by-name/boot_a|boot_b --active-slot _a|_b --policy FILE\n"
         "repack/restore/request online writes require --execute and current-ROM metadata/state.\n"
         "No inactive-slot, Recovery, vbmeta, GPT, OTA automation or automatic reboot.");
    return 0;
  }
  require(argc >= 2, "use --help for syntax");
  const char *action = argv[1], *input = NULL, *output = NULL, *selector_path = NULL;
  const char *shim_path = NULL, *fd_path = NULL, *app_path = NULL;
  uint64_t memory = 0, metadata = UINT64_MAX;
  for (int i = 2; i < argc; i++) {
    require(i + 1 < argc, "option needs a value");
    const char *name = argv[i], *value = argv[++i];
    if (!strcmp(name, "--input") && !input) input = value;
    else if (!strcmp(name, "--output") && !output) output = value;
    else if (!strcmp(name, "--selector") && !selector_path) selector_path = value;
    else if (!strcmp(name, "--selector-memory-bytes") && !memory) memory = number(value);
    else if (!strcmp(name, "--selector-metadata-offset") && metadata == UINT64_MAX) metadata = number(value);
    else if ((!strcmp(name, "--shim") || !strcmp(name, "--bootshim")) && !shim_path) shim_path = value;
    else if (!strcmp(name, "--fd") && !fd_path) fd_path = value;
    else if (!strcmp(name, "--app") && !app_path) app_path = value;
    else fail("unknown/duplicate file option");
  }
  if (!strcmp(action, "status")) {
    require(argc == 2, "status policy mode requires --policy");
    puts("{\"status\":\"ONLINE_INTERFACE_NOT_DEVICE_VERIFIED\",\"wrapper\":\"SPLITv1+RSTRv1\","
         "\"commands\":[\"status\",\"probe\",\"repack\",\"restore\",\"request\"],\"device_execution_ready\":false,"
         "\"online_interface_implemented\":true,\"request_policy\":\"persistent-until-changed\","
         "\"ota_automatic\":false,\"full_partition_backup\":false}"); return 0;
  }
  require(!strcmp(action, "probe") || !strcmp(action, "repack") || !strcmp(action, "restore"), "unknown command");
  struct blob b = load(input, BOOT_BYTES);
  struct boot s = parse_boot(&b, !strcmp(action, "repack"));
  const unsigned char *m = find_split(&b, &s);
  char source_sha[65], result_sha[65]; hash_hex(b.data, b.size, source_sha);
  uint64_t catalog_bytes = 0, records = 0;
  if (!strcmp(action, "probe")) {
    require(!output && !selector_path && !shim_path && !fd_path && !app_path, "probe does not accept output/payload options");
    if (m) { struct blob reconstructed = restore(&b, &s, m, &catalog_bytes, &records); free(reconstructed.data); }
    else require(s.avb.hash_matches, "stock BOOT hash descriptor mismatch");
    printf("{\"status\":\"FILE_PROBED\",\"wrapped\":%s,\"boot_bytes\":%zu,\"kernel_bytes\":%u,"
           "\"image_span\":%" PRIu64 ",\"source_sha256\":\"%s\",\"avb_hash_matches\":%s,"
           "\"restore_catalog_bytes\":%" PRIu64 ",\"padding_records\":%" PRIu64 ",\"device_operation_performed\":false}\n",
           m ? "true" : "false", b.size, s.kernel_bytes, s.span, source_sha,
           s.avb.hash_matches ? "true" : "false", catalog_bytes, records);
  } else if (!strcmp(action, "repack")) {
    require(!m, "input is already wrapped; restore first");
    struct blob selector = load(selector_path, 65536), shim = load(shim_path, 1024 * 1024);
    struct blob fd = load(fd_path, 0x300000), app = load(app_path, 64 * 1024 * 1024);
    struct blob result = repack(&b, &s, &selector, memory, metadata, &shim, &fd, &app);
    struct boot check = parse_boot(&result, 1);
    struct blob back = restore(&result, &check, find_split(&result, &check), &catalog_bytes, &records);
    require(back.size == b.size && !memcmp(back.data, b.data, b.size), "internal lossless reconstruction check failed");
    free(back.data); save(output, &result); hash_hex(result.data, result.size, result_sha);
    printf("{\"status\":\"FILE_REPACKED_NOT_DEVICE_VERIFIED\",\"source_sha256\":\"%s\",\"output_sha256\":\"%s\","
           "\"boot_bytes\":%zu,\"restore_catalog_bytes\":%" PRIu64 ",\"padding_records\":%" PRIu64 ","
           "\"full_partition_backup\":false,\"device_operation_performed\":false}\n", source_sha, result_sha, result.size, catalog_bytes, records);
    free(result.data); free(selector.data); free(shim.data); free(fd.data); free(app.data);
  } else {
    require(!selector_path && !shim_path && !fd_path && !app_path, "restore does not accept new payloads");
    struct blob result = restore(&b, &s, m, &catalog_bytes, &records);
    save(output, &result); hash_hex(result.data, result.size, result_sha);
    printf("{\"status\":\"FILE_RESTORED_BYTE_EQUAL\",\"output_sha256\":\"%s\",\"boot_bytes\":%zu,"
           "\"restore_catalog_bytes\":%" PRIu64 ",\"full_partition_backup\":false,\"device_operation_performed\":false}\n",
           result_sha, result.size, catalog_bytes); free(result.data);
  }
  free(b.data); return 0;
}
