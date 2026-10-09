/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PIANO_PEN_OWNER_H
#define PIANO_PEN_OWNER_H
#include "piano-pen-core.h"
#ifdef __cplusplus
extern "C" {
#endif

struct piano_pen_owner;
struct piano_pen_owner_stats {
    uint64_t reports5, rejected_reports, frames29, rejected_frames, epochs;
    uint64_t input_frames, input_errors, policy_contact_releases, policy_stream_releases;
};

/* All calls run in the existing touch owner's poll thread. The helper opens
 * only HID pressure and optional uinput; it never opens the THP FIFO.
 * Receipt and NTP1 timestamps must both use CLOCK_BOOTTIME. */
struct piano_pen_owner *piano_pen_owner_create(const char *ini,
    const char *hidraw, uint64_t pressure_max_age_ns);
void piano_pen_owner_destroy(struct piano_pen_owner *owner);
int piano_pen_owner_poll_fd(struct piano_pen_owner *owner);
void piano_pen_owner_read_hid(struct piano_pen_owner *owner, short revents);
void piano_pen_owner_tick(struct piano_pen_owner *owner);
int piano_pen_owner_record(struct piano_pen_owner *owner,
    const uint8_t *frame, size_t bytes, uint64_t timestamp_ns,
    uint16_t stream_flags, struct piano_pen_output *output);
void piano_pen_owner_stats(const struct piano_pen_owner *owner,
    struct piano_pen_owner_stats *stats);

/* Optional standard tablet-tool reporting. The verified landscape mapping is
 * X=portraitY,Y=portraitX_max-portraitX; tiltX=portraitTiltY,tiltY=-portraitTiltX.
 * Fresh real pressure>0 means contact, true0 hover. Missing/stale pressure
 * releases contact under the explicit freshness policy; it is not reported as
 * an invented zero-pressure sample. RAW_VALID without coordinates leaves range.
 * The same explicit timeout releases a stalled stream. No raw_distance inference. */
int piano_pen_owner_enable_input(struct piano_pen_owner *owner);
struct piano_pen_input_area {
    int32_t x_max, y_max, x_resolution, y_resolution;
    int tilt_supported;
};
enum piano_pen_input_state {
    PIANO_PEN_PROXIMITY_KNOWN = 1,
    PIANO_PEN_CONTACT_KNOWN = 2
};
int piano_pen_owner_input_open(struct piano_pen_owner *owner,
    const struct piano_pen_input_area *area);
int piano_pen_owner_input_report(struct piano_pen_owner *owner,
    const struct piano_pen_output *output, uint32_t known,
    int proximity, int contact, int32_t mapped_x, int32_t mapped_y);
#ifdef __cplusplus
}
#endif
#endif
