// Host wrapper around the pinned real libfdt implementation.
#include <Uefi.h>
#include <Library/FdtLib.h>
#undef FDT_TAGSIZE
#include <libfdt.h>
INT32 EFIAPI FdtCheckHeader(CONST VOID *F){return fdt_check_header(F);}
UINT32 EFIAPI Fdt32ToCpu(UINT32 V){return __builtin_bswap32(V);}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *F,INT32 N,CONST CHAR8 *P,INT32 *L){return fdt_getprop(F,N,P,L);}
INT32 EFIAPI FdtFirstSubnode(CONST VOID *F,INT32 N){return fdt_first_subnode(F,N);}
INT32 EFIAPI FdtNextSubnode(CONST VOID *F,INT32 N){return fdt_next_subnode(F,N);}
INT32 EFIAPI FdtPathOffset(CONST VOID *F,CONST CHAR8 *P){return fdt_path_offset(F,P);}
CONST CHAR8 *EFIAPI FdtGetName(CONST VOID *F,INT32 N,INT32 *L){return fdt_get_name(F,N,L);}
INT32 EFIAPI FdtOpenInto(CONST VOID *F,VOID *B,INT32 N){return fdt_open_into(F,B,N);}
INT32 EFIAPI FdtAddSubnode(VOID *F,INT32 N,CONST CHAR8 *P){return fdt_add_subnode(F,N,P);}
INT32 EFIAPI FdtDelNode(VOID *F,INT32 N){return fdt_del_node(F,N);}
INT32 EFIAPI FdtSetProp(VOID *F,INT32 N,CONST CHAR8 *P,CONST VOID *D,UINT32 B){return fdt_setprop(F,N,P,D,B);}
INT32 EFIAPI FdtDelProp(VOID *F,INT32 N,CONST CHAR8 *P){return fdt_delprop(F,N,P);}
INT32 EFIAPI FdtNodeOffsetByCompatible(CONST VOID *F,INT32 N,CONST CHAR8 *P){return fdt_node_offset_by_compatible(F,N,P);}
INT32 EFIAPI FdtPack(VOID *F){return fdt_pack(F);}
INTN EFIAPI FdtGetNumberOfReserveMapEntries(CONST VOID *F){return fdt_num_mem_rsv(F);}
INTN EFIAPI FdtGetReserveMapEntry(CONST VOID *F,INTN N,UINT64 *B,UINT64 *S){uint64_t Base,Size;int Status=fdt_get_mem_rsv(F,N,&Base,&Size);if(!Status){*B=Base;*S=Size;}return Status;}
