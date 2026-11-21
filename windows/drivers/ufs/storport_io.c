/* SPDX-License-Identifier: BSD-2-Clause */
#include "storport_io.h"
#include <srbhelper.h>
#include <intrin.h>

enum {
    HCI_CAP = 0, HCI_VER = 8, HCI_IS = 0x20, HCI_IE = 0x24, HCI_HCS = 0x30, HCI_HCE = 0x34,
    HCI_UTRLBA = 0x50, HCI_UTRLBAU = 0x54, HCI_UTRLDBR = 0x58, HCI_UTRLRSR = 0x60,
    HCI_MEM_CFG = 0x300, UTR_COMPLETE = 1,
    TRANSFER_FAULTS = 0x80 | 0x800 | 0x1000 | 0x10000 | 0x20000 | 0x40000
};

typedef struct { PUCHAR tail; } UFS_SRB_CONTEXT;

static ULONG command_bytes(const UFS_IO_QUEUE *q)
{
    /* The final DWORD is a separate aligned DMA target for an odd byte count.
     * It is never counted in the PRD table itself.
     */
    return (ULONG)sizeof(struct ufs_upius) + q->prds_per_command * (ULONG)sizeof(struct ufs_prd) + sizeof(ULONG);
}

static ULONG rd(UFS_IO_QUEUE *q, ULONG reg)
{
    return StorPortReadRegisterUlong(q->extension, (PULONG)(q->registers + reg));
}

static void wr(UFS_IO_QUEUE *q, ULONG reg, ULONG value)
{
    StorPortWriteRegisterUlong(q->extension, (PULONG)(q->registers + reg), value);
}

static int range_ok(ULONGLONG base, size_t bytes, ULONGLONG limit)
{
    return bytes && base <= limit && bytes - 1 <= limit - base;
}

static BOOLEAN dma_common_address(UFS_IO_QUEUE *q, PVOID cpu, ULONG bytes, ULONG alignment,
                                  STOR_PHYSICAL_ADDRESS *address)
{
    ULONG contiguous = 0;
    *address = StorPortGetPhysicalAddress(q->extension, NULL, cpu, &contiguous);
    return contiguous >= bytes && !((ULONGLONG)address->QuadPart % alignment) &&
        range_ok((ULONGLONG)address->QuadPart, bytes, q->dma_limit);
}

BOOLEAN UfsIoPrepare(UFS_IO_QUEUE *q, PVOID extension, PPORT_CONFIGURATION_INFORMATION config,
                     const ACCESS_RANGE *hci, UCHAR platform_dma_bits, USHORT prds_per_command)
{
    if (!q || !extension || !config || !hci || q->registers || !prds_per_command || !platform_dma_bits ||
        platform_dma_bits > 64 || config->Length < RTL_SIZEOF_THROUGH_FIELD(PORT_CONFIGURATION_INFORMATION,
                                                                            FeatureSupport) ||
        hci->RangeInMemory != TRUE || hci->RangeLength < HCI_MEM_CFG + sizeof(ULONG))
        return FALSE;
    q->registers = StorPortGetDeviceBase(extension, config->AdapterInterfaceType, config->SystemIoBusNumber,
                                         hci->RangeStart, hci->RangeLength, FALSE);
    if (!q->registers) return FALSE;
    q->extension = extension;
    ULONG caps = rd(q, HCI_CAP);
    if (caps == MAXULONG) goto fail;
    UCHAR bits = platform_dma_bits;
    if (bits > 32 && (!(caps & (1u << 24)) || !(config->Dma64BitAddresses & SCSI_DMA64_SYSTEM_SUPPORTED)))
        bits = 32;
    q->dma_limit = bits == 64 ? UINT64_MAX : ((ULONGLONG)1 << bits) - 1;
    q->slot_count = 1 + (caps & 31);
    q->prds_per_command = prds_per_command;
    config->Dma32BitAddresses = TRUE;
    config->Dma64BitAddresses = bits > 32 ? SCSI_DMA64_MINIPORT_FULL64BIT_NO_BOUNDARY_REQ_SUPPORTED : 0;
    config->DmaAddressWidth = bits;
    config->FeatureSupport |= STOR_ADAPTER_DMA_ADDRESS_WIDTH_SPECIFIED;
    config->ScatterGather = config->Master = TRUE;
    config->AlignmentMask = 3;
    config->NumberOfPhysicalBreaks = prds_per_command > 1 ? prds_per_command - 1 : 1;
    // Account for an unaligned first page and reserve a PRD for any tail.
    config->MaximumTransferLength = prds_per_command > 1 ? (ULONG)(prds_per_command - 2) * PAGE_SIZE + 4 : 4;
    config->SrbExtensionSize = sizeof(UFS_SRB_CONTEXT);
    config->SynchronizationModel = StorSynchronizeHalfDuplex;
    config->MaxNumberOfIO = q->slot_count;
    config->MaxIOsPerLun = q->slot_count;
    ULONG list_bytes = q->slot_count * (ULONG)sizeof(*q->list);
    ULONG ucd_bytes = command_bytes(q);
    ULONG stride = (ucd_bytes + UFS_UCD_ALIGNMENT - 1) & ~(UFS_UCD_ALIGNMENT - 1u);
    ULONG list_space = (list_bytes + UFS_UCD_ALIGNMENT - 1) & ~(UFS_UCD_ALIGNMENT - 1u);
    ULONG alloc_bytes = 1023 + list_space + q->slot_count * stride;
    PUCHAR buf = StorPortGetUncachedExtension(extension, config, alloc_bytes);
    if (!buf) goto fail;
    ULONG contiguous = 0;
    STOR_PHYSICAL_ADDRESS first = StorPortGetPhysicalAddress(extension, NULL, buf, &contiguous);
    ULONG pad = (ULONG)(-(ULONGLONG)first.QuadPart & 1023u);
    if (contiguous <= pad) goto fail;
    buf += pad;
    q->list = (struct ufs_utrd *)buf;
    if (!dma_common_address(q, buf, list_bytes, 1024, &q->list_dma)) goto fail;
    buf += list_space;
    for (ULONG i = 0; i < q->slot_count; ++i, buf += stride) {
        if (!dma_common_address(q, buf, ucd_bytes, UFS_UCD_ALIGNMENT, &q->slot[i].dma)) goto fail;
        q->slot[i].ucd = (struct ufs_upius *)buf;
    }
    return TRUE;
fail:
    UfsIoRelease(q);
    return FALSE;
}

