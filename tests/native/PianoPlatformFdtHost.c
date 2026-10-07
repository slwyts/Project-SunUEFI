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
INTN EFIAPI FdtGetNumberOfReserveMapEntries(CONST VOID *F){return fdt_num_mem_rsv(F);}
INTN EFIAPI FdtGetReserveMapEntry(CONST VOID *F,INTN N,UINT64 *B,UINT64 *S){uint64_t Base,Size;int Status=fdt_get_mem_rsv(F,N,&Base,&Size);if(!Status){*B=Base;*S=Size;}return Status;}
