/* SPDX-License-Identifier: MIT */
#ifndef PIANO_QUAD_H
#define PIANO_QUAD_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct piano_quad;
enum piano_quad_cfa { PIANO_QUAD_RGGB, PIANO_QUAD_BGGR, PIANO_QUAD_GRBG, PIANO_QUAD_GBRG };
struct piano_quad_params {
    float black_level, digital_gain;
    float white_balance[3], ccm[9];
    float saturation, contrast, gamma;
};
struct piano_quad_metrics {
    uint64_t unpack_ns, demosaic_ns, color_ns, working_bytes;
};

/* One caller serializes the handle. CFA describes 2x2 same-color cells, not
 * ordinary Bayer. Dimensions are retained, up to the device's 8160x6120.
 * Input is MIPI RAW10 (4 pixels/5 bytes), using the actual capture stride.
 * Output is tightly packed BT.601 full-range NV12 of width*height*3/2 bytes.
 * Buffers must not overlap the handle; no pointer is retained. Return is 0 or -errno.
 */
int piano_quad_create(uint32_t width, uint32_t height, uint32_t cfa,
                      struct piano_quad **output);
void piano_quad_params_default(struct piano_quad_params *params);
/* Unpack freezes a copy of the controls and owns every raw sample. Requeue
 * the capture DMA buffer immediately after unpack, then call render. Render
 * consumes one prepared frame, preventing stale output on a second call.
 */
int piano_quad_unpack(struct piano_quad *converter, const uint8_t *raw,
                      size_t raw_bytes, size_t stride,
                      const struct piano_quad_params *params);
int piano_quad_render(struct piano_quad *converter, uint8_t *output,
                      size_t output_bytes, struct piano_quad_metrics *metrics);
void piano_quad_destroy(struct piano_quad *converter);

/* Defaults: no guessed black offset, unity gain/WB/CCM, saturation1,
 * contrast96/256, gamma2.2. The owner supplies its accepted camera controls:
 * gain/WB Q10 -> /1024, CCM Q10 -> /1024, saturation percent -> /100,
 * contrast share -> /256. Read exposure/analog gain continue in the sensor.
 */
#ifdef __cplusplus
}
#endif
#endif