VOID UfsIoRelease(UFS_IO_QUEUE *q)
{
    if (!q->running && !q->active && q->registers) {
        StorPortFreeDeviceBase(q->extension, q->registers);
        RtlZeroMemory(q, sizeof(*q));
    }
}

BOOLEAN UfsIoStart(UFS_IO_QUEUE *q)
{
    if (!q || !q->extension || !q->registers || !q->list || !q->slot_count || q->slot_count > UFS_SDB_MAX_SLOTS ||
        !q->prds_per_command || q->running || q->active || q->faults)
        return FALSE;
    ULONG caps = rd(q, HCI_CAP), version = rd(q, HCI_VER);
    if (version < 0x200 || version == MAXULONG || caps == MAXULONG || q->slot_count > 1 + (caps & 31) ||
        (version >= 0x400 && ((caps & (1u << 29)) || (rd(q, HCI_MEM_CFG) & 1))) ||
        (!(caps & (1u << 24)) && q->dma_limit > MAXULONG))
        return FALSE;
    /* The board layer has already initialized power, PHY and link. Never
     * reprogram another owner's running list or reuse firmware DMA pages.
     */
    if (!(rd(q, HCI_HCE) & 1) || (rd(q, HCI_HCS) & 3) != 3 || rd(q, HCI_UTRLDBR) ||
        rd(q, HCI_UTRLRSR) || (rd(q, HCI_IS) & TRANSFER_FAULTS))
        return FALSE;
    ULONGLONG list = (ULONGLONG)q->list_dma.QuadPart;
    if (list % 1024 || !range_ok(list, q->slot_count * sizeof(*q->list), q->dma_limit))
        return FALSE;
    for (ULONG i = 0; i < q->slot_count; ++i) {
        ULONGLONG dma = (ULONGLONG)q->slot[i].dma.QuadPart;
        if (!q->slot[i].ucd || q->slot[i].srb || dma % UFS_UCD_ALIGNMENT ||
            !range_ok(dma, command_bytes(q), q->dma_limit))
            return FALSE;
    }
    RtlZeroMemory(q->list, q->slot_count * sizeof(*q->list));
    wr(q, HCI_UTRLBA, (ULONG)list);
    wr(q, HCI_UTRLBAU, (ULONG)(list >> 32));
    wr(q, HCI_IS, UTR_COMPLETE);
    wr(q, HCI_UTRLRSR, 1);
    if (!(rd(q, HCI_UTRLRSR) & 1)) return FALSE;
    wr(q, HCI_IE, rd(q, HCI_IE) | UTR_COMPLETE | TRANSFER_FAULTS);
    q->running = TRUE;
    return TRUE;
}

