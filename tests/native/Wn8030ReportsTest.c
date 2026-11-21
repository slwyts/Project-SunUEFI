/* SPDX-License-Identifier: BSD-2-Clause */
#include "reports.h"
#include <assert.h>
#include <string.h>

struct log {
    uint8_t data[12][WN8030_REPORT_MAX];
    size_t length[12], count, fail;
};

static int record(void *context, const uint8_t *p, size_t n)
{
    struct log *l = context;
    if (l->fail && l->fail == l->count + 1) return 0;
    assert(l->count < 12 && n <= WN8030_REPORT_MAX);
    memcpy(l->data[l->count], p, n);
    l->length[l->count++] = n;
    return 1;
}

static void point(uint8_t *frame, size_t slot, uint8_t id, uint16_t x, uint16_t y)
{
    uint8_t *p = frame + 3 + 2 + 8 * slot;
    p[0] = 7; p[1] = id; p[2] = 32;
    p[4] = (uint8_t)x; p[5] = (uint8_t)(x >> 8);
    p[6] = (uint8_t)y; p[7] = (uint8_t)(y >> 8);
}

int main(void)
{
    struct wn8030_reports s;
    wn8030_reports_init(&s);
    struct log l = {0};
    uint8_t f[68] = {0xd7, 0, 0x39, 5, 0x05, 0, 4, 5, 6, 0, 0, 0,
                      6, 0xe9, 0, 0xff, 0xff, 2, 1, 0xfe, 0xff, 4, 0, 0, 0};
    assert(wn8030_frame(&s, f, sizeof(f), 15, record, &l) == WN8030_OK);
    assert(l.count == 3 && l.length[0] == 9 && l.length[1] == 3 && l.length[2] == 8);
    assert(memcmp(l.data[0], f + 3, 9) == 0 && memcmp(l.data[1], f + 12, 3) == 0);
    assert(memcmp(l.data[2], f + 17, 8) == 0);
    memset(f + 3, 0, 65);
    f[3] = 0x22; f[5] = 0x38; f[6] = 0x80; f[7] = 0xa2; f[12] = 2;
    f[19] = 5; f[22] = 4;
    assert(wn8030_frame(&s, f, sizeof(f), 16, record, &l) == WN8030_OK);
    assert(l.count == 6 && l.data[3][0] == 5 && l.data[3][3] == 0);
    assert(l.data[4][0] == 6 && !l.data[4][1] && l.data[5][0] == 2 && !l.data[5][1]);
    assert(s.detached && s.attachment_known);
    l.count = 0; f[12] = 1;
    assert(wn8030_frame(&s, f, sizeof(f), 17, record, &l) == WN8030_OK);
    assert(l.count == 1 && l.data[0][3] == 4 && !s.detached);

    /* A truncated fifth report does not deliver an earlier, otherwise valid chord. */
    l.count = 0; memset(f + 3, 0, 65); f[3] = 5; f[12] = 0x19;
    f[39] = 0x22; f[55] = 0x19;
    assert(wn8030_frame(&s, f, sizeof(f), 20, record, &l) == WN8030_BAD_FRAME && !l.count);

    uint8_t feature[257];
    size_t size = sizeof(feature);
    assert(wn8030_get_feature(&s, WN8030_CAPABILITIES, feature, &size) == WN8030_OK);
    assert(size == 3 && feature[1] == 3 && feature[2] == 0);
    assert(wn8030_get_feature(&s, WN8030_CERT_STATUS, feature, &size) == WN8030_SHORT && size == 257);
    memset(feature, 0xcd, sizeof(feature));
    assert(wn8030_get_feature(&s, WN8030_CERT_STATUS, feature, &size) == WN8030_OK);
    for (size_t i = 1; i < size; ++i) assert(!feature[i]);

    uint8_t mode[2] = { WN8030_INPUT_MODE, 3 };
    memset(f + 3, 0, 65); f[3] = 0x19; f[29] = 1;
    point(f, 0, 1, 1000, 2000);
    assert(wn8030_frame(&s, f, sizeof(f), 21, record, &l) == WN8030_OK && !l.count);
    assert(wn8030_set_feature(&s, mode[0], mode, 2, 22, record, &l) == WN8030_OK);
    assert(wn8030_frame(&s, f, sizeof(f), 23, record, &l) == WN8030_OK && !l.count);
    f[29] = 0;
    assert(wn8030_frame(&s, f, sizeof(f), 24, record, &l) == WN8030_OK);
    l.count = 0; f[29] = 3; f[4] = 1;
    point(f, 1, 2, 3199, 2135); point(f, 2, 3, 0, 0);
    assert(wn8030_frame(&s, f, sizeof(f), 0xffff, record, &l) == WN8030_OK);
    assert(l.count == 1 && l.length[0] == 23 && l.data[0][1] == 3 && l.data[0][2] == 255 &&
           l.data[0][3] == 255 && l.data[0][4] == 1 && l.data[0][5] == 3 && l.data[0][6] == 1 &&
           l.data[0][7] == 0xe8 && l.data[0][8] == 3 && l.data[0][9] == 0xd0 && l.data[0][10] == 7);
    point(f, 0, 4, 22, 33); point(f, 1, 5, 44, 55); point(f, 2, 6, 66, 77);
    assert(wn8030_frame(&s, f, sizeof(f), 0, record, &l) == WN8030_OK);
    assert(l.count == 3 && l.data[1][1] == 3 && l.data[1][5] == 2 && l.data[1][6] == 1);
    assert(!memcmp(l.data[1] + 7, l.data[0] + 7, 4) && l.data[2][6] == 4 && s.count == 3);
    point(f, 1, 4, 10, 10);
    assert(wn8030_frame(&s, f, sizeof(f), 1, record, &l) == WN8030_BAD_FRAME && l.count == 3);
    point(f, 1, 5, 3200, 100);
    assert(wn8030_frame(&s, f, sizeof(f), 1, record, &l) == WN8030_BAD_FRAME && l.count == 3);
    uint8_t switches[2] = { WN8030_FUNCTION_SWITCH, 2 };
    l.fail = 4;
    assert(wn8030_set_feature(&s, switches[0], switches, 2, 2, record, &l) == WN8030_IO &&
           s.count == 3 && s.switches == 3);
    l.fail = 0;
    assert(wn8030_set_feature(&s, switches[0], switches, 2, 2, record, &l) == WN8030_OK);
    assert(l.count == 4 && l.data[3][5] == 2 && l.data[3][4] == 1 && !s.count);
    assert(wn8030_release(&s, 3, record, &l) == WN8030_OK && !s.buttons && !s.keyboard_active);
    return 0;
}
