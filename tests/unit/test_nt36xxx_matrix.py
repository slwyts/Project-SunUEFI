"""Compile and exercise the kernel's real integer THP decoder under sanitizers."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / 'patches/linux/7.3/0095-input-nt36532e-matrix.patch'
SOURCE = Path('drivers/input/touchscreen/nt36532e')

# Only kernel platform helpers are replaced. The decoder and integer square
# root implementation are compiled from their actual sources.
HEADERS = {
    'types.h': '''#include <stdbool.h>
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
    'export.h': '#define EXPORT_SYMBOL(name)\n',
    'bitops.h': '''#define BITS_PER_LONG (__SIZEOF_LONG__ * 8)
#define __fls(x) (BITS_PER_LONG - 1 - __builtin_clzl(x))
''',
    'limits.h': '''#include <limits.h>
#include <stdint.h>
#define U16_MAX UINT16_MAX
#define U32_MAX UINT32_MAX
#define S32_MAX INT32_MAX
#define S32_MIN INT32_MIN
''',
    'math.h': '''#include <linux/types.h>
unsigned long int_sqrt(unsigned long value);
static inline u32 int_sqrt64(u64 value) { return int_sqrt(value); }
''',
    'math64.h': '''#include <linux/types.h>
static inline s64 div64_s64(s64 n, s64 d) { return n / d; }
static inline u64 div64_u64(u64 n, u64 d) { return n / d; }
''',
    'sort.h': '''#include <stdlib.h>
static inline void sort(void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *),
    void (*swap)(void *, void *, int))
{
    (void)swap;
    qsort(base, count, size, compare);
}
''',
    'string.h': '#include <string.h>\n',
    'unaligned.h': '''#include <linux/types.h>
static inline u16 get_unaligned_le16(const void *pointer)
{
    const u8 *p = pointer;
    return (u16)p[0] | (u16)p[1] << 8;
}
static inline u32 get_unaligned_le32(const void *pointer)
{
    const u8 *p = pointer;
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
''',
}

HARNESS = r'''
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nt36xxx-matrix.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
    exit(1); \
} } while (0)

#define ROWS 40
#define COLS 60
#define NODES (ROWS * COLS)
#define BYTES (64 + NODES * 2)
static unsigned char payload[64 + 4096 * 2];
static size_t bytes = BYTES;
static struct nvt_matrix_frame output;
unsigned int nvt_test_filter_calls;

size_t nvt_matrix_filtered_size(unsigned int capacity);
struct nvt_matrix *nvt_matrix_filtered_init(void *memory, unsigned int capacity);
int nvt_matrix_filtered_process(struct nvt_matrix *matrix, const u8 *payload,
    size_t bytes, struct nvt_matrix_frame *out);

static void le16(unsigned char *p, unsigned value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void le32(unsigned char *p, uint32_t value)
{
    for (int i = 0; i < 4; i++) p[i] = value >> (8 * i);
}

static void checksum(size_t covered)
{
    unsigned sum = 0;
    CHECK(covered >= 20 && !(covered % 4));
    le32(payload + 8, (covered - 20) / 4);
    le32(payload + 16, ~((uint32_t)(covered - 20) / 4));
    for (size_t i = 20; i < covered; i += 2)
        sum += payload[i] | payload[i + 1] << 8;
    le16(payload + 4, -sum);
    le16(payload + 12, ~(-sum));
}

static void blank(void)
{
    memset(payload, 0, sizeof(payload));
    bytes = BYTES;
    payload[48] = COLS;
    payload[49] = ROWS;
    payload[56] = 3;
    for (int i = 0; i < NODES; i++) le16(payload + 64 + i * 2, 1000);
}

static void cell(int row, int col, int value)
{
    CHECK(row >= 0 && row < ROWS && col >= 0 && col < COLS);
    le16(payload + 64 + (row * COLS + col) * 2, 1000 + value);
}

static void blob(int row, int col, int rows, int cols, int value)
{
    for (int r = row; r < row + rows; r++)
        for (int c = col; c < col + cols; c++) cell(r, c, value);
}

static int process(struct nvt_matrix *matrix)
{
    checksum(bytes);
    return nvt_matrix_process(matrix, payload, bytes, &output);
}

static struct nvt_matrix *create(unsigned capacity, void **memory)
{
    size_t size = nvt_matrix_size(capacity);
    CHECK(size);
    *memory = calloc(1, size);
    CHECK(*memory);
    struct nvt_matrix *matrix = nvt_matrix_init(*memory, capacity);
    CHECK(matrix);
    return matrix;
}

static void warmup(struct nvt_matrix *matrix)
{
    blank();
    for (int i = 0; i < 16 + 32; i++) CHECK(process(matrix) == 0);
    CHECK(process(matrix) == 1 && output.count == 0);
}

static void validation(void)
{
    void *memory;
    struct nvt_matrix *matrix = create(NODES, &memory);
    blank();
    checksum(bytes);
    for (size_t i = 0; i < 64; i++)
        CHECK(nvt_matrix_process(matrix, payload, i, &output) < 0);
    CHECK(nvt_matrix_process(matrix, payload, bytes - 1, &output) < 0);
    payload[bytes - 1] ^= 1;
    CHECK(nvt_matrix_process(matrix, payload, bytes, &output) < 0);
    payload[bytes - 1] ^= 1;
    checksum(64);
    CHECK(nvt_matrix_process(matrix, payload, bytes, &output) < 0);
    checksum(bytes);
    le32(payload + 8, UINT32_MAX);
    le32(payload + 16, 0);
    CHECK(nvt_matrix_process(matrix, payload, bytes, &output) < 0);
    blank();
    payload[48] = 255;
    payload[49] = 255;
    CHECK(process(matrix) < 0);
    blank();
    payload[48] = 0;
    CHECK(process(matrix) < 0);
    blank();
    payload[49] = 0;
    CHECK(process(matrix) < 0);
    /* Invalid records must not advance the learning state. */
    warmup(matrix);
    payload[48] = 80;
    payload[49] = 30;
    CHECK(process(matrix) < 0);
    blank();
    CHECK(process(matrix) == 1 && output.count == 0);
    free(memory);
}

static void capacity(void)
{
    CHECK(!nvt_matrix_size(0));
    CHECK(!nvt_matrix_size(4097));
    CHECK(!nvt_matrix_size(UINT32_MAX));
    CHECK(!nvt_matrix_init(NULL, NODES));
    for (unsigned cap = 1; cap <= 4096; cap = cap == 1 ? 4096 : 4097) {
        void *memory;
        struct nvt_matrix *matrix = create(cap, &memory);
        blank();
        if (cap == 1) {
            CHECK(process(matrix) < 0);
            payload[48] = payload[49] = 1;
            bytes = 68;
        } else {
            payload[48] = payload[49] = 64;
            bytes = sizeof(payload);
            for (int i = 0; i < 4096; i++) le16(payload + 64 + i * 2, 1000);
        }
        for (int i = 0; i < 16 + 32; i++) CHECK(process(matrix) == 0);
        CHECK(process(matrix) == 1 && output.count == 0);
        free(memory);
    }
}

static void reference_and_reset(void)
{
    void *memory;
    struct nvt_matrix *matrix = create(NODES, &memory);
    blank();
    /* Pen and SC types must not select the finger type or seed its baseline. */
    const int skip[] = {6, 7, 9, 17, 29};
    for (unsigned t = 0; t < sizeof(skip) / sizeof(skip[0]); t++) {
        payload[56] = skip[t];
        for (int i = 0; i < 64; i++) CHECK(process(matrix) == 0);
    }
    warmup(matrix);
    blob(19, 29, 3, 3, 600);
    cell(20, 30, 1500);
    CHECK(process(matrix) == 1 && output.count == 1);
    for (unsigned t = 0; t < sizeof(skip) / sizeof(skip[0]); t++) {
        payload[56] = skip[t];
        CHECK(process(matrix) == 0);
    }
    nvt_matrix_reset(matrix);
    warmup(matrix);
    blob(19, 29, 3, 3, 600);
    cell(20, 30, 1500);
    CHECK(process(matrix) == 1 && output.count == 1);
    nvt_matrix_reset(matrix);
    blank();
    for (int i = 0; i < 16; i++) CHECK(process(matrix) == 0);
    /* The upper median of sixteen 1000s and sixteen 1001s is 1001. */
    for (int i = 0; i < 32; i++) {
        cell(20, 30, i % 2);
        CHECK(process(matrix) == 0);
    }
    blank();
    cell(20, 30, 200);
    CHECK(process(matrix) == 1 && output.count == 0);
    cell(20, 30, 201);
    CHECK(process(matrix) == 1 && output.count == 1);
    free(memory);
}

static void interference(void)
{
    void *memory;
    struct nvt_matrix *matrix = create(NODES, &memory);
    warmup(matrix);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cell(r, c, (r % 7 - 3) * 300 + (c % 11 - 5) * 500);
    CHECK(process(matrix) == 1 && output.count == 0);
    /* Noise filtering must preserve a local touch on top of line bands. */
    for (int r = 19; r < 22; r++)
        for (int c = 29; c < 32; c++)
            cell(r, c, (r % 7 - 3) * 300 + (c % 11 - 5) * 500 +
                 (r == 20 && c == 30 ? 1500 : 600));
    CHECK(process(matrix) == 1 && output.count == 1);
    CHECK(!output.points[0].palm);
    free(memory);
}

static void contacts(void)
{
    void *memory;
    struct nvt_matrix *matrix = create(NODES, &memory);
    warmup(matrix);
    blob(19, 29, 3, 3, 600);
    cell(20, 30, 1500);
    CHECK(process(matrix) == 1 && output.count == 1);
    struct nvt_matrix_point point = output.points[0];
    CHECK(!point.palm && point.major > 0 && point.minor > 0);
    CHECK(point.major >= point.minor && point.major <= NVT_MATRIX_SHAPE_MAX);
    /* Four standard deviations of a filled 3x3-cell rectangle, at 13/mm. */
    CHECK(abs((int)point.major - 184) <= 1);
    CHECK(abs((int)point.minor - 179) <= 1);
    /* A symmetric blob centred at sensor (30.5,20.5), with Y reversed. */
    CHECK(abs(point.x - 1626) <= 1);
    CHECK(abs(point.y - 1041) <= 1);
    blank();
    blob(19, 27, 3, 8, 600);
    cell(20, 28, 1500);
    cell(20, 33, 1400);
    CHECK(process(matrix) == 1 && output.count == 2);
    CHECK(!output.points[0].palm && !output.points[1].palm);
    CHECK(abs(output.points[0].x - output.points[1].x) > 200);
    blank();
    CHECK(process(matrix) == 1 && output.count == 0);
    CHECK(process(matrix) == 1 && output.count == 0);
    free(memory);
}

static void palms(void)
{
    void *memory;
    struct nvt_matrix *matrix = create(NODES, &memory);
    warmup(matrix);
    blob(10, 10, 7, 10, 800);
    CHECK(process(matrix) == 1 && output.count == 1);
    CHECK(output.points[0].palm);
    CHECK(output.points[0].major >= output.points[0].minor);
    CHECK(output.points[0].minor > 0);
    CHECK(abs((int)output.points[0].major - 598) <= 1);
    CHECK(abs((int)output.points[0].minor - 428) <= 1);
    blank();
    blob(10, 10, 7, 10, 800);
    cell(16, 19, 0);
    CHECK(process(matrix) == 1 && output.count == 1);
    CHECK(!output.points[0].palm);
    blank();
    blob(10, 10, 7, 12, 800);
    cell(12, 12, 1200);
    cell(13, 19, 1100);
    blob(30, 50, 3, 3, 900);
    CHECK(process(matrix) == 1 && output.count == 2);
    CHECK(output.points[0].palm != output.points[1].palm);
    free(memory);
}

/* The reference is the same decoder with only its quiet shortcut disabled. */
static int compare_filtered(struct nvt_matrix *matrix, struct nvt_matrix *filtered)
{
    struct nvt_matrix_frame expected;
    checksum(bytes);
    int ret = nvt_matrix_process(matrix, payload, bytes, &output);
    CHECK(ret == nvt_matrix_filtered_process(filtered, payload, bytes, &expected));
    CHECK(output.count == expected.count);
    for (unsigned i = 0; i < output.count; i++) {
        CHECK(output.points[i].x == expected.points[i].x);
        CHECK(output.points[i].y == expected.points[i].y);
        CHECK(output.points[i].major == expected.points[i].major);
        CHECK(output.points[i].minor == expected.points[i].minor);
        CHECK(output.points[i].palm == expected.points[i].palm);
    }
    return ret;
}

static struct nvt_matrix *filtered_reference(void **memory)
{
    size_t size = nvt_matrix_filtered_size(NODES);
    CHECK(size);
    *memory = calloc(1, size);
    CHECK(*memory);
    struct nvt_matrix *matrix = nvt_matrix_filtered_init(*memory, NODES);
    CHECK(matrix);
    return matrix;
}

static void warmup_pair(struct nvt_matrix *matrix, struct nvt_matrix *filtered)
{
    blank();
    for (int i = 0; i < 16 + 32; i++)
        CHECK(compare_filtered(matrix, filtered) == 0);
    CHECK(compare_filtered(matrix, filtered) == 1 && output.count == 0);
}

static void quiet_boundary(void)
{
    const struct { int minimum, maximum; unsigned filters; } cases[] = {
        {0, 0, 0}, {-49, 0, 0}, {0, 49, 0}, {-25, 24, 0}, {-24, 25, 0},
        {-50, 0, 1}, {0, 50, 1}, {-25, 25, 1},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        void *memory, *filtered_memory;
        struct nvt_matrix *matrix = create(NODES, &memory);
        struct nvt_matrix *filtered = filtered_reference(&filtered_memory);
        warmup_pair(matrix, filtered);
        blank();
        cell(7, 9, cases[c].minimum);
        cell(23, 44, cases[c].maximum);
        nvt_test_filter_calls = 0;
        CHECK(compare_filtered(matrix, filtered) == 1 && output.count == 0);
        CHECK(nvt_test_filter_calls == cases[c].filters);

        /* Subsequent contacts must retain their coordinates, area and count. */
        blank();
        blob(19, 29, 3, 3, 600);
        cell(20, 30, 1500);
        CHECK(compare_filtered(matrix, filtered) == 1 && output.count == 1);
        blank();
        CHECK(compare_filtered(matrix, filtered) == 1 && output.count == 0);
        blob(19, 27, 3, 8, 600);
        cell(20, 28, 1500);
        cell(20, 33, 1400);
        CHECK(compare_filtered(matrix, filtered) == 1 && output.count == 2);
        free(filtered_memory);
        free(memory);
    }
}

static void quiet_signed_drift(void)
{
    const struct { int delta, drift; } cases[] = {
        {-49, -6}, {-17, -2}, {-16, -2}, {-15, -1}, {-9, -1}, {-8, -1},
        {-7, 0}, {-1, 0}, {1, 0}, {7, 0}, {8, 1}, {9, 1},
        {15, 1}, {16, 2}, {17, 2}, {49, 6},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        void *memory, *filtered_memory;
        struct nvt_matrix *matrix = create(NODES, &memory);
        struct nvt_matrix *filtered = filtered_reference(&filtered_memory);
        warmup_pair(matrix, filtered);
        blank();
        cell(20, 30, cases[c].delta);
        nvt_test_filter_calls = 0;
        CHECK(compare_filtered(matrix, filtered) == 1 && output.count == 0);
        CHECK(nvt_test_filter_calls == 0);

        /* Probe the learned baseline through the public touch threshold.
         * A signed shift or rounding away from zero changes these counts.
         */
        blank();
        cell(20, 30, cases[c].drift + 199);
        CHECK(compare_filtered(matrix, filtered) == 1 && output.count == 0);
        cell(20, 30, cases[c].drift + 200);
        CHECK(compare_filtered(matrix, filtered) == 1 && output.count == 1);
        free(filtered_memory);
        free(memory);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "validation")) validation();
    else if (!strcmp(argv[1], "capacity")) capacity();
    else if (!strcmp(argv[1], "reference")) reference_and_reset();
    else if (!strcmp(argv[1], "interference")) interference();
    else if (!strcmp(argv[1], "contacts")) contacts();
    else if (!strcmp(argv[1], "palms")) palms();
    else if (!strcmp(argv[1], "quiet-boundary")) quiet_boundary();
    else if (!strcmp(argv[1], "quiet-drift")) quiet_signed_drift();
    else CHECK(0);
    return 0;
}
'''


class Nt36xxxMatrixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reason = None
        compiler = shutil.which('cc')
        sqrt = ROOT / 'upstream/linux-piano/lib/math/int_sqrt.c'
        if not compiler or not sqrt.is_file():
            cls.reason = 'Host C compiler or Linux integer square-root source unavailable'
            return
        cls.workspace = tempfile.TemporaryDirectory(prefix='nt36xxx-matrix-')
        folder = Path(cls.workspace.name)
        subprocess.run(['git', 'apply', '--no-index',
                        '--include=' + str(SOURCE / 'nt36xxx-matrix.c'),
                        '--include=' + str(SOURCE / 'nt36xxx-matrix.h'), str(PATCH)],
                       cwd=folder, check=True)
        headers = folder / 'include/linux'
        headers.mkdir(parents=True)
        for name, text in HEADERS.items():
            (headers / name).write_text(text)
        harness = folder / 'matrix-test.c'
        harness.write_text(HARNESS)
        core = (folder / SOURCE / 'nt36xxx-matrix.c').read_text()
        shortcut = 'if (maximum - minimum < NVT_MATRIX_THRESHOLD / 4)'
        filter_entry = 'static s32 nvt_matrix_filter(struct nvt_matrix *matrix)\n{'
        if core.count(shortcut) != 1 or core.count(filter_entry) != 1:
            raise AssertionError('Expected one quiet shortcut and one matrix filter')
        # Count calls in the tested core; keep all of its decoding code intact.
        measured = folder / 'matrix-measured.c'
        measured.write_text(core.replace(filter_entry, filter_entry +
            '\n\textern unsigned int nvt_test_filter_calls;\n'
            '\n\tnvt_test_filter_calls++;'))
        filtered = folder / 'matrix-filtered.c'
        filtered.write_text(core.replace(shortcut, 'if (false)'))
        flags = [compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                 '-Werror', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-I', str(headers.parent),
                 '-I', str(folder / SOURCE)]
        reference_object = folder / 'matrix-filtered.o'
        renames = ['-Dnvt_matrix_' + name + '=nvt_matrix_filtered_' + name
                   for name in ('size', 'init', 'reset', 'process')]
        subprocess.run([*flags, *renames, '-c', str(filtered),
                        '-o', str(reference_object)], check=True)
        cls.binary = folder / 'matrix-test'
        subprocess.run([*flags, str(harness), str(measured),
                        str(reference_object), str(sqrt),
                        '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        if not cls.reason:
            cls.workspace.cleanup()

    def run_case(self, case):
        if self.reason:
            self.skipTest(self.reason)
        # The core allocates nothing; leak tracing requires ptrace permissions
        # on some hosts, while address/undefined checks work without them.
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        subprocess.run([str(self.binary), case], env=env, check=True, timeout=10)

    def test_malformed_payloads_cannot_seed_reference_or_overrun_buffers(self):
        self.run_case('validation')

    def test_capacity_and_geometry_bounds(self):
        self.run_case('capacity')

    def test_type_probe_reference_learning_and_epoch_reset(self):
        self.run_case('reference')

    def test_row_and_column_interference_preserves_local_touch(self):
        self.run_case('interference')

    def test_single_touch_adjacent_fingers_and_idle_release(self):
        self.run_case('contacts')

    def test_palm_threshold_and_separate_finger(self):
        self.run_case('palms')

    def test_quiet_range_49_skips_filter_but_50_preserves_contact_results(self):
        self.run_case('quiet-boundary')

    def test_quiet_signed_drift_truncates_toward_zero(self):
        self.run_case('quiet-drift')


if __name__ == '__main__':
    unittest.main()