static void finish(UFS_IO_QUEUE *q, PVOID srb, UCHAR status, UCHAR scsi, ULONG bytes)
{
    SrbSetSrbStatus(srb, status);
    SrbSetScsiStatus(srb, scsi);
    SrbSetDataTransferLength(srb, bytes);
    StorPortNotification(RequestComplete, q->extension, srb);
}

BOOLEAN UfsIoBuildRequest(PVOID extension, PSCSI_REQUEST_BLOCK srb)
{
    if (SrbGetSrbFunction(srb) != SRB_FUNCTION_EXECUTE_SCSI) return TRUE;
    ULONG bytes = SrbGetDataTransferLength(srb), tail = bytes & 3;
    if (!tail) return TRUE;
    UFS_SRB_CONTEXT *context = SrbGetMiniportContext(srb);
    PVOID cpu = NULL;
    if (!context || StorPortGetSystemAddress(extension, srb, &cpu) != STOR_STATUS_SUCCESS || !cpu) {
        SrbSetSrbStatus(srb, SRB_STATUS_ERROR);
        SrbSetDataTransferLength(srb, 0);
        StorPortNotification(RequestComplete, extension, srb);
        return FALSE;
    }
    context->tail = (PUCHAR)cpu + bytes - tail;
    return TRUE;
}

BOOLEAN UfsIoStartRequest(UFS_IO_QUEUE *q, PVOID srb)
{
    ULONG flags = SrbGetSrbFlags(srb), size = SrbGetDataTransferLength(srb);
    if (!q->running || q->faults) {
        finish(q, srb, SRB_STATUS_BUSY, 0, 0);
        return TRUE;
    }
    if (SrbGetSrbFunction(srb) != SRB_FUNCTION_EXECUTE_SCSI || SrbGetPathId(srb) || SrbGetTargetId(srb)) {
        finish(q, srb, SRB_STATUS_INVALID_REQUEST, 0, 0);
        return TRUE;
    }
    ULONG slot;
    for (slot = 0; slot < q->slot_count && q->slot[slot].srb; ++slot) {}
    if (slot == q->slot_count) {
        finish(q, srb, SRB_STATUS_BUSY, 0, 0);
        return TRUE;
    }
    UFS_IO_SLOT *s = &q->slot[slot];
    ULONG io = flags & (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT);
    enum ufs_direction direction = !size ? UFS_NO_DATA : io == SRB_FLAGS_DATA_IN ? UFS_DATA_IN : UFS_DATA_OUT;
    enum ufs_attribute attr = UFS_SIMPLE;
    if (flags & SRB_FLAGS_QUEUE_ACTION_ENABLE) {
        ULONG tag_type = SrbGetRequestAttribute(srb);
        if (tag_type == SRB_HEAD_OF_QUEUE_TAG_REQUEST) attr = UFS_HEAD;
        if (tag_type == SRB_ORDERED_QUEUE_TAG_REQUEST) attr = UFS_ORDERED;
    }
    struct ufs_transfer x;
    if ((size && io != SRB_FLAGS_DATA_IN && io != SRB_FLAGS_DATA_OUT) ||
        !ufs_transfer_init(&x, &q->list[slot], s->ucd, (struct ufs_prd *)(s->ucd + 1), q->prds_per_command,
                           (ULONGLONG)s->dma.QuadPart, q->dma_limit, SrbGetLun(srb), (uint8_t)slot, attr, direction,
                           (const uint8_t *)SrbGetCdb(srb), SrbGetCdbLength(srb), size)) {
        finish(q, srb, SRB_STATUS_INVALID_REQUEST, 0, 0);
        return TRUE;
    }
    ULONG whole = size & ~3u, tail = size - whole;
    if (whole) {
        PSTOR_SCATTER_GATHER_LIST sg = StorPortGetScatterGatherList(q->extension, srb);
        if (!sg) goto bad_mapping;
        for (ULONG i = 0; i < sg->NumberOfElements && whole; ++i) {
            ULONG chunk = min(sg->List[i].Length, whole);
            if (!ufs_transfer_add_dma(&x, (ULONGLONG)sg->List[i].PhysicalAddress.QuadPart, chunk)) goto bad_mapping;
            whole -= chunk;
        }
        if (whole) goto bad_mapping;
    }
    s->tail_destination = NULL;
    if (tail) {
        UFS_SRB_CONTEXT *context = SrbGetMiniportContext(srb);
        ULONG offset = command_bytes(q) - sizeof(ULONG);
        PUCHAR bounce = (PUCHAR)s->ucd + offset;
        if (!context || !context->tail ||
            !ufs_transfer_add_dma(&x, (ULONGLONG)s->dma.QuadPart + offset, sizeof(ULONG))) goto bad_mapping;
        RtlZeroMemory(bounce, sizeof(ULONG));
        if (direction == UFS_DATA_OUT) StorPortMoveMemory(bounce, context->tail, tail);
        else s->tail_destination = context->tail;
    }
    if (!ufs_transfer_ready(&x)) goto bad_mapping;
    ULONG bit = 1u << slot;
    s->srb = srb;
    q->active |= bit;
    SrbSetSrbStatus(srb, SRB_STATUS_PENDING);
    __dmb(_ARM64_BARRIER_OSHST);
    wr(q, HCI_UTRLDBR, bit);
    return TRUE;
bad_mapping:
    finish(q, srb, SRB_STATUS_INVALID_REQUEST, 0, 0);
    return TRUE;
}

