/* SPDX-License-Identifier: BSD-2-Clause */
#include "fixed_pll.h"
#include "piano_base_clock.h"
#include "piano_clock.h"
#include "piano_power.h"
#include <errno.h>

enum { VOTE = 0x52020 / 4, GPLL4 = 0x4000 / 4, L = 4, ALPHA = 5, USER = 6, UFS_BASE = 0x100000 };
#define LOCKED (UINT32_C(1) << 31)
static ULONG first[SM8750_GCC_REQUIRED_BYTES / 4], second[RTL_NUMBER_OF(first)], *mapped;
static unsigned accesses;
static int disk, other;
static SM8750_GCC_PLLS gcc;
static KSPIN_LOCK vote_lock = PTHREAD_MUTEX_INITIALIZER;
_Thread_local KIRQL gcc_test_irql;

/* Hold one emulated MMIO read in flight until the remove thread either
 * blocks on a kernel lock or returns the mapping to its owner. */
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int pause_read, reading, remove_blocked, detached;
static _Thread_local int removing;

void gcc_test_lock(pthread_mutex_t *lock)
{
    if (removing) {
        int result = pthread_mutex_trylock(lock);
        if (!result) return;
        assert(result == EBUSY);
        assert(!pthread_mutex_lock(&gate));
        remove_blocked = 1;
        assert(!pthread_cond_broadcast(&changed));
        assert(!pthread_mutex_unlock(&gate));
    }
    assert(!pthread_mutex_lock(lock));
}

static unsigned word(volatile ULONG *reg)
{
    uintptr_t address = (uintptr_t)reg, base = (uintptr_t)mapped;
    assert(mapped && address >= base && address - base < sizeof(first) && !(address & 3));
    ++accesses;
    return (unsigned)((address - base) / sizeof(*reg));
}

ULONG READ_REGISTER_ULONG(volatile ULONG *reg)
{
    unsigned i = word(reg);
    if (pause_read) {
        assert(!pthread_mutex_lock(&gate));
        reading = 1;
        assert(!pthread_cond_broadcast(&changed));
        while (pause_read) assert(!pthread_cond_wait(&changed, &gate));
        assert(!pthread_mutex_unlock(&gate));
    }
    return mapped[i];
}

void WRITE_REGISTER_ULONG(volatile ULONG *reg, ULONG value)
{
    unsigned i = word(reg);
    if (i == VOTE) {
        assert(gcc_test_irql == DISPATCH_LEVEL);
        mapped[0] = value & 1 ? LOCKED : 0;
        mapped[GPLL4] = value & 16 ? LOCKED : 0;
    }
    mapped[i] = value;
}

void KeStallExecutionProcessor(ULONG usec) { assert(usec == 1); }

static void prepare(void)
{
    CM_PARTIAL_RESOURCE_DESCRIPTOR r = {
        .Type = CmResourceTypeMemory, .ShareDisposition = CmResourceShareDeviceExclusive,
        .start = UINT64_C(0x100100000), .length = SM8750_GCC_REQUIRED_BYTES - 1,
    };
    Sm8750InitializeGcc(&gcc, &vote_lock);
    assert(Sm8750GccPower(&gcc, TRUE, FALSE) == STATUS_DEVICE_NOT_READY);
    assert(Sm8750PrepareGccPlls(&gcc, &r, first) == STATUS_DEVICE_CONFIGURATION_ERROR);
    ++r.length;
    r.Flags = CM_RESOURCE_MEMORY_READ_ONLY;
    assert(Sm8750PrepareGccPlls(&gcc, &r, first) == STATUS_DEVICE_CONFIGURATION_ERROR);
    r.Flags = 0;
    r.ShareDisposition = CmResourceShareShared;
    assert(Sm8750PrepareGccPlls(&gcc, &r, first) == STATUS_DEVICE_CONFIGURATION_ERROR);
    r.ShareDisposition = CmResourceShareDeviceExclusive;
    ++r.start;
    assert(Sm8750PrepareGccPlls(&gcc, &r, first) == STATUS_DEVICE_CONFIGURATION_ERROR);
    --r.start;
    assert(Sm8750PrepareGccPlls(&gcc, &r, (UCHAR *)first + 1) == STATUS_DEVICE_CONFIGURATION_ERROR);
    assert(Sm8750PrepareGccPlls(&gcc, &r, first) == STATUS_SUCCESS);
    assert(Sm8750PrepareGccPlls(&gcc, &r, second) == STATUS_DEVICE_BUSY);
    assert(Sm8750OpenUfsClocks(&gcc, &disk) == STATUS_DEVICE_NOT_READY && !accesses);
    assert(Sm8750GccPower(&gcc, TRUE, FALSE) == STATUS_SUCCESS && !accesses);
    mapped = first;
}

