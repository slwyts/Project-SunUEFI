/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Read KMS properties/blobs through the public UAPI. No modeset or MMIO. */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_ITEMS 4096U
#define MAX_BLOB (16U * 1024U * 1024U)
static unsigned errors;
static int dump_blobs;

static int query(int fd, unsigned long request, void *data) {
  int result;
  do {
    result = ioctl(fd, request, data);
  } while (result < 0 && errno == EINTR);
  return result;
}

static uint64_t pointer(void *data) { return (uint64_t)(uintptr_t)data; }

static void string(const char *text, size_t limit) {
  putchar('"');
  for (size_t i = 0; i < limit && text[i]; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == '"' || c == '\\')
      printf("\\%c", c);
    else if (c < 32 || c >= 127)
      printf("\\u%04x", c);
    else
      putchar(c);
  }
  putchar('"');
}

static void error(void) {
  int saved = errno;
  ++errors;
  printf("\"errno\":%d", saved);
}

static void blob(int fd, uint32_t id) {
  struct drm_mode_get_blob b = {.blob_id = id};
  if (query(fd, DRM_IOCTL_MODE_GETPROPBLOB, &b) < 0) {
    error();
    return;
  }
  printf("\"bytes\":%u", b.length);
  if (!b.length || !dump_blobs)
    return;
  if (b.length > MAX_BLOB) {
    errno = E2BIG;
    putchar(',');
    error();
    return;
  }
  uint32_t available = b.length;
  unsigned char *data = calloc(available, 1);
  if (!data) {
    putchar(',');
    error();
    return;
  }
  b.data = pointer(data);
  if (query(fd, DRM_IOCTL_MODE_GETPROPBLOB, &b) < 0) {
    putchar(',');
    error();
  } else if (b.length > available) {
    errno = EAGAIN;
    putchar(',');
    error();
  } else {
    fputs(",\"hex\":\"", stdout);
    for (uint32_t i = 0; i < b.length; i++)
      printf("%02x", data[i]);
    putchar('"');
  }
  free(data);
}

static void property(int fd, uint32_t id, uint64_t value) {
  struct drm_mode_get_property p = {.prop_id = id};
  printf("{\"id\":%u,\"value\":%" PRIu64, id, value);
  if (query(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) < 0) {
    putchar(',');
    error();
    putchar('}');
    return;
  }
  fputs(",\"name\":", stdout);
  string(p.name, sizeof(p.name));
  printf(",\"flags\":%u", p.flags);
  if (p.flags & DRM_MODE_PROP_BLOB) {
    if (value > UINT32_MAX) {
      errno = EOVERFLOW;
      putchar(',');
      error();
      putchar('}');
      return;
    } else if (value) {
      fputs(",\"blob\":{", stdout);
      blob(fd, (uint32_t)value);
      putchar('}');
    }
  }
  uint32_t nv = p.count_values, ne = p.count_enum_blobs;
  if (nv > MAX_ITEMS || ne > MAX_ITEMS) {
    errno = E2BIG;
    putchar(',');
    error();
  } else {
    uint64_t *values = calloc(nv ? nv : 1, sizeof(*values));
    struct drm_mode_property_enum *enums = calloc(ne ? ne : 1, sizeof(*enums));
    if (!values || !enums) {
      errno = ENOMEM;
      putchar(',');
      error();
    } else {
      p.values_ptr = pointer(values);
      p.enum_blob_ptr = pointer(enums);
      if (query(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) < 0) {
        putchar(',');
        error();
      } else if (p.count_values > nv || p.count_enum_blobs > ne) {
        errno = EAGAIN;
        putchar(',');
        error();
      } else {
        fputs(",\"values\":[", stdout);
        for (uint32_t i = 0; i < p.count_values; i++)
          printf("%s%" PRIu64, i ? "," : "", (uint64_t)values[i]);
        fputs("],\"enums\":[", stdout);
        for (uint32_t i = 0; i < p.count_enum_blobs; i++) {
          printf("%s{\"value\":%" PRIu64 ",\"name\":", i ? "," : "",
                 (uint64_t)enums[i].value);
          string(enums[i].name, sizeof(enums[i].name));
          putchar('}');
        }
        putchar(']');
      }
    }
    free(values);
    free(enums);
  }
  putchar('}');
}

