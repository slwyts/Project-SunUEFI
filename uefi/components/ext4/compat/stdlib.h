// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#define malloc PianoExt4Malloc
#define calloc PianoExt4Calloc
#define realloc PianoExt4Realloc
#define free PianoExt4Free
#define qsort PianoExt4Qsort
void *malloc(size_t);
void *calloc(size_t,size_t);
void *realloc(void *,size_t);
void free(void *);
void qsort(void *,size_t,size_t,int (*)(const void *,const void *));
