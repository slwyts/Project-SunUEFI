// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#define PIANO_UFS_UCD_BYTES 1024U
#define PIANO_UFS_RESPONSE_OFFSET 64U
EFI_STATUS PianoUfsBuildNop(VOID *Utrd,UINTN UtrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 UcdIova,UINT8 Tag);
EFI_STATUS PianoUfsBuildReadDescriptor(VOID *Utrd,UINTN UtrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 UcdIova,
                                      UINT8 Tag,UINT8 Idn,UINT8 Index,UINT16 Bytes);
EFI_STATUS PianoUfsCheckNop(CONST VOID *Utrd,CONST VOID *Ucd,UINT8 Tag);
// Sole attribute diagnostic: READ bCurrentPowerMode (IDN 2), available even
// while asleep. No arbitrary attribute or attribute-write constructor.
EFI_STATUS PianoUfsBuildReadPowerMode(VOID *Utrd,UINTN UtrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 UcdIova,UINT8 Tag);
// Resume prerequisite only: START STOP UNIT Active on device WLUN D0,
// expected length zero, no PRDT and no DATA OUT. Not a block write interface.
EFI_STATUS PianoUfsBuildResumeActive(VOID *Utrd,UINTN UtrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 UcdIova,UINT8 Tag);
typedef enum {PianoUfsReportLuns,PianoUfsReadCapacity16,PianoUfsReadLba10} PIANO_UFS_READ_COMMAND;
EFI_STATUS PianoUfsBuildReadCommand(VOID *Utrd,UINTN UtrdBytes,VOID *Ucd,UINTN UcdBytes,
                                  UINT64 UcdIova,UINT64 DataIova,UINT32 DataBytes,
                                  UINT8 Tag,UINT8 Lun,PIANO_UFS_READ_COMMAND Command,UINT32 Lba,UINT16 Blocks);
EFI_STATUS PianoUfsCheckReadResponse(CONST VOID *Utrd,CONST VOID *Ucd,UINT8 Tag,UINT32 Requested,UINT32 *Transferred);
EFI_STATUS PianoUfsParseLuns(CONST VOID *Data,UINTN Bytes,UINT8 Luns[8],UINTN *Count);
EFI_STATUS PianoUfsParseCapacity(CONST VOID *Data,UINTN Bytes,UINT64 *LastLba,UINT32 *BlockBytes);