ULONG UfsIoInterrupt(UFS_IO_QUEUE *q)
{
    if (!q->running) return 0;
    ULONG pending = rd(q, HCI_IS) & rd(q, HCI_IE) & (UTR_COMPLETE | TRANSFER_FAULTS);
    if (!pending) return 0;
    wr(q, HCI_IS, pending); /* IS uses write-one-to-clear. */
    q->faults |= pending & TRANSFER_FAULTS;
    if (q->faults) return pending;
    ULONG done = q->active & ~rd(q, HCI_UTRLDBR);
    __dmb(_ARM64_BARRIER_OSHLD);
    for (ULONG slot = 0; slot < q->slot_count && done; ++slot) {
        ULONG bit = 1u << slot;
        if (!(done & bit)) continue;
        UFS_IO_SLOT *s = &q->slot[slot];
        struct ufs_completion c = ufs_transfer_complete(&q->list[slot], s->ucd);
        UCHAR status = SRB_STATUS_ERROR;
        if (c.result == UFS_RESULT_GOOD) status = SRB_STATUS_SUCCESS;
        else if (c.result == UFS_RESULT_LENGTH) status = SRB_STATUS_DATA_OVERRUN;
        else if (c.result == UFS_RESULT_ABORTED) status = SRB_STATUS_ABORTED;
        else if (c.result == UFS_RESULT_SCSI && c.scsi_status == SCSISTAT_BUSY) status = SRB_STATUS_BUSY;
        PVOID srb = s->srb;
        if (s->tail_destination) {
            ULONG head = SrbGetDataTransferLength(srb) & ~3u;
            if (c.bytes > head) {
                PUCHAR bounce = (PUCHAR)s->ucd + command_bytes(q) - sizeof(ULONG);
                StorPortMoveMemory(s->tail_destination, bounce, c.bytes - head);
            }
            s->tail_destination = NULL;
        }
        if (c.scsi_status == SCSISTAT_CHECK_CONDITION && c.sense_bytes && SrbGetSenseInfoBuffer(srb) &&
            !(SrbGetSrbFlags(srb) & SRB_FLAGS_DISABLE_AUTOSENSE)) {
            UCHAR available = SrbGetSenseInfoBufferLength(srb);
            UCHAR copied = (UCHAR)(c.sense_bytes < available ? c.sense_bytes : available);
            if (copied) {
                StorPortMoveMemory(SrbGetSenseInfoBuffer(srb), (PVOID)c.sense, copied);
                SrbSetSenseInfoBufferLength(srb, copied);
                status |= SRB_STATUS_AUTOSENSE_VALID;
            }
        }
        q->active &= ~bit;
        s->srb = NULL;
        done &= ~bit;
        finish(q, srb, status, c.scsi_status, c.bytes);
    }
    return pending;
}

BOOLEAN UfsIoStop(UFS_IO_QUEUE *q)
{
    if (!q->running) return TRUE;
    if (q->active || rd(q, HCI_UTRLDBR)) return FALSE;
    wr(q, HCI_IE, rd(q, HCI_IE) & ~(UTR_COMPLETE | TRANSFER_FAULTS));
    wr(q, HCI_UTRLRSR, 0);
    if (rd(q, HCI_UTRLRSR) || rd(q, HCI_UTRLDBR)) return FALSE;
    wr(q, HCI_UTRLBA, 0);
    wr(q, HCI_UTRLBAU, 0);
    q->running = FALSE;
    return TRUE;
}
