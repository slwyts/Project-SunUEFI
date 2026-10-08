/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_PEN_FRAME_H
#define PIANO_PEN_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define PIANO_PEN_INTERNAL_BYTES 0x511
#define PIANO_PEN_MAX_PACKET (8192 - 257 - 1)

/* The Novatek v2 configuration uses these four ini values and the actual
 * hardware rx*tx count. No geometry is inferred from an absent pen frame.
 */
struct piano_pen_geometry {
    uint8_t col1, row1, col2, row2;
    uint16_t nodes;
};

/* Only the pen prefix and its main-touch quarter accumulator are reconstructed.
 * Other HAL fields, including SC pointers at0x84/0x8c, are not provided.
 * Do not pass this object to alg_compute_points_core/parse_data_package.
 * frame and embedded pointers remain valid until next decode or destroy.
 */
struct piano_pen_frame {
    struct piano_pen_geometry geometry;
    unsigned char frame[PIANO_PEN_INTERNAL_BYTES];
    int16_t *matrices[4];
    int16_t *hand, *hand_parts;
    size_t n1, n2;
    unsigned packet_sum, part_mask;
    size_t outer_end, pen_end;
    int frequency_request; /* -1 or actual metadata frequency byte */
};

int piano_pen_frame_init(struct piano_pen_frame *,
                         const struct piano_pen_geometry *);
void piano_pen_frame_destroy(struct piano_pen_frame *);

/* Input is the original SPI frame_data_packet, with record/SPI prefixes already
 * removed. Both real additive checksums/complements are required; Android mmap
 * CRC-rewritten input and type17 are deliberately not normalized or bypassed.
 * A non-NULL error explains any rejected original packet.
 */
int piano_pen_frame_decode(struct piano_pen_frame *, const void *, size_t,
                           const char **error);

#endif
