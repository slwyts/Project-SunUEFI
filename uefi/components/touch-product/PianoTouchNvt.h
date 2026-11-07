// SPDX-License-Identifier: BSD-2-Clause-Patent
// NT36532 host-download formats from the product Linux 7.3 driver
// (drivers/input/touchscreen/nt36532e); no hardware access
#pragma once
#include <Uefi.h>

#define PIANO_NVT_TRANSFER_LEN (63U*1024U)
#define PIANO_NVT_READ_LEN (8U*1024U)
#define PIANO_NVT_HEADER_LEN 257U
#define PIANO_NVT_DEFAULT_PAYLOAD 5160U
#define PIANO_NVT_MAX_PAYLOAD (PIANO_NVT_READ_LEN-PIANO_NVT_HEADER_LEN-1U)
#define PIANO_NVT_MAX_PARTITIONS 32U
#define PIANO_NVT_ENG_RST_ADDR 0x7FFF80U
#define PIANO_NVT_EVENT_RESET_COMPLETE 0x60U
#define PIANO_NVT_EVENT_HOST_CMD 0x50U
#define PIANO_NVT_EVENT_FWINFO 0x78U

typedef struct {UINT32 BinAddr,SramAddr,Size,Crc;} PIANO_NVT_PARTITION;
typedef struct {
  UINT32 NeedWriteSize,Count;
  BOOLEAN Cascade2ndHeader;
  UINT8 FwVer;
  PIANO_NVT_PARTITION Part[PIANO_NVT_MAX_PARTITIONS];
} PIANO_NVT_IMAGE;
// update_firmware_request + nvt_bin_header_parser for a HW-CRC chip
EFI_STATUS PianoNvtParseImage(IN CONST UINT8 *Data,IN UINTN Bytes,OUT PIANO_NVT_IMAGE *Image);
// The driver's Crc32(): (len + 4) / 4 little-endian words
UINT32 PianoNvtCrc32(IN CONST UINT8 *Data,IN UINT32 Length);

typedef struct {
  UINT32 EventBuf,TxAutoCopyEn,BootRdy,IlmDes,IlmLength,GIlmChecksum,DlmDes,DlmLength,GDlmChecksum,PollInfo;
} PIANO_NVT_MAP;
typedef struct {UINT32 ChipVerTrim,SwrstSif;} PIANO_NVT_HW_REGS;
// The hw_reg_addr_info tables nvt_ts_check_chip_ver_trim_loop tries, in order
extern CONST PIANO_NVT_HW_REGS gPianoNvtHwRegs[3];
#define PIANO_NVT_ENB_CASC_ADDR 0x1FB12CU
// trim_id_table rows 0 and 1 (NT36532); -1 when neither matches
INTN PianoNvtMatchTrim(IN CONST UINT8 Id[6]);
VOID PianoNvtMemoryMap(IN INTN Row,IN BOOLEAN Cascade,OUT PIANO_NVT_MAP *Map);
