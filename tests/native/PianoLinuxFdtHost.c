// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/FdtLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#undef FDT_TAGSIZE
#include <libfdt.h>
INT32 EFIAPI FdtCheckHeader(CONST VOID*F){return fdt_check_header(F);}UINT32 EFIAPI Fdt32ToCpu(UINT32 V){return __builtin_bswap32(V);}
INT32 EFIAPI FdtOpenInto(CONST VOID*F,VOID*B,INT32 N){return fdt_open_into(F,B,N);}INT32 EFIAPI FdtPathOffset(CONST VOID*F,CONST CHAR8*P){return fdt_path_offset(F,P);}
INT32 EFIAPI FdtDelProp(VOID*F,INT32 N,CONST CHAR8*P){return fdt_delprop(F,N,P);}INT32 EFIAPI FdtSetProp(VOID*F,INT32 N,CONST CHAR8*P,CONST VOID*D,UINT32 B){return fdt_setprop(F,N,P,D,B);}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *F,INT32 N,CONST CHAR8 *P,INT32 *Bytes){return fdt_getprop(F,N,P,Bytes);}
INT32 EFIAPI FdtNodeOffsetByCompatible(CONST VOID *F,INT32 Start,CONST CHAR8 *Name){return fdt_node_offset_by_compatible(F,Start,Name);}
INT32 EFIAPI FdtPack(VOID *F){return fdt_pack(F);}
__attribute__((weak)) INT32 EFIAPI FdtSubnodeOffset(CONST VOID *F,INT32 N,CONST CHAR8 *Name){return fdt_subnode_offset(F,N,Name);}
__attribute__((weak)) INT32 EFIAPI FdtAddSubnode(VOID *F,INT32 N,CONST CHAR8 *Name){return fdt_add_subnode(F,N,Name);}
__attribute__((weak)) INT32 EFIAPI FdtNodeOffsetByPhandle(CONST VOID *F,UINT32 P){return fdt_node_offset_by_phandle(F,P);}
__attribute__((weak)) UINT32 EFIAPI FdtGetPhandle(CONST VOID *F,INT32 N){return fdt_get_phandle(F,N);}
__attribute__((weak)) INT32 EFIAPI FdtFindMaxPhandle(CONST VOID *F,UINT32 *P){return fdt_find_max_phandle(F,P);}
__attribute__((weak)) UINT64 EFIAPI SwapBytes64(UINT64 Value){return __builtin_bswap64(Value);}
__attribute__((weak)) UINTN EFIAPI AsciiSPrint(CHAR8 *Out,UINTN Bytes,CONST CHAR8 *Format,...){
 va_list Args;va_start(Args,Format);int Count=vsnprintf(Out,Bytes,Format,Args);va_end(Args);
 return Count<0?0:(UINTN)Count;
}
// These lifecycle fixtures have no factory SEC HOB. The actual cold producer
// and panel selector are checked together separately, with the real DTB input.
__attribute__((weak)) UINT32 PianoProductBootObjectsFactoryPanel(VOID){return 0;}
__attribute__((weak)) INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
__attribute__((weak)) UINTN EFIAPI AsciiStrLen(CONST CHAR8 *Text){return strlen(Text);}
__attribute__((weak)) UINT32 EFIAPI SwapBytes32(UINT32 Value){return __builtin_bswap32(Value);}
__attribute__((weak)) BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
__attribute__((weak)) BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level){(VOID)Level;return FALSE;}
__attribute__((weak)) VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;(VOID)Format;}
