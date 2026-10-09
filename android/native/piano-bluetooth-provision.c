// SPDX-License-Identifier: BSD-2-Clause-Patent
// The public controller address belongs to the individual tablet. Keep the
// release DTB generic and derive only local-bd-address when installing it.
#define _GNU_SOURCE
#include "piano-bluetooth-provision.h"
#include <errno.h>
#include <fcntl.h>
#include <libfdt.h>
#include <sha1.h>
#include <sha2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE 4096u
#define BOOT_MAX (1024u * 1024u * 1024u)
#define DTB_MAX (2u * 1024u * 1024u)

static void require(int ok, const char *message) {
  if (!ok) {
    fprintf(stderr, "piano-storage: %s\n", message);
    exit(1);
  }
}
static uint32_t le32(const unsigned char *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static void put32(unsigned char *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = n >> (i * 8);
}
static size_t aligned(size_t n) { return (n + PAGE - 1) & ~(size_t)(PAGE - 1); }
static int zeros(const unsigned char *p, size_t n) {
  while (n--) if (*p++) return 0;
  return 1;
}
static unsigned char *read_file(const char *name, size_t min, size_t max,
                                size_t *size, struct stat *st) {
  int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  require(fd >= 0 && !fstat(fd, st) && S_ISREG(st->st_mode) &&
          st->st_size >= (off_t)min && st->st_size <= (off_t)max,
          "设备专属蓝牙参数或 Linux 启动文件无效。" );
  *size = (size_t)st->st_size;
  unsigned char *data = malloc(*size);
  require(data != NULL, "无法分配蓝牙安装缓冲区。" );
  size_t done = 0;
  while (done < *size) {
    ssize_t n = read(fd, data + done, *size - done);
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "读取蓝牙安装文件失败。" );
    done += (size_t)n;
  }
  unsigned char extra;
  require(read(fd, &extra, 1) == 0, "蓝牙安装文件在读取期间发生变化。" );
  close(fd);
  return data;
}
static void address(const char *factory, unsigned char little[6]) {
  struct stat st; size_t n;
  unsigned char *raw = read_file(factory, 6, 6, &n, &st);
  require(!(raw[0] & 3) && !zeros(raw, 6) && memcmp(raw, "\xff\xff\xff\xff\xff\xff", 6),
          "原厂蓝牙地址无效，不能使用随机地址替代。" );
  for (unsigned i = 0; i < 6; ++i) little[i] = raw[5 - i];
  require(memcmp(little, "\x00\xbd\x00\x00\xbd\x00", 6) &&
          memcmp(little, "\x11\x22\x33\x44\x55\x66", 6),
          "原厂文件包含蓝牙默认地址。" );
  free(raw);
}
void piano_check_bluetooth_address(const char *factory) {
  unsigned char value[6]; address(factory, value);
  puts("{\"factory_address_available\":true,\"random_address\":false}");
}
static void boot_id(const unsigned char *kernel, uint32_t kn,
                    const unsigned char *ramdisk, uint32_t rn,
                    const unsigned char *dtb, uint32_t dn, unsigned char id[32]) {
  SHA1_CTX ctx; unsigned char count[4];
  SHA1Init(&ctx);
  const unsigned char *parts[] = {kernel, ramdisk, NULL, NULL, dtb};
  const uint32_t lengths[] = {kn, rn, 0, 0, dn};
  for (unsigned i = 0; i < 5; ++i) {
    if (lengths[i]) SHA1Update(&ctx, parts[i], lengths[i]);
    put32(count, lengths[i]); SHA1Update(&ctx, count, sizeof(count));
  }
  memset(id, 0, 32); SHA1Final(id, &ctx);
}
static void replace_file(const char *name, const unsigned char *before,
                          size_t bn, const unsigned char *after, size_t an,
                          const struct stat *original) {
  char *pending = NULL;
  require(asprintf(&pending, "%s.bluetooth-new.XXXXXX", name) > 0,
          "无法准备蓝牙安装路径。" );
  int fd = mkstemp(pending);
  require(fd >= 0, "无法准备蓝牙安装文件。" );
  size_t done = 0;
  while (done < an) {
    ssize_t n = write(fd, after + done, an - done);
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "写入蓝牙安装文件失败。" );
    done += (size_t)n;
  }
  require(!fsync(fd), "同步蓝牙安装文件失败。" );
  close(fd);
  struct stat st; size_t n;
  unsigned char *readback = read_file(pending, an, an, &n, &st);
  require(!memcmp(readback, after, an), "蓝牙启动文件读回不一致。" );
  free(readback);
  unsigned char *current = read_file(name, bn, bn, &n, &st);
  require(st.st_dev == original->st_dev && st.st_ino == original->st_ino &&
          !memcmp(current, before, bn), "Linux 启动文件已变化，取消替换。" );
  free(current);
  require(!rename(pending, name), "替换蓝牙启动文件失败。" );
  free(pending);
  char *dir = strdup(name); require(dir != NULL, "无法同步 ESP 目录。" );
  char *slash = strrchr(dir, '/');
  if (slash) { if (slash == dir) slash[1] = 0; else *slash = 0; }
  else strcpy(dir, ".");
  fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(fd >= 0 && !fsync(fd), "同步 ESP 目录失败。" );
  close(fd); free(dir);
}
void piano_provision_bluetooth(const char *boot, const char *factory) {
  unsigned char bdaddr[6]; address(factory, bdaddr);
  struct stat st; size_t n;
  unsigned char *data = read_file(boot, PAGE, BOOT_MAX, &n, &st);
  require(!memcmp(data, "ANDROID!", 8) && le32(data + 36) == PAGE &&
          le32(data + 40) == 2 && le32(data + 1644) == 1660 &&
          zeros(data + 1632, 12) && le32(data + 24) == 0,
          "设备地址安装只支持标准 Linux BOOTv2 启动文件。" );
  uint32_t kn = le32(data + 8), rn = le32(data + 16), dn = le32(data + 1648);
  size_t ro = PAGE + aligned(kn), dto = ro + aligned(rn);
  require(kn >= 64 && rn > 0 && dn >= sizeof(struct fdt_header) && dn < DTB_MAX - 65536 &&
          kn < BOOT_MAX && rn < BOOT_MAX && ro < n && dto < n &&
          dto + aligned(dn) == n && !memcmp(data + PAGE + 56, "ARMd", 4) &&
          !(le32(data + PAGE + 24) & 1) &&
          zeros(data + PAGE + kn, aligned(kn) - kn) &&
          zeros(data + ro + rn, aligned(rn) - rn) &&
          zeros(data + dto + dn, aligned(dn) - dn),
          "Linux 启动文件布局或填充无效。" );
  unsigned char id[32];
  boot_id(data + PAGE, kn, data + ro, rn, data + dto, dn, id);
  require(!memcmp(data + 576, id, 32), "Linux BOOTv2 校验值无效。" );
  require(!fdt_check_full(data + dto, dn) && fdt_totalsize(data + dto) == dn &&
          !fdt_node_check_compatible(data + dto, 0, "xiaomi,piano"),
          "设备树不属于 Piano。" );
  int node = -1, match = -1, depth = 0;
  while ((node = fdt_next_node(data + dto, node, &depth)) >= 0) {
    if (!fdt_node_check_compatible(data + dto, node, "qcom,wcn7850-bt") ||
        !fdt_node_check_compatible(data + dto, node, "qcom,wcn7861-bt")) {
      require(match < 0, "设备树包含多个蓝牙控制器。" ); match = node;
    }
  }
  require(match >= 0 && !fdt_getprop(data + dto, match, "qcom,local-bd-address-broken", NULL),
          "设备树缺少标准 QCA 蓝牙控制器。" );
  int len = 0;
  const void *old = fdt_getprop(data + dto, match, "local-bd-address", &len);
  if (old && len == 6 && !memcmp(old, bdaddr, 6)) {
    puts("{\"bluetooth_provisioned\":true,\"changed\":false,\"kernel_initramfs_unchanged\":true}");
    free(data); return;
  }
  void *dtb = calloc(1, dn + 65536);
  require(dtb && !fdt_open_into(data + dto, dtb, dn + 65536) &&
          !fdt_setprop(dtb, match, "local-bd-address", bdaddr, 6) && !fdt_pack(dtb),
          "更新标准蓝牙设备树属性失败。" );
  dn = fdt_totalsize(dtb);
  size_t result_n = dto + aligned(dn);
  unsigned char *result = calloc(1, result_n);
  require(result != NULL, "无法分配 Linux 启动文件。" );
  memcpy(result, data, dto); memcpy(result + dto, dtb, dn);
  put32(result + 1648, dn);
  boot_id(result + PAGE, kn, result + ro, rn, result + dto, dn, result + 576);
  require(!fdt_check_full(result + dto, dn), "更新后的设备树无效。" );
  unsigned char current_addr[6]; address(factory, current_addr);
  require(!memcmp(current_addr, bdaddr, 6), "安装期间原厂蓝牙地址发生变化。" );
  replace_file(boot, data, n, result, result_n, &st);
  unsigned char source_hash[32], derived_hash[32];
  char source_hex[65], derived_hex[65]; SHA2_CTX digest;
  SHA256Init(&digest); SHA256Update(&digest, data, n); SHA256Final(source_hash, &digest);
  SHA256Init(&digest); SHA256Update(&digest, result, result_n); SHA256Final(derived_hash, &digest);
  for (unsigned i = 0; i < 32; ++i) {
    sprintf(source_hex + i * 2, "%02x", source_hash[i]);
    sprintf(derived_hex + i * 2, "%02x", derived_hash[i]);
  }
  printf("{\"bluetooth_provisioned\":true,\"changed\":true,\"kernel_initramfs_unchanged\":true,"
         "\"random_address\":false,\"source_boot_sha256\":\"%s\",\"derived_boot_sha256\":\"%s\"}\n",
         source_hex, derived_hex);
  free(result); free(dtb); free(data);
}