static void object(int fd, uint32_t id, uint32_t type, const char *kind) {
  struct drm_mode_obj_get_properties p = {.obj_id = id, .obj_type = type};
  printf("{\"id\":%u,\"kind\":", id);
  string(kind, 32);
  if (query(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p) < 0) {
    putchar(',');
    error();
    putchar('}');
    return;
  }
  uint32_t count = p.count_props;
  if (count > MAX_ITEMS) {
    errno = E2BIG;
    putchar(',');
    error();
    putchar('}');
    return;
  }
  uint32_t *ids = calloc(count ? count : 1, sizeof(*ids));
  uint64_t *values = calloc(count ? count : 1, sizeof(*values));
  if (!ids || !values) {
    errno = ENOMEM;
    putchar(',');
    error();
  } else {
    p.props_ptr = pointer(ids);
    p.prop_values_ptr = pointer(values);
    if (query(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p) < 0) {
      putchar(',');
      error();
    } else if (p.count_props > count) {
      errno = EAGAIN;
      putchar(',');
      error();
    } else {
      fputs(",\"properties\":[", stdout);
      for (uint32_t i = 0; i < p.count_props; i++) {
        if (i)
          putchar(',');
        property(fd, ids[i], values[i]);
      }
      putchar(']');
    }
  }
  free(ids);
  free(values);
  putchar('}');
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--blobs"))) {
    fprintf(stderr, "Usage: %s /dev/dri/cardN [--blobs]\n", argv[0]);
    return 2;
  }
  dump_blobs = argc == 3;
  int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    perror(argv[1]);
    return 1;
  }
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  struct drm_set_client_cap cap = {.capability = DRM_CLIENT_CAP_ATOMIC,
                                   .value = 1};
  int atomic_error = query(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0 ? errno : 0;
  int universal_error = 0;
  if (atomic_error) {
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    universal_error = query(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0 ? errno : 0;
    if (universal_error)
      ++errors;
  }
  printf("{\"schema\":1,\"device\":");
  string(argv[1], 4096);
  printf(",\"read_only\":true,\"globally_atomic_snapshot\":false,\"atomic_cap_"
         "errno\":%d,\"universal_cap_errno\":%d,\"universal_planes\":%s,"
         "\"start_ns\":%" PRIu64,
         atomic_error, universal_error, universal_error ? "false" : "true",
         (uint64_t)start.tv_sec * 1000000000 + (uint64_t)start.tv_nsec);
  struct drm_mode_card_res r = {0};
  fputs(",\"objects\":[", stdout);
  int first = 1;
  if (query(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0) {
    putchar('{');
    error();
    putchar('}');
    first = 0;
  } else if (r.count_crtcs > MAX_ITEMS || r.count_connectors > MAX_ITEMS) {
    errno = E2BIG;
    putchar('{');
    error();
    putchar('}');
    first = 0;
  } else {
    uint32_t nc = r.count_crtcs, nn = r.count_connectors;
    uint32_t *crtcs = calloc(nc ? nc : 1, sizeof(*crtcs));
    uint32_t *connectors = calloc(nn ? nn : 1, sizeof(*connectors));
    if (!crtcs || !connectors) {
      errno = ENOMEM;
      putchar('{');
      error();
      putchar('}');
      first = 0;
    } else {
      r.crtc_id_ptr = pointer(crtcs);
      r.connector_id_ptr = pointer(connectors);
      /* Do not offer capacity for the lists whose pointers remain zero. */
      r.count_fbs = 0;
      r.count_encoders = 0;
      if (query(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0) {
        putchar('{');
        error();
        putchar('}');
        first = 0;
      } else if (r.count_crtcs > nc || r.count_connectors > nn) {
        errno = EAGAIN;
        putchar('{');
        error();
        putchar('}');
        first = 0;
      } else {
        for (uint32_t i = 0; i < r.count_crtcs; i++) {
          if (!first)
            putchar(',');
          first = 0;
          object(fd, crtcs[i], DRM_MODE_OBJECT_CRTC, "crtc");
        }
        for (uint32_t i = 0; i < r.count_connectors; i++) {
          if (!first)
            putchar(',');
          first = 0;
          object(fd, connectors[i], DRM_MODE_OBJECT_CONNECTOR, "connector");
        }
      }
    }
    free(crtcs);
    free(connectors);
  }
  struct drm_mode_get_plane_res planes = {0};
  if (query(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes) < 0) {
    if (!first)
      putchar(',');
    putchar('{');
    error();
    putchar('}');
  } else if (planes.count_planes > MAX_ITEMS) {
    if (!first)
      putchar(',');
    errno = E2BIG;
    putchar('{');
    error();
    putchar('}');
  } else {
    uint32_t count = planes.count_planes;
    uint32_t *ids = calloc(count ? count : 1, sizeof(*ids));
    if (!ids) {
      if (!first)
        putchar(',');
      errno = ENOMEM;
      putchar('{');
      error();
      putchar('}');
    } else {
      planes.plane_id_ptr = pointer(ids);
      if (query(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes) < 0) {
        if (!first)
          putchar(',');
        putchar('{');
        error();
        putchar('}');
      } else if (planes.count_planes > count) {
        if (!first)
          putchar(',');
        errno = EAGAIN;
        putchar('{');
        error();
        putchar('}');
      } else
        for (uint32_t i = 0; i < planes.count_planes; i++) {
          if (!first)
            putchar(',');
          first = 0;
          object(fd, ids[i], DRM_MODE_OBJECT_PLANE, "plane");
        }
    }
    free(ids);
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  printf("],\"end_ns\":%" PRIu64 ",\"errors\":%u}\n",
         (uint64_t)end.tv_sec * 1000000000 + (uint64_t)end.tv_nsec, errors);
  close(fd);
  return errors || ferror(stdout) ? 1 : 0;
}
