/* SPDX-License-Identifier: MIT */
#include "piano-quad.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
_Static_assert(sizeof(struct piano_quad_params)==68,"C/Rust parameter ABI");
_Static_assert(sizeof(struct piano_quad_metrics)==32,"C/Rust metric ABI");
int main(void)
{
    struct piano_quad *converter=NULL;
    struct piano_quad_params params;
    struct piano_quad_metrics metrics;
    unsigned char raw[8*16], output[8*8*3/2];
    memset(raw,255,sizeof(raw));
    piano_quad_params_default(&params);
    assert(piano_quad_create(8,8,PIANO_QUAD_GRBG,&converter)==0 && converter);
    assert(piano_quad_unpack(converter,raw,sizeof(raw),16,&params)==0);
    memset(raw,0,sizeof(raw));
    assert(piano_quad_render(converter,output,sizeof(output),&metrics)==0);
    for(unsigned int i=0;i<64;i++) assert(output[i]==255);
    for(unsigned int i=64;i<sizeof(output);i++) assert(output[i]==128);
    assert(piano_quad_render(converter,output,sizeof(output),NULL)==-EINVAL);
    piano_quad_destroy(converter);
    return 0;
}
