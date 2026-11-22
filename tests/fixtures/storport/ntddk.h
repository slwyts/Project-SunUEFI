/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef void VOID, *PVOID, *PDRIVER_OBJECT, *PUNICODE_STRING;
typedef uint8_t UCHAR, BOOLEAN, *PUCHAR, *PBOOLEAN;
typedef uint16_t USHORT;
typedef uint32_t ULONG, *PULONG;
typedef int32_t LONG, NTSTATUS;
typedef uint64_t ULONGLONG;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
typedef char *PCHAR;
typedef struct { int64_t QuadPart; } LARGE_INTEGER, STOR_PHYSICAL_ADDRESS;
typedef struct { uint32_t a; uint16_t b,c; uint8_t d[8]; } GUID;
typedef struct { USHORT Size,Version; PVOID Context; VOID (*InterfaceReference)(PVOID),(*InterfaceDereference)(PVOID); }
    INTERFACE;
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define PAGE_SIZE 4096
#define STATUS_INVALID_PARAMETER (-1)
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define _IRQL_requires_(x)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RTL_SIZEOF_THROUGH_FIELD(t,m) (offsetof(t,m)+sizeof(((t *)0)->m))
#define RtlCopyMemory memcpy
#define RtlZeroMemory(a,n) memset((a),0,(n))
#define InterlockedExchange(p,v) __atomic_exchange_n((p),(v),__ATOMIC_SEQ_CST)
#define InterlockedIncrement(p) __atomic_add_fetch((p),1,__ATOMIC_SEQ_CST)
static inline LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG expected)
{
    __atomic_compare_exchange_n(p,&expected,v,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    return expected;
}
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
