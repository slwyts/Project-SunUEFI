/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Pinned HAL0cc145e5...: novatek_operation_v2 decode0x30718/core0x2fe44.
 * Pointer layout is confirmed by actual ELF relocations and copies. See
 * docs/devel/piano-pen-protocol.md for scope and original MiCode references.
 * This is CPU-only: no HAL startup, ioctl, frequency command or frame source.
 */
#include "piano-pen-frame.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t)==8,"factory internal pen pointers are64-bit");

static uint16_t le16(const unsigned char *p)
{ return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put_pointer(unsigned char *dst, const void *pointer)
{
    /* HAL pointers are intentionally unaligned. Never cast dst to void **. */
    uintptr_t value=(uintptr_t)pointer;
    memcpy(dst,&value,sizeof(value));
}
static int reject(const char **error, const char *message)
{ if (error) *error=message; return EINVAL; }

static int checksum(const unsigned char *packet, size_t bytes, size_t header,
                    size_t required_end, size_t maximum_end, size_t *end)
{
    if (header > bytes || bytes-header < 20) return 0;
    const unsigned char *h=packet+header;
    uint32_t count=le32(h+8);
    if (!count || count > INT32_MAX || count > (bytes-20)/4) return 0;
    size_t limit=20+(size_t)count*4;
    if (limit < required_end || limit > maximum_end) return 0;
    uint16_t expected=le16(h+4);
    if (le16(h+12)!=(uint16_t)~expected || le32(h+16)!=~count) return 0;
    uint32_t sum=0;
    for (size_t i=20;i<limit;i+=2) sum+=le16(packet+i);
    if ((uint16_t)(0u-sum)!=expected) return 0;
    *end=limit;return 1;
}

int piano_pen_frame_init(struct piano_pen_frame *output,
                         const struct piano_pen_geometry *geometry)
{
    if (!output || !geometry) return EINVAL;
    memset(output,0,sizeof(*output));
    size_t n1=(size_t)geometry->col1*geometry->row1;
    size_t n2=(size_t)geometry->col2*geometry->row2;
    /* ALG stylus_total_data has two28800-byte s32 matrix regions. */
    if (!n1 || !n2 || n1+n2>7200 || !geometry->nodes ||
        geometry->nodes>7200 || geometry->nodes%4) return EINVAL;
    output->geometry=*geometry;output->n1=n1;output->n2=n2;
    output->frequency_request=-1;
    for (unsigned i=0;i<4;i++) {
        output->matrices[i]=malloc((i%2?n2:n1)*sizeof(int16_t));
        if (!output->matrices[i]) goto fail;
    }
    output->hand=calloc(geometry->nodes,sizeof(int16_t));
    output->hand_parts=calloc(geometry->nodes,sizeof(int16_t));
    if (!output->hand || !output->hand_parts) goto fail;
    return 0;
fail:
    piano_pen_frame_destroy(output);return ENOMEM;
}

void piano_pen_frame_destroy(struct piano_pen_frame *output)
{
    if (!output) return;
    for (unsigned i=0;i<4;i++) free(output->matrices[i]);
    free(output->hand);free(output->hand_parts);
    memset(output,0,sizeof(*output));
}

int piano_pen_frame_decode(struct piano_pen_frame *output, const void *input,
                           size_t bytes, const char **error)
{
    if (error) *error=NULL;
    if (!output || !input || !output->hand_parts)
        return reject(error,"decoder has not been allocated");
    if (bytes<100 || bytes>PIANO_PEN_MAX_PACKET)
        return reject(error,"original SPI packet length outside capture bounds");
    const unsigned char *packet=input, *metadata=packet+64;
    if (packet[56]!=29)
        return reject(error,"Novatek v2 pen adapter requires actual type29; type17 is not converted");
    const struct piano_pen_geometry *g=&output->geometry;
    if (metadata[7]!=g->col1 || metadata[8]!=g->row1 ||
        metadata[9]!=g->col2 || metadata[10]!=g->row2)
        return reject(error,"pen frame dimensions differ from actual HAL ini");
    if ((size_t)packet[48]*packet[49]!=g->nodes)
        return reject(error,"main-touch node count differs from actual hardware config");
    size_t quarter=g->nodes/4, quarter_bytes=quarter*2;
    size_t trailer=100+4*(output->n1+output->n2);
    size_t hand_start=trailer+20, required=hand_start+quarter_bytes;
    if (required>bytes)
        return reject(error,"four pen matrices, trailer or hand quarter truncated");
    unsigned part=metadata[11];
    if (part<1 || part>4 || le16(metadata+12)!=quarter_bytes)
        return reject(error,"hand quarter number or length does not match v2 copy size");
    size_t outer_end, pen_end;
    if (!checksum(packet,bytes,0,required,bytes,&outer_end))
        return reject(error,"outer original additive checksum, complement or length failed");
    /* MiCode17/29 trailer advertises a second checksum from packet+20.
     * Its extent must cover all four arrays but stop before its checksum word.
     */
    if (!checksum(packet,bytes,trailer,trailer,trailer+4,&pen_end))
        return reject(error,"pen original additive checksum, complement or length failed");

    memset(output->frame,0,sizeof(output->frame));
    memcpy(output->frame+0x3c,packet,64);
    memcpy(output->frame+0x10d,metadata,36);
    static const size_t pointers[4]={0x131,0x139,0x141,0x149};
    size_t cursor=100;
    for (unsigned i=0;i<4;i++) {
        size_t elements=i%2?output->n2:output->n1;
        for (size_t j=0;j<elements;j++)
            output->matrices[i][j]=(int16_t)le16(packet+cursor+j*2);
        put_pointer(output->frame+pointers[i],output->matrices[i]);
        cursor+=elements*2;
    }
    memcpy(output->frame+0x151,packet+trailer,20);
    output->frame[0x166]=1;output->frame[0x167]=1;
    output->frame[0x16a]=packet[24]==4;
    put_pointer(output->frame+0x7c,output->hand);
    /* HAL30040..300c4 accumulates quarter indices and accepts sum10 on part4.
     * Track actual populated quarters too: sum10 alone does not prove all
     * buffers were filled (for example2,2,2,4). Never publish missing bytes.
     */
    size_t offset=(part-1)*quarter;
    for (size_t i=0;i<quarter;i++)
        output->hand_parts[offset+i]=(int16_t)le16(packet+hand_start+i*2);
    output->packet_sum+=part;output->part_mask|=1u<<(part-1);
    if (part==4) {
        if (output->packet_sum==10 && output->part_mask==15) {
            memcpy(output->hand,output->hand_parts,g->nodes*sizeof(int16_t));
            output->frame[0x165]=1;
        }
        output->packet_sum=0;output->part_mask=0;
    } else if (output->packet_sum>10) {
        /* A damaged/missing sequence cannot grow the accumulator unbounded. */
        output->packet_sum=0;output->part_mask=0;
    }
    output->outer_end=outer_end;output->pen_end=pen_end;
    output->frequency_request=metadata[17]<=63 && (metadata[17]&15)<=3?
        metadata[17]:-1;
    return 0;
}
