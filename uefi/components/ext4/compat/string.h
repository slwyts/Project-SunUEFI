// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#define memcpy PianoExt4Memcpy
#define memmove PianoExt4Memmove
#define memset PianoExt4Memset
#define memcmp PianoExt4Memcmp
#define strlen PianoExt4Strlen
#define strcmp PianoExt4Strcmp
#define strncmp PianoExt4Strncmp
#define strcpy PianoExt4Strcpy
#define strncpy PianoExt4Strncpy
void *memcpy(void *,const void *,size_t);
void *memmove(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
size_t strlen(const char *);
int strcmp(const char *,const char *);
int strncmp(const char *,const char *,size_t);
char *strcpy(char *,const char *);
char *strncpy(char *,const char *,size_t);
