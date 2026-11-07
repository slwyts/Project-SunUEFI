/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "piano-pen-owner.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#define TRANSPORT_BYTES 257

struct piano_pen_owner {
    struct piano_pen_core *core;
    struct piano_pen_info info;
    struct piano_pen_owner_stats stats;
    struct piano_pen_input_area area;
    char *hidraw;
    uint64_t retry_at, max_age, last_frame, last_pressure;
    int hid_fd, notify_fd, input_fd, tool_present, contact_present, have_frame, release_pending;
    int pressure_started;
    int squeeze_down, squeeze_reported;
    int squeeze_clicks, squeeze_long;
    int tap_down, tap_reported;
    uint64_t squeeze_started;
};

#define SQUEEZE_HOLD_NS UINT64_C(600000000)

static uint64_t owner_boot_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_BOOTTIME, &t))
        return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

static int input_events(struct piano_pen_owner *p,
    struct input_event *events, size_t count)
{
    ssize_t bytes;
    do {
        bytes = write(p->input_fd, events, count * sizeof(*events));
    } while (bytes < 0 && errno == EINTR);
    if (bytes != (ssize_t)(count * sizeof(*events)))
        return bytes < 0 ? -errno : -EIO;
    return 0;
}

static void release_tool(struct piano_pen_owner *p)
{
    if (p->input_fd < 0 || !p->tool_present)
        return;
    struct input_event events[] = {
        { .type = EV_KEY, .code = BTN_TOUCH, .value = 0 },
        { .type = EV_KEY, .code = BTN_STYLUS, .value = 0 },
        { .type = EV_KEY, .code = BTN_STYLUS2, .value = 0 },
        { .type = EV_KEY, .code = BTN_TOOL_PEN, .value = 0 },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 }
    };
    p->release_pending = 1;
    if (input_events(p, events, sizeof(events) / sizeof(events[0]))) {
        p->stats.input_errors++;
        return;
    }
    p->tool_present = 0;
    p->contact_present = 0;
    p->squeeze_reported = 0;
    p->squeeze_started = 0;
    p->squeeze_long = 0;
    p->tap_reported = 0;
    p->release_pending = 0;
}

static void reset_owner(struct piano_pen_owner *p)
{
    release_tool(p);
    piano_pen_core_reset(p->core);
    p->have_frame = 0;
    p->last_pressure = 0;
    p->pressure_started = 0;
    p->squeeze_down = 0;
    p->squeeze_started = 0;
    p->squeeze_long = 0;
    p->tap_down = 0;
}

static int matching_hid(const char *path)
{
    struct hidraw_devinfo info = { 0 };
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0 ||
        info.bustype != BUS_BLUETOOTH || info.vendor != 0x22 ||
        info.product != 0x5081) {
        close(fd);
        return -ENODEV;
    }
    return fd;
}

struct piano_pen_owner *piano_pen_owner_create(const char *ini,
    const char *hidraw, uint64_t age)
{
    struct piano_pen_owner *p = calloc(1, sizeof(*p));
    char error[256] = { 0 };
    if (!p)
        return NULL;
    p->hid_fd = p->notify_fd = p->input_fd = -1;
    const char *squeeze = getenv("PIANO_PEN_SQUEEZE_ACTION");
    p->squeeze_clicks = !squeeze || strcmp(squeeze, "stylus");
    p->max_age = age;
    p->core = piano_pen_core_create(ini, 3, PIANO_PEN_PORTRAIT,
                                    age, error, sizeof(error));
    if (!p->core) {
        fprintf(stderr, "pen configuration: %s\n", error);
        free(p);
        return NULL;
    }
    if (piano_pen_core_info(p->core, &p->info))
        goto failed;
    if (hidraw) {
        p->hidraw = strdup(hidraw);
        if (!p->hidraw)
            goto failed;
    }
    p->notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (p->notify_fd >= 0 &&
        inotify_add_watch(p->notify_fd, "/dev", IN_CREATE | IN_MOVED_TO | IN_ATTRIB) < 0) {
        close(p->notify_fd);
        p->notify_fd = -1;
    }
    return p;
failed:
    piano_pen_owner_destroy(p);
    return NULL;
}