static void check_writable(ULONG bus)
{
    ULONG offset = bus - UFS_BASE;
    Sm8750WriteUfsClock(&gcc, &disk, offset, bus);
    assert(mapped[offset / 4] == bus && Sm8750ReadUfsClock(&gcc, &disk, offset) == bus);
}

static void ufs_registers(void)
{
    assert(Sm8750OpenUfsClocks(&gcc, NULL) == STATUS_INVALID_PARAMETER);
    assert(Sm8750OpenUfsClocks(&gcc, &disk) == STATUS_SUCCESS);
    assert(Sm8750OpenUfsClocks(&gcc, &other) == STATUS_DEVICE_BUSY);
    assert(!Sm8750CloseUfsClocks(&gcc, &other));
    gcc_test_irql = DISPATCH_LEVEL + 1;  /* Half-duplex HwStorStartIo. */
    assert(Sm8750UfsPowerRetained(&gcc, &disk) && !Sm8750UfsPowerRetained(&gcc, &other));
    gcc_test_irql = PASSIVE_LEVEL;
    for (unsigned i = 0; i < PIANO_UFS_CLOCK_REGS; ++i) check_writable(piano_ufs_gcc_address[i]);
    for (unsigned i = 0; i < PIANO_QREF; ++i) check_writable(piano_ufs_base_address[i]);
    for (unsigned i = 0; i < PIANO_UFS_DOMAINS; ++i) {
        ULONG control = piano_ufs_domain_address[i];
        check_writable(control);
        ULONG status = control - UFS_BASE + 4;
        mapped[status / 4] = UINT32_C(1) << 16;
        assert(Sm8750ReadUfsClock(&gcc, &disk, status) == (UINT32_C(1) << 16));
        unsigned before = accesses;
        Sm8750WriteUfsClock(&gcc, &disk, status, 0);
        assert(accesses == before && mapped[status / 4]);
    }
    ULONG reset = piano_ufs_gcc_address[PIANO_UFS_BCR] - UFS_BASE;
    const ULONG excluded[] = {0, 4 * VOTE, 0x52000, reset + 1, reset + 12,
                              piano_ufs_base_address[PIANO_QREF] - UFS_BASE, MAXULONG};
    unsigned before = accesses;
    for (unsigned i = 0; i < RTL_NUMBER_OF(excluded); ++i) {
        assert(Sm8750ReadUfsClock(&gcc, &disk, excluded[i]) == MAXULONG);
        Sm8750WriteUfsClock(&gcc, &disk, excluded[i], 0);
    }
    Sm8750WriteUfsClock(&gcc, &other, reset, 0);
    assert(Sm8750ReadUfsClock(&gcc, &other, reset) == MAXULONG && accesses == before);
    assert(Sm8750CloseUfsClocks(&gcc, &disk) && !Sm8750UfsPowerRetained(&gcc, &disk));
    assert(Sm8750ReadUfsClock(&gcc, &disk, reset) == MAXULONG && accesses == before);
    assert(Sm8750OpenUfsClocks(&gcc, &disk) == STATUS_SUCCESS);
}

static void shared_plls(void)
{
    BOOLEAN gpu = FALSE, even = FALSE, storage = FALSE;
    mapped[L] = 31; mapped[ALPHA] = 0x4000; mapped[USER] = (1u << 25) | (1u << 10);
    mapped[GPLL4 + L] = 41; mapped[GPLL4 + ALPHA] = 0xfaaa; mapped[GPLL4 + USER] = 1u << 25;
    mapped[VOTE] = 0x90;  /* Another subsystem and an inherited GPLL4. */
    mapped[GPLL4] = LOCKED;
    for (unsigned retained = 0; retained <= 1; ++retained) {
        assert(Sm8750SetGccPll(&gcc, Sm8750Gpll0, &gpu, TRUE) == STATUS_SUCCESS);
        assert(Sm8750SetGccPll(&gcc, Sm8750Gpll0Even, &even, TRUE) == STATUS_SUCCESS);
        assert(Sm8750SetGccPll(&gcc, Sm8750Gpll4, &storage, TRUE) == STATUS_SUCCESS && mapped[VOTE] == 0x91);
        assert(Sm8750GccPower(&gcc, FALSE, FALSE) == STATUS_SUCCESS);
        mapped = NULL;  /* No kernel register access is valid in D3. */
        assert(!Sm8750UfsPowerRetained(&gcc, &disk));
        assert(Sm8750ReadUfsClock(&gcc, &disk, 0x77000) == MAXULONG);
        Sm8750WriteUfsClock(&gcc, &disk, 0x77000, 99);
        assert(Sm8750SetGccPll(&gcc, Sm8750Gpll4, &storage, FALSE) == STATUS_DEVICE_NOT_READY && storage);
        if (!retained) { first[VOTE] &= ~1u; first[0] = 0; }
        assert(Sm8750GccPower(&gcc, TRUE, FALSE) == STATUS_SUCCESS);
        mapped = first;
        assert(!Sm8750UfsPowerRetained(&gcc, &disk));
        assert(Sm8750SetGccPll(&gcc, Sm8750Gpll0, &gpu, FALSE) == STATUS_SUCCESS && !gpu && even);
        assert(mapped[VOTE] == 0x91 && (mapped[0] & LOCKED));
        assert(Sm8750SetGccPll(&gcc, Sm8750Gpll0Even, &even, FALSE) == STATUS_SUCCESS && mapped[VOTE] == 0x90);
        assert(Sm8750SetGccPll(&gcc, Sm8750Gpll4, &storage, FALSE) == STATUS_SUCCESS && mapped[VOTE] == 0x90);
    }
    assert(Sm8750ReleaseGccPllBootReference(&gcc, Sm8750Gpll4) == STATUS_SUCCESS && mapped[VOTE] == 0x80);
}

