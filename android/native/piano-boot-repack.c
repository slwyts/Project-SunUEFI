// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Regular-file SPLITv1 repacking only. No block-device or online write mode.
 * BOOT4 and AVB fields follow the AOSP bootimg/libavb wire structures.
 * Recovery data follows APP; the original GKI is kept once, in place.
 */
#define _GNU_SOURCE
#include "BootRequest.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sha2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
        require(value >= 13 && !memcmp(d + 33 + key, "Xiaomi/piano/", 13), "BOOT is not the supported Piano ROM family");
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
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("piano-boot-repack status\n"
         "piano-boot-repack probe --input BOOT.img\n"
         "piano-boot-repack repack --input STOCK.img --output NEW.img --selector FILE\n"
         "  --selector-memory-bytes N --selector-metadata-offset N --shim FILE --fd FILE --app FILE\n"
         "piano-boot-repack restore --input WRAPPED.img --output RESTORED.img\n"
         "Regular files only. No --execute, block-device writes, OTA automation or module-ready mode.");
    return 0;
  }
  require(argc >= 2, "use --help for syntax");
  const char *action = argv[1], *input = NULL, *output = NULL, *selector_path = NULL;
  const char *shim_path = NULL, *fd_path = NULL, *app_path = NULL;
  uint64_t memory = 0, metadata = UINT64_MAX;
  for (int i = 2; i < argc; i++) {
    require(i + 1 < argc, "option needs a value; online/device modes are not implemented");
    const char *name = argv[i], *value = argv[++i];
    if (!strcmp(name, "--input") && !input) input = value;
    else if (!strcmp(name, "--output") && !output) output = value;
    else if (!strcmp(name, "--selector") && !selector_path) selector_path = value;
    else if (!strcmp(name, "--selector-memory-bytes") && !memory) memory = number(value);
    else if (!strcmp(name, "--selector-metadata-offset") && metadata == UINT64_MAX) metadata = number(value);
    else if ((!strcmp(name, "--shim") || !strcmp(name, "--bootshim")) && !shim_path) shim_path = value;
    else if (!strcmp(name, "--fd") && !fd_path) fd_path = value;
    else if (!strcmp(name, "--app") && !app_path) app_path = value;
    else fail("unknown/duplicate option; module execution is not implemented");
  }
  if (!strcmp(action, "status")) {
    require(argc == 2, "module policy/readiness interface is not implemented");
    puts("{\"status\":\"FILE_MODE_ONLY_NOT_DEVICE_VERIFIED\",\"wrapper\":\"SPLITv1+RSTRv1\","
         "\"commands\":[\"status\",\"probe\",\"repack\",\"restore\"],\"device_execution_ready\":false,"
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