void piano_pen_owner_destroy(struct piano_pen_owner *p)
{
    if (!p)
        return;
    release_tool(p);
    if (p->input_fd >= 0) {
        ioctl(p->input_fd, UI_DEV_DESTROY);
        close(p->input_fd);
    }
    if (p->hid_fd >= 0)
        close(p->hid_fd);
    if (p->notify_fd >= 0)
        close(p->notify_fd);
    piano_pen_core_destroy(p->core);
    free(p->hidraw);
    free(p);
}

int piano_pen_owner_poll_fd(struct piano_pen_owner *p)
{
    if (!p)
        return -1;
    if (p->hid_fd >= 0)
        return p->hid_fd;
    uint64_t now = owner_boot_ns();
    if (!now || now < p->retry_at)
        return p->notify_fd;
    p->retry_at = now + UINT64_C(1000000000);
    if (p->hidraw) {
        int fd = matching_hid(p->hidraw);
        if (fd >= 0)
            p->hid_fd = fd;
        return p->hid_fd >= 0 ? p->hid_fd : p->notify_fd;
    }
    glob_t paths = { 0 };
    int selected = -1, matches = 0;
    if (!glob("/dev/hidraw*", 0, NULL, &paths)) {
        for (size_t i = 0; i < paths.gl_pathc; i++) {
            int fd = matching_hid(paths.gl_pathv[i]);
            if (fd < 0)
                continue;
            matches++;
            if (selected >= 0)
                close(fd);
            else
                selected = fd;
        }
    }
    globfree(&paths);
    if (matches == 1)
        p->hid_fd = selected;
    else if (selected >= 0) {
        close(selected);
        fprintf(stderr, "multiple Focus Pen Pro devices; select --pen-hidraw\n");
    }
    if (p->hid_fd >= 0)
        fprintf(stderr, "pen: pressure transport opened at %llu ns\n",
                (unsigned long long)now);
    return p->hid_fd >= 0 ? p->hid_fd : p->notify_fd;
}

/* cancel: another gesture replaced the squeeze; never turn it into a click */
static void report_squeeze(struct piano_pen_owner *p, int down, int cancel)
{
    if (down == p->squeeze_down)
        return;
    p->squeeze_down = down;
    if (p->squeeze_clicks) {
        if (down) {
            /* A squeeze must act at a current pen location, never at the
             * unrelated seat mouse position. Outside proximity it does not
             * produce a desktop click.
             */
            p->squeeze_started = p->tool_present ? owner_boot_ns() : 0;
            p->squeeze_long = 0;
        } else if (p->squeeze_started && p->tool_present) {
            if (p->squeeze_long) {
                struct input_event events[] = {
                    { .type = EV_KEY, .code = BTN_STYLUS, .value = 0 },
                    { .type = EV_SYN, .code = SYN_REPORT }
                };
                if (input_events(p, events, 2))
                    p->stats.input_errors++;
                p->squeeze_reported = 0;
            } else if (!cancel && !p->contact_present) {
                /* Primary activation on the same tablet tool. Retain its
                 * real position and pressure; do not interrupt a held tip.
                 */
                struct input_event events[] = {
                    { .type = EV_KEY, .code = BTN_TOUCH, .value = 1 },
                    { .type = EV_SYN, .code = SYN_REPORT },
                    { .type = EV_KEY, .code = BTN_TOUCH, .value = 0 },
                    { .type = EV_SYN, .code = SYN_REPORT }
                };
                if (input_events(p, events, 4))
                    p->stats.input_errors++;
            }
            p->squeeze_started = 0;
            p->squeeze_long = 0;
        }
        return;
    }
    /* Tool buttons belong to the actual tablet tool. No global keyboard
     * shortcut is emitted while the pen is outside the display's range.
     */
    int value = p->tool_present && down;
    if (p->input_fd < 0 || value == p->squeeze_reported)
        return;
    struct input_event events[] = {
        { .type = EV_KEY, .code = BTN_STYLUS, .value = value },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 }
    };
    if (input_events(p, events, sizeof(events) / sizeof(events[0])))
        p->stats.input_errors++;
    else
        p->squeeze_reported = value;
}

