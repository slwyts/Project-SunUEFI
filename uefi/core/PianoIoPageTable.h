// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoDma.h"
#define PIANO_IOVA_BASE 0x40000000ULL
#define PIANO_IOVA_BYTES 0x01000000U
#define PIANO_IO_PT_BYTES (10U*4096U)
typedef struct {UINT64 *Tables;EFI_PHYSICAL_ADDRESS Physical;} PIANO_IO_PAGE_TABLE;
EFI_STATUS PianoIoPageTableInit(PIANO_IO_PAGE_TABLE *Table,VOID *Memory,EFI_PHYSICAL_ADDRESS Physical,UINTN Bytes);
EFI_STATUS PianoIoPageTableMap(PIANO_IO_PAGE_TABLE *Table,UINT64 Iova,UINT64 Physical,UINTN Bytes,PIANO_DMA_DIRECTION Direction);
EFI_STATUS PianoIoPageTableUnmap(PIANO_IO_PAGE_TABLE *Table,UINT64 Iova,UINTN Bytes);
EFI_STATUS PianoIoPageTableTranslate(PIANO_IO_PAGE_TABLE *Table,UINT64 Iova,BOOLEAN Write,UINT64 *Physical);