static void *read_once(void *arg)
{
    (void)arg;
    assert(Sm8750ReadUfsClock(&gcc, &disk, 0x77000) == 37);
    return NULL;
}

static void *remove_device(void *arg)
{
    (void)arg;
    removing = 1;
    assert(Sm8750GccPower(&gcc, FALSE, TRUE) == STATUS_SUCCESS);
    assert(Sm8750DetachGcc(&gcc) == second);
    assert(!pthread_mutex_lock(&gate));
    detached = 1;
    assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&gate));
    return NULL;
}

static void remap_and_remove(void)
{
    assert(Sm8750GccPower(&gcc, FALSE, FALSE) == STATUS_SUCCESS && Sm8750DetachGcc(&gcc) == first);
    mapped = NULL;
    assert(!Sm8750UfsPowerRetained(&gcc, &disk) && Sm8750ReadUfsClock(&gcc, &disk, 0x77000) == MAXULONG);
    CM_PARTIAL_RESOURCE_DESCRIPTOR moved = {
        .Type = CmResourceTypeMemory, .ShareDisposition = CmResourceShareDeviceExclusive,
        .start = UINT64_C(0x200200004), .length = SM8750_GCC_REQUIRED_BYTES,
    };
    assert(Sm8750PrepareGccPlls(&gcc, &moved, second) == STATUS_SUCCESS && !Sm8750UfsPowerRetained(&gcc, &disk));
    assert(Sm8750GccPower(&gcc, TRUE, FALSE) == STATUS_SUCCESS);
    assert(!Sm8750UfsPowerRetained(&gcc, &disk));
    assert(Sm8750CloseUfsClocks(&gcc, &disk));
    assert(Sm8750OpenUfsClocks(&gcc, &disk) == STATUS_SUCCESS && Sm8750UfsPowerRetained(&gcc, &disk));
    mapped = second;
    Sm8750WriteUfsClock(&gcc, &disk, 0x77000, 37);
    assert(second[0x77000 / 4] == 37 && first[0x77000 / 4] != 37);
    second[L] = 31; second[ALPHA] = 0x4000; second[USER] = 1u << 25;
    BOOLEAN gpu = FALSE;
    assert(Sm8750SetGccPll(&gcc, Sm8750Gpll0, &gpu, TRUE) == STATUS_SUCCESS && gpu);
    pause_read = 1;
    pthread_t reader, remover;
    assert(!pthread_create(&reader, NULL, read_once, NULL));
    assert(!pthread_mutex_lock(&gate));
    while (!reading) assert(!pthread_cond_wait(&changed, &gate));
    assert(!pthread_create(&remover, NULL, remove_device, NULL));
    while (!remove_blocked && !detached) assert(!pthread_cond_wait(&changed, &gate));
    assert(remove_blocked && !detached);
    pause_read = 0;
    assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_join(reader, NULL) && !pthread_join(remover, NULL) && detached);
    mapped = NULL;
    assert(!Sm8750UfsPowerRetained(&gcc, &disk));
    assert(Sm8750ReadUfsClock(&gcc, &disk, 0x77000) == MAXULONG);
    Sm8750WriteUfsClock(&gcc, &disk, 0x77000, 3);
    assert(Sm8750GccPower(&gcc, TRUE, FALSE) == STATUS_DEVICE_REMOVED);
    assert(Sm8750PrepareGccPlls(&gcc, &moved, second) == STATUS_DEVICE_REMOVED);
    assert(Sm8750SetGccPll(&gcc, Sm8750Gpll0, &gpu, FALSE) == STATUS_SUCCESS && !gpu);
    assert(Sm8750SetGccPll(&gcc, Sm8750Gpll0, &gpu, TRUE) == STATUS_DEVICE_REMOVED && !gpu);
    assert(Sm8750CloseUfsClocks(&gcc, &disk) && !Sm8750CloseUfsClocks(&gcc, &disk));
}

int main(void)
{
    prepare();
    ufs_registers();
    shared_plls();
    remap_and_remove();
    return 0;
}
