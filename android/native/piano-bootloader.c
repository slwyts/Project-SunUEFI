// SPDX-License-Identifier: BSD-2-Clause-Patent
#define _GNU_SOURCE
#include "piano-bootloader.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define INPUTS 32
#define WORD_BITS (8 * sizeof(unsigned long))
#define BIT_WORDS ((KEY_MAX + WORD_BITS) / WORD_BITS)
static int bit(const unsigned long *bits, unsigned code) {
  return (bits[code / WORD_BITS] >> (code % WORD_BITS)) & 1;
}

int piano_bootloader_write_allowed(const char *flash_locked) {
  const char *confirmed = getenv("PIANO_CONFIRMED_UNVERIFIED_BOOTLOADER");
  return !strcmp(flash_locked, "0") || (confirmed && !strcmp(confirmed, "1"));
}

int piano_bootloader_key(struct piano_bootloader_keys *keys, unsigned code,
                         int value) {
  if (code == KEY_VOLUMEUP) {
    if (value == 0) keys->up = 0;
    else if (keys->up >= 0 && (value == 1 || value == 2)) return -1;
    return 0;
  }
  if (code != KEY_VOLUMEDOWN) return 0;
  if (value == 1 && keys->down >= 0) keys->down = 1;
  else if (value == 0 && keys->down > 0) {
    keys->down = 0;
    return ++keys->count == 3 ? 1 : 0;
  } else if (value == 0) keys->down = 0;
  return 0;
}

static long long now_ms(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
  return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int confirm_keys(const char *warning, const char *success) {
  fprintf(stderr, "%s\n松开音量键，连续按下并松开三次音量下继续；按音量上退出。30 秒未完成则退出。\n", warning);
  struct pollfd fds[INPUTS];
  struct piano_bootloader_keys keys[INPUTS] = {0};
  unsigned total = 0;
  int has_up = 0, has_down = 0;
  const char *failure = "音量键读取中断，已退出，尚未写入。";
  nfds_t count = 0;
  DIR *dir = opendir("/dev/input");
  if (!dir) {
    fprintf(stderr, "无法读取实体音量键，已退出，尚未写入。\n");
    return 1;
  }
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (strncmp(entry->d_name, "event", 5) || count == INPUTS) continue;
    int fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    char name[80] = {0}, sysfs[512], device[4096];
    snprintf(sysfs, sizeof(sysfs), "/sys/class/input/%s/device", entry->d_name);
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 ||
        (strcmp(name, "gpio-keys") && strcmp(name, "pmic_resin")) ||
        !realpath(sysfs, device) || strncmp(device, "/sys/devices/platform/", 22)) {
      close(fd);
      continue;
    }
    unsigned long supported[BIT_WORDS] = {0}, held[BIT_WORDS] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(supported)), supported) < 0 ||
        (!bit(supported, KEY_VOLUMEDOWN) && !bit(supported, KEY_VOLUMEUP)) ||
        ioctl(fd, EVIOCGKEY(sizeof(held)), held) < 0) {
      close(fd);
      continue;
    }
    if (ioctl(fd, EVIOCGRAB, 1) < 0) { close(fd); continue; }
    has_up |= bit(supported, KEY_VOLUMEUP);
    has_down |= bit(supported, KEY_VOLUMEDOWN);
    /* A key held before the warning is not a confirmation press. */
    keys[count].down = bit(held, KEY_VOLUMEDOWN) ? -1 : 0;
    keys[count].up = bit(held, KEY_VOLUMEUP) ? -1 : 0;
    fds[count++] = (struct pollfd){.fd = fd, .events = POLLIN};
  }
  closedir(dir);
  long long start = now_ms();
  int result = 1;
  if (!has_up || !has_down) {
    failure = "无法同时读取两个实体音量键，已退出，尚未写入。";
    goto done;
  }
  while (count && start >= 0) {
    long long now = now_ms();
    if (now < 0) break;
    if (now - start >= 30000) { failure = "30 秒未完成确认，已退出，尚未写入。"; break; }
    int ready = poll(fds, count, (int)(30000 - (now - start)));
    if (ready < 0 && errno == EINTR) continue;
    if (!ready) { failure = "30 秒未完成确认，已退出，尚未写入。"; break; }
    if (ready < 0) break;
    for (nfds_t i = 0; i < count; ++i) {
      if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) goto done;
      if (!(fds[i].revents & POLLIN)) continue;
      struct input_event event;
      ssize_t bytes;
      unsigned events = 0;
      while ((bytes = read(fds[i].fd, &event, sizeof(event))) == sizeof(event)) {
        if (++events > 256) goto done;
        if (event.type == EV_SYN && event.code == SYN_DROPPED) goto done;
        if (event.type != EV_KEY) continue;
        unsigned before = keys[i].count;
        int action = piano_bootloader_key(&keys[i], event.code, event.value);
        if (action < 0) { failure = "已按音量上取消，尚未写入。"; goto done; }
        if (keys[i].count != before) {
          ++total;
          fprintf(stderr, "音量下确认：%u/3\n", total);
        }
      }
      if (bytes != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) goto done;
    }
    /* Read all ready devices first so Volume Up takes priority over the third
     * release when both keys arrive in the same poll cycle. */
    if (total >= 3) {
      now = now_ms();
      if (now >= 0 && now - start < 30000) result = 0;
      goto done;
    }
  }
done:
  for (nfds_t i = 0; i < count; ++i) close(fds[i].fd);
  fprintf(stderr, "%s\n", result ? failure : success);
  return result;
}

int piano_bootloader_confirm(void) {
  return confirm_keys(
      "无法确认 Bootloader 已解锁，可能启用了 BL 状态伪装模块。\n"
      "如果只有临时 Root、实际没有解锁 BL，写入引导可能导致设备无法启动（变砖）。\n"
      "请确认实际已解锁，并愿意承担继续安装的风险。",
      "已确认继续；仅本次操作跳过 BL 状态检查，其余写入校验保持不变。");
}

int piano_boot_source_confirm(void) {
  return confirm_keys(
      "当前 BOOT 缺少有效的 AVB 数据摘要，可能由自定义 GKI 或 Root 工具修改。\n"
      "已检查内核入口，并确认能完整还原当前 BOOT；这不验证内核来源或启动兼容性。\n"
      "确认这是目前正常启动 Android 所用的 BOOT 后再继续。",
      "已确认保留当前内核继续安装；仅本次操作接受原 BOOT 的 AVB 摘要缺失或不一致。");
}
