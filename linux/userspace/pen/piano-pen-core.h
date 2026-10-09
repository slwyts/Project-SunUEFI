/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PIANO_PEN_CORE_H
#define PIANO_PEN_CORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct piano_pen_core;
enum piano_pen_orientation { PIANO_PEN_PORTRAIT = 1 };
enum piano_pen_valid {
    PIANO_PEN_RAW_VALID = 1,
    PIANO_PEN_COORDINATES_VALID = 2,
    PIANO_PEN_TILT_VALID = 4,
    PIANO_PEN_PRESSURE_VALID = 8,
    PIANO_PEN_RAW_DISTANCE_VALID = 16,
    PIANO_PEN_MUTUAL_COMPLETE = 32
};
struct piano_pen_info {
    int32_t orientation, vendor_id, resolution;
    int32_t x_max, y_max, pressure_max, tilt_min, tilt_max;
    int32_t pitch_x, pitch_y, calibration_threshold, calibration_rate;
    int32_t mapping_x_first, mapping_x_last, mapping_y_first, mapping_y_last;
};
struct piano_pen_output {
    uint64_t timestamp_ns, pressure_age_ns;
    uint32_t valid;
    int32_t x, y, pressure, raw_distance, tilt_x, tilt_y;
    uint16_t frame_no, raw_pressure;
    int32_t frequency_request;
};

/* Callers select the actual Touch LCDid profile, not the display label,
 * and P81c vendor3. Both the factory ini and format_version=1 numeric facts
 * (mapping.mapping_40 / mapping.mapping_60) are accepted. Coordinates are portrait
 * native units, not screen pixels. Unknown calibration is rejected.
 * pressure_max_age_ns is an explicit caller freshness limit, not an OEM value. */
struct piano_pen_core *piano_pen_core_create(const char *ini, int vendor_id,
    enum piano_pen_orientation orientation, uint64_t pressure_max_age_ns,
    char *error, size_t error_size);
void piano_pen_core_destroy(struct piano_pen_core *core);
void piano_pen_core_reset(struct piano_pen_core *core);
int piano_pen_core_info(const struct piano_pen_core *core, struct piano_pen_info *info);

/* BLE report5 must be the actual15-byte report including ID, not raw29
 * metadata or a made-up common record. Both timestamps must use CLOCK_BOOTTIME:
 * the NTP driver timestamps frames with ktime_get_boottime(). Mixing them with
 * CLOCK_MONOTONIC becomes incorrect across suspend. */
int piano_pen_core_report5(struct piano_pen_core *core, const uint8_t *report,
    size_t bytes, uint64_t timestamp_ns, char *error, size_t error_size);
/* payload begins at original frame_data_packet, without the257-byte transport
 * or stream header. This API neither owns the FIFO nor creates input devices.
 * Fields without their validity bit are unknown and must not be injected. */
int piano_pen_core_process29(struct piano_pen_core *core, const uint8_t *payload,
    size_t bytes, uint64_t timestamp_ns, struct piano_pen_output *output,
    char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
#endif
