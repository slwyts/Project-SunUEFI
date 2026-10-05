#ifndef PIANO_EFI_HANDOFF_TRACE_TEST_SHIM_H
#define PIANO_EFI_HANDOFF_TRACE_TEST_SHIM_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
typedef void VOID;
typedef char CHAR8;
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef uintptr_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint8_t BOOLEAN;
#define STATIC static
#define CONST const
#define EFIAPI
#define TRUE 1
#define FALSE 0
#define OFFSET_OF(T,F) offsetof(T,F)
#define EFI_SUCCESS 0
#define EFI_ERROR(S) (((S) >> 63) != 0)
#define EFI_INVALID_PARAMETER ((EFI_STATUS)(1ULL << 63) | 2)
#define EFI_UNSUPPORTED ((EFI_STATUS)(1ULL << 63) | 3)
#define EFI_BAD_BUFFER_SIZE ((EFI_STATUS)(1ULL << 63) | 4)
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)(1ULL << 63) | 5)
#define EFI_NOT_READY ((EFI_STATUS)(1ULL << 63) | 6)
#define EFI_ACCESS_DENIED ((EFI_STATUS)(1ULL << 63) | 15)
#define EFI_NOT_STARTED ((EFI_STATUS)(1ULL << 63) | 19)
#define EFI_ALREADY_STARTED ((EFI_STATUS)(1ULL << 63) | 20)
#define EFI_CRC_ERROR ((EFI_STATUS)(1ULL << 63) | 27)
#define EFI_BOOT_SERVICES_SIGNATURE 0x56524553544f4f42ULL
typedef struct { UINT64 Signature; UINT32 Revision, HeaderSize, CRC32, Reserved; } EFI_TABLE_HEADER;
typedef struct { UINT32 Type, Pad; UINT64 PhysicalStart, VirtualStart, NumberOfPages, Attribute; } EFI_MEMORY_DESCRIPTOR;
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, UINT32 *);
typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE, UINTN);
typedef struct { EFI_TABLE_HEADER Hdr; EFI_GET_MEMORY_MAP GetMemoryMap; EFI_EXIT_BOOT_SERVICES ExitBootServices; } EFI_BOOT_SERVICES;
#define CopyMem(D,S,N) memcpy(D,S,N)
#define ZeroMem(D,N) memset(D,0,N)
static inline UINTN AsciiSPrint(CHAR8 *Out, UINTN Size, CONST CHAR8 *Format, ...) {
  va_list Arguments;va_start(Arguments,Format);int Count=vsnprintf(Out,Size,Format,Arguments);va_end(Arguments);
  return Count<0?0:(UINTN)Count>=Size?Size-1:(UINTN)Count;
}
UINTN SerialPortWrite(UINT8 *Buffer,UINTN Size);
#endif
