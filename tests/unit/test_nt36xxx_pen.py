"""Exercise the kernel pen parser and integer maths, not hardware behaviour."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / 'patches/linux/7.3/0123-input-nt36532e-pen.patch'
SOURCE = Path('drivers/input/touchscreen/nt36532e')
CALIBRATION = ROOT / 'linux/bsp/common/usr/share/piano/pen/calibration'

HEADERS = {
    'types.h': '''#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
''',
    'ctype.h': '#include <ctype.h>\n',
    'err.h': '''#include <errno.h>
#include <stdint.h>
#define ERR_PTR(n) ((void *)(intptr_t)(n))
''',
    'firmware.h': '''#include <linux/types.h>
struct device { int unused; };
struct firmware { const u8 *data; size_t size; };
static inline int request_firmware_direct(const struct firmware **f,
    const char *name, struct device *dev)
{ (void)f; (void)name; (void)dev; return -ENOENT; }
static inline void release_firmware(const struct firmware *f) { (void)f; }
''',
    'kernel.h': '''#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define S32_MAX INT32_MAX
#define S32_MIN INT32_MIN
#define S16_MAX INT16_MAX
#define S16_MIN INT16_MIN
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define max_t(t,a,b) max((t)(a),(t)(b))
#define clamp(x,a,b) min(max((x),(a)),(b))
''',
    'math64.h': '''#include <linux/types.h>
static inline s64 div_s64(s64 a, s32 b) { return a / b; }
static inline s64 div64_s64(s64 a, s64 b) { return a / b; }
''',
    'module.h': '#define MODULE_FIRMWARE(name)\n',
    'sort.h': '''#include <stdlib.h>
static inline void sort(void *p, size_t n, size_t size,
    int (*compare)(const void *, const void *), void (*swap)(void *, void *, int))
{ (void)swap; qsort(p, n, size, compare); }
''',
    'slab.h': '''#include <stdlib.h>
#include <string.h>
#define GFP_KERNEL 0
#define kzalloc_obj(p) calloc(1, sizeof(p))
#define kfree(p) free(p)
static inline void *kmemdup_nul(const void *p, size_t n, int flags)
{ (void)flags; char *s = malloc(n + 1); if (s) { memcpy(s, p, n); s[n] = 0; } return s; }
''',
    'string.h': '#include <string.h>\n',
    'unaligned.h': '''#include <linux/types.h>
static inline u16 get_unaligned_le16(const void *p)
{ const u8 *b = p; return b[0] | (u16)b[1] << 8; }
static inline u32 get_unaligned_le32(const void *p)
{ const u8 *b = p; return b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24; }
''',
}

HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include "nt36xxx-pen.c"

static char *load(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long n = ftell(file);
    assert(n > 0 && n < 16384);
    rewind(file);
    char *text = calloc(1, n + 1);
    assert(text && fread(text, 1, n, file) == (size_t)n);
    assert(!fclose(file));
    return text;
}

static void maths(const struct nvt_pen_decoder *d)
{
    unsigned char plane[960] = {0};
    s32 axis[60], line[60], result;
    /* A single positive electrode has its known physical coordinate. */
    plane[(20 * 12 + 5) * 2] = 0xe8;
    plane[(20 * 12 + 5) * 2 + 1] = 3;
    assert(nvt_pen_centroid(plane, 40, 12, true, d->mapping_x,
                           d->tip_threshold, d->resolution, axis, line, &result));
    assert(result == d->mapping_x[20] * d->resolution);
    memset(plane, 0xff, sizeof(plane));
    assert(!nvt_pen_centroid(plane, 40, 12, true, d->mapping_x,
                            d->tip_threshold, d->resolution, axis, line, &result));
    /* Signed extremes and non-uniform real mappings remain within their span. */
    for (size_t i = 0; i < 480; ++i) {
        plane[i * 2] = i % 7 == 0 ? 255 : 0;
        plane[i * 2 + 1] = i % 7 == 0 ? 127 : 128;
    }
    if (nvt_pen_centroid(plane, 40, 12, true, d->mapping_x,
                        d->tip_threshold, d->resolution, axis, line, &result)) {
        assert(result >= d->mapping_x[0] * d->resolution);
        assert(result <= d->mapping_x[39] * d->resolution);
    }
    /* Three measured neighbours can exceed the lobe gate without any one
     * electrode reaching it. Do not turn this into a per-electrode gate. */
    memset(plane, 0, sizeof(plane));
    plane[(9 * 12 + 5) * 2] = 80;
    plane[(10 * 12 + 5) * 2] = 90;
    plane[(11 * 12 + 5) * 2] = 80;
    assert(nvt_pen_centroid(plane, 40, 12, true, d->mapping_x,
                           d->tip_threshold, d->resolution, axis, line, &result));
    assert(result == d->mapping_x[10] * d->resolution);
    /* Each scan group's independent DC offset must not move the centroid. */
    for (unsigned node = 0; node < 40; ++node) {
        for (unsigned group = 0; group < 12; ++group) {
            unsigned index = (node * 12 + group) * 2;
            int value = (int16_t)get_unaligned_le16(plane + index);
            value += ((int)group - 4) * 23;
            plane[index] = value;
            plane[index + 1] = (unsigned)value >> 8;
        }
    }
    assert(nvt_pen_centroid(plane, 40, 12, true, d->mapping_x,
                           d->tip_threshold, d->resolution, axis, line, &result));
    assert(result == d->mapping_x[10] * d->resolution);
    /* A weaker competing scan group must not splice its peak into the
     * selected group's centroid. */
    memset(plane, 0, sizeof(plane));
    const int amplitudes[] = {100, 400, 1000, 400, 100};
    for (unsigned i = 0; i < 5; ++i) {
        unsigned index = ((8 + i) * 12 + 5) * 2;
        plane[index] = amplitudes[i];
        plane[index + 1] = amplitudes[i] >> 8;
    }
    plane[(11 * 12 + 6) * 2] = 900 & 255;
    plane[(11 * 12 + 6) * 2 + 1] = 900 >> 8;
    assert(nvt_pen_centroid(plane, 40, 12, true, d->mapping_x,
                           d->tip_threshold, d->resolution, axis, line, &result));
    assert(result == d->mapping_x[10] * d->resolution);
    for (unsigned profile = 0; profile < 2; ++profile) {
        for (unsigned i = 0; i < 6; ++i) {
            s32 diff = d->profiles[profile].difference[i] * d->resolution;
            s32 expected = min(d->angles[i] / 100, 60);
            assert(nvt_pen_tilt(d, &d->profiles[profile], diff) == expected);
            assert(nvt_pen_tilt(d, &d->profiles[profile], -diff) == -expected);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    for (int i = 1; i < argc; ++i) {
        char *config = load(argv[i]);
        struct nvt_pen_decoder decoder = {0};
        assert(!nvt_pen_read_config(config, &decoder));
        maths(&decoder);
        /* Duplicate keys, truncation, overflow and incorrect units are errors. */
        size_t n = strlen(config);
        char *duplicate = malloc(n + 64);
        assert(duplicate);
        sprintf(duplicate, "%s\ncalibration\nformat_version=1\n", config);
        assert(nvt_pen_read_config(duplicate, &decoder) < 0);
        free(duplicate);
        for (size_t cut = 0; cut < n; cut += 17) {
            char saved = config[cut];
            config[cut] = 0;
            (void)nvt_pen_read_config(config, &decoder);
            config[cut] = saved;
        }
        s32 value;
        assert(nvt_pen_config_value("x\nv=999999999999999999999999999999\n",
                                    "x", "v", &value, 1) == -ERANGE);
        free(config);
        /* These invalid byte strings test bounds, not invented pen captures. */
        unsigned char bytes[7934] = {0};
        struct nvt_pen_point point;
        for (size_t n = 0; n <= sizeof(bytes); ++n)
            assert(nvt_pen_decode(&decoder, bytes, n, 3, &point) < 0);
        assert(!nvt_pen_checksum(bytes, sizeof(bytes), SIZE_MAX, 0, sizeof(bytes)));
    }
    puts("PASS: real BOE/CSOT calibration, parser bounds, integer centroid and tilt curve");
    return 0;
}
'''


@unittest.skipUnless(shutil.which('cc') and shutil.which('git'), 'C compiler and Git required')
class Nt36xxxPenTests(unittest.TestCase):
    def test_real_calibration_and_integer_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='nt36xxx-pen-') as directory:
            work = Path(directory)
            for name, text in HEADERS.items():
                path = work / 'include/linux' / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            subprocess.run(['git', 'apply', '-p1',
                            '--include=' + str(SOURCE / 'nt36xxx-pen.c'),
                            '--include=' + str(SOURCE / 'nt36xxx-pen.h'),
                            str(PATCH)], cwd=work, check=True, capture_output=True)
            harness = work / 'test.c'
            harness.write_text(HARNESS)
            executable = work / 'test'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                            '-fsanitize=undefined,address', '-fno-omit-frame-pointer',
                            '-I' + str(work / 'include'), '-I' + str(work / SOURCE),
                            str(harness), '-o', str(executable)], check=True, capture_output=True)
            subprocess.run([str(executable), str(CALIBRATION / 'piano-boe.ini'),
                            str(CALIBRATION / 'piano-csot.ini')], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
