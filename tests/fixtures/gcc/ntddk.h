/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "../storport/ntddk.h"
#include <assert.h>
#include <pthread.h>

typedef unsigned KIRQL;
typedef pthread_mutex_t KMUTEX, KSPIN_LOCK, *PKSPIN_LOCK;
typedef struct {
    unsigned Type, ShareDisposition, Flags;
    ULONGLONG start, length;
} CM_PARTIAL_RESOURCE_DESCRIPTOR, *PCM_PARTIAL_RESOURCE_DESCRIPTOR;

enum {
    CmResourceTypeMemory = 3, CmResourceTypeMemoryLarge = 7,
    CmResourceShareDeviceExclusive = 1, CmResourceShareShared = 3,
    CM_RESOURCE_MEMORY_READ_ONLY = 1, CM_RESOURCE_MEMORY_WRITE_ONLY = 2,
    KernelMode = 0, Executive = 0, PASSIVE_LEVEL = 0, DISPATCH_LEVEL = 2,
    STATUS_SUCCESS = 0, STATUS_DEVICE_NOT_READY = -2, STATUS_DEVICE_REMOVED = -3,
    STATUS_DEVICE_NOT_CONNECTED = -4, STATUS_DEVICE_HARDWARE_ERROR = -5,
    STATUS_IO_TIMEOUT = -6, STATUS_DEVICE_CONFIGURATION_ERROR = -7, STATUS_INTEGER_OVERFLOW = -8,
    STATUS_INVALID_DEVICE_STATE = -9, STATUS_DEVICE_BUSY = -10,
};
#define MAXLONGLONG INT64_MAX
#define PAGED_CODE() assert(gcc_test_irql == PASSIVE_LEVEL)

extern _Thread_local KIRQL gcc_test_irql;
void gcc_test_lock(pthread_mutex_t *lock);
void KeStallExecutionProcessor(ULONG us);
ULONG READ_REGISTER_ULONG(volatile ULONG *reg);
void WRITE_REGISTER_ULONG(volatile ULONG *reg, ULONG value);

static inline PVOID InterlockedExchangePointer(PVOID volatile *p, PVOID v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
static inline PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID v, PVOID expected)
{
    __atomic_compare_exchange_n(p, &expected, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static inline ULONGLONG RtlCmDecodeMemIoResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR r, ULONGLONG *start)
{
    *start = r->start;
    return r->length;
}
static inline void KeInitializeMutex(KMUTEX *lock, unsigned level)
{
    assert(!level && !pthread_mutex_init(lock, NULL));
}
static inline void KeWaitForSingleObject(KMUTEX *lock, int reason, int mode, int alertable, void *timeout)
{
    assert(gcc_test_irql == PASSIVE_LEVEL && reason == Executive && mode == KernelMode && !alertable && !timeout);
    gcc_test_lock(lock);
}
static inline void KeReleaseMutex(KMUTEX *lock, int wait)
{
    assert(!wait && !pthread_mutex_unlock(lock));
}
static inline void KeAcquireSpinLock(PKSPIN_LOCK lock, KIRQL *old)
{
    assert(gcc_test_irql <= DISPATCH_LEVEL);
    *old = gcc_test_irql;
    gcc_test_lock(lock);
    gcc_test_irql = DISPATCH_LEVEL;
}
static inline void KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL old)
{
    assert(gcc_test_irql == DISPATCH_LEVEL && !pthread_mutex_unlock(lock));
    gcc_test_irql = old;
}