/* A tool button needs a pen position; out of range the tap is dropped */
static void report_double_tap(struct piano_pen_owner *p, int down)
{
    if (down == p->tap_down)
        return;
    p->tap_down = down;
    int value = down && p->tool_present;
    if (p->input_fd < 0 || value == p->tap_reported)
        return;
    struct input_event events[] = {
        { .type = EV_KEY, .code = BTN_STYLUS2, .value = value },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 }
    };
    if (input_events(p, events, sizeof(events) / sizeof(events[0])))
        p->stats.input_errors++;
    else
        p->tap_reported = value;
}

void piano_pen_owner_read_hid(struct piano_pen_owner *p, short revents)
{
    if (!p)
        return;
    if (p->hid_fd < 0) {
        /* A new HID device should be consumed immediately, not after the
         * one-second fallback discovery interval. Keep this in the existing
         * poll loop: no reconnect thread or faster periodic scanning.
         */
        if (p->notify_fd >= 0 && (revents & POLLIN)) {
            union {
                struct inotify_event aligned;
                char bytes[4096];
            } events;
            ssize_t size;
            while ((size = read(p->notify_fd, events.bytes, sizeof(events.bytes))) > 0) {
                size_t offset = 0;
                while (offset + sizeof(struct inotify_event) <= (size_t)size) {
                    const struct inotify_event *event =
                        (const struct inotify_event *)(events.bytes + offset);
                    if ((event->mask & IN_Q_OVERFLOW) ||
                        (event->len && !strncmp(event->name, "hidraw", 6)))
                        p->retry_at = 0;
                    offset += sizeof(*event) + event->len;
                }
            }
        }
        return;
    }
    if (revents & (POLLHUP | POLLERR | POLLNVAL))
        goto disconnected;
    if (!(revents & POLLIN))
        return;
    /* Bound one dispatch so pressure cannot starve the THP owner. */
    for (unsigned int i = 0; i < 64; i++) {
        uint8_t report[64];
        ssize_t bytes = read(p->hid_fd, report, sizeof(report));
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes <= 0)
            goto disconnected;
        if (bytes == 2 && report[0] == 2) {
            /* Keyboard array: a new value ends the previous one. The kernel
             * reports slides as page keys
             */
            uint8_t value = report[1];
            report_squeeze(p, value == PIANO_PEN_BODY_SQUEEZE,
                           value != PIANO_PEN_BODY_RELEASE);
            report_double_tap(p, value == PIANO_PEN_BODY_DOUBLE_TAP);
            continue;
        }
        if (report[0] != 5)
            continue;
        uint64_t receipt = owner_boot_ns();
        char error[256];
        if (!receipt || piano_pen_core_report5(p->core, report,
                (size_t)bytes, receipt, error, sizeof(error)))
            p->stats.rejected_reports++;
        else {
            if (!p->pressure_started) {
                p->pressure_started = 1;
                fprintf(stderr, "pen: first pressure report at %llu ns\n",
                        (unsigned long long)receipt);
            }
            p->stats.reports5++;
        }
    }
    return;
disconnected:
    close(p->hid_fd);
    p->hid_fd = -1;
    reset_owner(p);
}

void piano_pen_owner_tick(struct piano_pen_owner *p)
{
    if (!p || p->input_fd < 0)
        return;
    if (p->release_pending) {
        release_tool(p);
        return;
    }
    uint64_t now = owner_boot_ns();
    if (!now)
        return;
    if (p->squeeze_clicks && p->squeeze_down && p->squeeze_started &&
        !p->squeeze_long && now >= p->squeeze_started &&
        now - p->squeeze_started >= SQUEEZE_HOLD_NS) {
        /* Mark it consumed even on disconnect; release must not turn a
         * failed secondary click into an unexpected primary click.
         */
        p->squeeze_long = 1;
        if (p->tool_present) {
            struct input_event events[] = {
                { .type = EV_KEY, .code = BTN_STYLUS, .value = 1 },
                { .type = EV_SYN, .code = SYN_REPORT }
            };
            if (input_events(p, events, 2))
                p->stats.input_errors++;
            else
                p->squeeze_reported = 1;
        }
    }
    if (p->tool_present && p->have_frame && now >= p->last_frame &&
        now - p->last_frame > p->max_age) {
        release_tool(p);
        p->stats.policy_stream_releases++;
        return;
    }
    if (p->contact_present && now >= p->last_pressure &&
        now - p->last_pressure > p->max_age) {
        struct input_event events[] = {
            { .type = EV_KEY, .code = BTN_TOUCH, .value = 0 },
            { .type = EV_SYN, .code = SYN_REPORT, .value = 0 }
        };
        if (input_events(p, events, sizeof(events) / sizeof(events[0])))
            p->stats.input_errors++;
        else {
            p->contact_present = 0;
            p->stats.policy_contact_releases++;
        }
    }
}

