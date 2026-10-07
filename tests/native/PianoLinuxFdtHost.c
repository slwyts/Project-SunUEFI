// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/FdtLib.h>
#include <Library/DebugLib.h>
#undef FDT_TAGSIZE
#include <libfdt.h>
INT32 EFIAPI FdtCheckHeader(CONST VOID*F){return fdt_check_header(F);}UINT32 EFIAPI Fdt32ToCpu(UINT32 V){return __builtin_bswap32(V);}
INT32 EFIAPI FdtOpenInto(CONST VOID*F,VOID*B,INT32 N){return fdt_open_into(F,B,N);}INT32 EFIAPI FdtPathOffset(CONST VOID*F,CONST CHAR8*P){return fdt_path_offset(F,P);}
INT32 EFIAPI FdtDelProp(VOID*F,INT32 N,CONST CHAR8*P){return fdt_delprop(F,N,P);}INT32 EFIAPI FdtSetProp(VOID*F,INT32 N,CONST CHAR8*P,CONST VOID*D,UINT32 B){return fdt_setprop(F,N,P,D,B);}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *F,INT32 N,CONST CHAR8 *P,INT32 *Bytes){return fdt_getprop(F,N,P,Bytes);}
INT32 EFIAPI FdtNodeOffsetByCompatible(CONST VOID *F,INT32 Start,CONST CHAR8 *Name){return fdt_node_offset_by_compatible(F,Start,Name);}
// These lifecycle fixtures have no factory SEC HOB. The actual cold producer
// and panel selector are checked together separately, with the real DTB input.
__attribute__((weak)) UINT32 PianoProductBootObjectsFactoryPanel(VOID){return 0;}
__attribute__((weak)) BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
__attribute__((weak)) BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level){(VOID)Level;return FALSE;}
__attribute__((weak)) VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;(VOID)Format;}