static void report_decoded(struct piano_pen_owner *p,
    const struct piano_pen_output *original)
{
    if (p->input_fd < 0)
        return;
    struct piano_pen_output out = *original;
    uint64_t now = owner_boot_ns();
    if (!now || now < out.timestamp_ns) {
        p->stats.input_errors++;
        return;
    }
    if (now - out.timestamp_ns > p->max_age) {
        if (p->tool_present) {
            release_tool(p);
            p->stats.policy_stream_releases++;
        }
        return;
    }
    int proximity = !!(out.valid & PIANO_PEN_COORDINATES_VALID);
    int contact = 0;
    if ((out.valid & PIANO_PEN_PRESSURE_VALID) &&
        out.pressure_age_ns <= out.timestamp_ns) {
        uint64_t pressure_time = out.timestamp_ns - out.pressure_age_ns;
        if (!now || now < pressure_time || now - pressure_time > p->max_age)
            out.valid &= ~PIANO_PEN_PRESSURE_VALID;
        else {
            p->last_pressure = pressure_time;
            contact = proximity && out.pressure > 0;
        }
    }
    int policy_release = p->contact_present && !contact &&
        !(out.valid & PIANO_PEN_PRESSURE_VALID);
    int32_t x = out.y;
    int32_t y = p->info.x_max - out.x;
    int32_t tilt_x = out.tilt_y;
    out.tilt_y = -out.tilt_x;
    out.tilt_x = tilt_x;
    int ret = piano_pen_owner_input_report(p, &out,
        PIANO_PEN_PROXIMITY_KNOWN | PIANO_PEN_CONTACT_KNOWN,
        proximity, contact, x, y);
    if (ret)
        p->stats.input_errors++;
    else {
        p->stats.input_frames++;
        if (policy_release)
            p->stats.policy_contact_releases++;
    }
}

int piano_pen_owner_record(struct piano_pen_owner *p,
    const uint8_t *frame, size_t bytes, uint64_t time,
    uint16_t flags, struct piano_pen_output *output)
{
    if (!p || !output || !frame)
        return -EINVAL;
    *output = (struct piano_pen_output){ 0 };
    if (flags & 2) {
        reset_owner(p);
        p->stats.epochs++;
    }
    if (bytes < TRANSPORT_BYTES + 100 || frame[TRANSPORT_BYTES + 56] != 29)
        return 0;
    char error[256];
    int ret = piano_pen_core_process29(p->core, frame + TRANSPORT_BYTES,
        bytes - TRANSPORT_BYTES, time, output, error, sizeof(error));
    if (ret) {
        p->stats.rejected_frames++;
        return ret;
    }
    p->stats.frames29++;
    p->last_frame = time;
    p->have_frame = 1;
    report_decoded(p, output);
    return 1;
}

void piano_pen_owner_stats(const struct piano_pen_owner *p,
    struct piano_pen_owner_stats *stats)
{
    if (p && stats)
        *stats = p->stats;
}

static int setup_abs(int fd, int code, int maximum, int minimum, int resolution)
{
    struct uinput_abs_setup axis = { .code = code };
    axis.absinfo.minimum = minimum;
    axis.absinfo.maximum = maximum;
    axis.absinfo.resolution = resolution;
    if (ioctl(fd, UI_SET_ABSBIT, code) || ioctl(fd, UI_ABS_SETUP, &axis))
        return -errno;
    return 0;
}

int piano_pen_owner_enable_input(struct piano_pen_owner *p)
{
    if (!p || !p->max_age)
        return -EINVAL;
    const struct piano_pen_input_area area = {
        .x_max = p->info.y_max, .y_max = p->info.x_max,
        .x_resolution = 1339, .y_resolution = 1310, .tilt_supported = 1
    };
    int ret = piano_pen_owner_input_open(p, &area);
    if (!ret)
        fprintf(stderr, "pen input uses explicit %llu ns pressure/stream freshness policy; stale pressure releases contact\n",
                (unsigned long long)p->max_age);
    return ret;
}

int piano_pen_owner_input_open(struct piano_pen_owner *p,
    const struct piano_pen_input_area *area)
{
    if (!p || !area || p->input_fd >= 0 || area->x_max <= 0 ||
        area->y_max <= 0 || area->x_resolution < 0 || area->y_resolution < 0)
        return -EINVAL;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    int ret = 0;
    struct uinput_setup device = { 0 };
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) || ioctl(fd, UI_SET_EVBIT, EV_ABS) ||
        ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN) || ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) ||
        ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS) || ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS2) ||
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)) {
        ret = -errno;
        goto failed;
    }
    if ((ret = setup_abs(fd, ABS_X, area->x_max, 0, area->x_resolution)) ||
        (ret = setup_abs(fd, ABS_Y, area->y_max, 0, area->y_resolution)) ||
        (ret = setup_abs(fd, ABS_PRESSURE, p->info.pressure_max, 0, 0)))
        goto failed;
    if (area->tilt_supported &&
        ((ret = setup_abs(fd, ABS_TILT_X, p->info.tilt_max, p->info.tilt_min, 0)) ||
         (ret = setup_abs(fd, ABS_TILT_Y, p->info.tilt_max, p->info.tilt_min, 0))))
        goto failed;
    device.id.bustype = BUS_VIRTUAL;
    device.id.vendor = 0x22;
    device.id.product = 0x5081;
    device.id.version = 1;
    snprintf(device.name, sizeof(device.name), "Piano Focus Pen Pro");
    if (ioctl(fd, UI_DEV_SETUP, &device) || ioctl(fd, UI_DEV_CREATE)) {
        ret = -errno;
        goto failed;
    }
    p->area = *area;
    p->input_fd = fd;
    return 0;
failed:
    close(fd);
    return ret;
}

int piano_pen_owner_input_report(struct piano_pen_owner *p,
    const struct piano_pen_output *out, uint32_t known,
    int proximity, int contact, int32_t x, int32_t y)
{
    if (!p || !out || p->input_fd < 0)
        return -ENODEV;
    if ((known & 3) != 3)
        return -EAGAIN;
    if ((proximity != 0 && proximity != 1) || (contact != 0 && contact != 1) ||
        (contact && (!proximity || !(out->valid & PIANO_PEN_PRESSURE_VALID))))
        return -EINVAL;
    if ((out->valid & PIANO_PEN_PRESSURE_VALID) &&
        (out->pressure < 0 || out->pressure > p->info.pressure_max))
        return -ERANGE;
    if (proximity && (!(out->valid & PIANO_PEN_COORDINATES_VALID) ||
        x < 0 || x > p->area.x_max || y < 0 || y > p->area.y_max))
        return -ERANGE;
    struct input_event events[12];
    size_t n = 0;
#define ADD(t, c, v) events[n++] = (struct input_event){ .type = (t), .code = (c), .value = (v) }
    ADD(EV_KEY, BTN_TOUCH, contact);
    ADD(EV_KEY, BTN_TOOL_PEN, proximity);
    ADD(EV_KEY, BTN_STYLUS, proximity && p->squeeze_down &&
        (!p->squeeze_clicks || p->squeeze_long));
    ADD(EV_KEY, BTN_STYLUS2, proximity && p->tap_reported);
    if (proximity) {
        ADD(EV_ABS, ABS_X, x);
        ADD(EV_ABS, ABS_Y, y);
    }
    if (out->valid & PIANO_PEN_PRESSURE_VALID)
        ADD(EV_ABS, ABS_PRESSURE, out->pressure);
    if (proximity && p->area.tilt_supported && (out->valid & PIANO_PEN_TILT_VALID)) {
        ADD(EV_ABS, ABS_TILT_X, out->tilt_x);
        ADD(EV_ABS, ABS_TILT_Y, out->tilt_y);
    }
    ADD(EV_SYN, SYN_REPORT, 0);
#undef ADD
    int ret = input_events(p, events, n);
    if (!ret) {
        p->tool_present = proximity;
        p->contact_present = contact;
        p->squeeze_reported = proximity && p->squeeze_down &&
            (!p->squeeze_clicks || p->squeeze_long);
        p->tap_reported = proximity && p->tap_reported;
    }
    return ret;
}
