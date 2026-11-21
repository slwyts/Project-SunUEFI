/* SPDX-License-Identifier: BSD-2-Clause */
#include "input.h"
#include "firmware.h"
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <vhf.h>
#include "reports.h"

#define WN_TAG 'n8NW'

struct WN8030_INPUT {
    VHFHANDLE vhf;
    HANDLE spb;
    PDEVICE_OBJECT fdo;
    KMUTEX lock;
    KSPIN_LOCK queue_lock;
    LIST_ENTRY operations;
    BOOLEAN stopping;
    BOOLEAN caps;
    UCHAR brightness;
    ULONG configured_generation;
    struct wn8030_reports reports;
    NTSTATUS submit_status;
};

static USHORT scan_time(ULONGLONG interrupt_time)
{
    return (USHORT)(interrupt_time / 1000);
}

static int send_report(void *context, const uint8_t *report, size_t length)
{
    WN8030_INPUT *in = context;
    HID_XFER_PACKET packet = { (PUCHAR)report, (ULONG)length, report[0] };
    in->submit_status = VhfReadReportSubmit(in->vhf, &packet);
    return NT_SUCCESS(in->submit_status);
}

static NTSTATUS result_status(const WN8030_INPUT *in, enum wn8030_result result)
{
    switch (result) {
    case WN8030_OK: return STATUS_SUCCESS;
    case WN8030_UNSUPPORTED: return STATUS_NOT_SUPPORTED;
    case WN8030_SHORT: return STATUS_BUFFER_TOO_SMALL;
    case WN8030_IO: return in->submit_status;
    default: return STATUS_DEVICE_DATA_ERROR;
    }
}

static NTSTATUS spb_open(const CM_PARTIAL_RESOURCE_DESCRIPTOR *connection, HANDLE *result)
{
    if (!connection || connection->Type != CmResourceTypeConnection ||
        connection->u.Connection.Class != CM_RESOURCE_CONNECTION_CLASS_SERIAL ||
        connection->u.Connection.Type != CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    WCHAR path[RESOURCE_HUB_PATH_CHARS];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    RtlInitEmptyUnicodeString(&name, path, sizeof(path));
    NTSTATUS status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&name, connection->u.Connection.IdLowPart,
                                                       connection->u.Connection.IdHighPart);
    if (!NT_SUCCESS(status)) return status;
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwOpenFile(result, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, 0,
                      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

static NTSTATUS read_frame(WN8030_INPUT *in, UCHAR data[WN8030_FRAME_SIZE])
{
    UCHAR reg = 0x4c;
    SPB_TRANSFER_LIST_AND_ENTRIES(2) tx;
    SPB_TRANSFER_LIST_INIT(&tx.List, 2);
    tx.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0,
                                                              &reg, sizeof(reg));
    tx.ExtraTransfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0,
                                                              data, WN8030_FRAME_SIZE);
    IO_STATUS_BLOCK io;
    NTSTATUS status = ZwDeviceIoControlFile(in->spb, NULL, NULL, NULL, &io, IOCTL_SPB_EXECUTE_SEQUENCE,
                                            &tx, sizeof(tx), NULL, 0);
    if (NT_SUCCESS(status) && io.Information != sizeof(reg) + WN8030_FRAME_SIZE)
        return STATUS_DEVICE_DATA_ERROR;
    return status;
}

static NTSTATUS command(WN8030_INPUT *in, UCHAR cmd, UCHAR value)
{
    UCHAR data[67] = { 0x4c, 0x32, 0, 0x4e, 0x31, 0x80, 0x38, cmd, 1, value };
    for (size_t i = 3; i <= 9; ++i) data[10] += data[i];
    IO_STATUS_BLOCK io;
    NTSTATUS status = ZwWriteFile(in->spb, NULL, NULL, NULL, &io, data, sizeof(data), NULL, NULL);
    if (NT_SUCCESS(status) && io.Information != sizeof(data)) return STATUS_DEVICE_DATA_ERROR;
    return status;
}

static NTSTATUS configure_cover(WN8030_INPUT *in)
{
    NTSTATUS status = command(in, 0x25, 1);
    if (NT_SUCCESS(status)) status = command(in, 0x21, 1);
    if (NT_SUCCESS(status)) status = command(in, 0x2e, in->caps ? 0xfd : 0xfc);
    if (NT_SUCCESS(status)) status = command(in, 0x23, in->brightness);
    if (NT_SUCCESS(status)) in->configured_generation = in->reports.attachment_generation;
    return status;
}

static BOOLEAN needs_setup(const WN8030_INPUT *in)
{
    return in->reports.attachment_known && !in->reports.detached &&
           in->reports.attachment_generation != in->configured_generation;
}

static void lock(WN8030_INPUT *in)
{
    (void)KeWaitForSingleObject(&in->lock, Executive, KernelMode, FALSE, NULL);
}

/* VHF feature/output callbacks run up to DISPATCH_LEVEL. Both SPB transfers
 * and changes of input mode run at PASSIVE_LEVEL, serialized with FIFO reads. */
enum hid_operation { get_feature, set_feature, write_report };
struct hid_work {
    LIST_ENTRY entry;
    PIO_WORKITEM work;
    WN8030_INPUT *in;
    VHFOPERATIONHANDLE op;
    PHID_XFER_PACKET packet;
    enum hid_operation kind;
};

static IO_WORKITEM_ROUTINE hid_worker;
static VOID hid_worker(PDEVICE_OBJECT fdo, PVOID context)
{
    struct hid_work *w = context;
    WN8030_INPUT *in = w->in;
    PHID_XFER_PACKET p = w->packet;
    NTSTATUS status = STATUS_DELETE_PENDING;
    UNREFERENCED_PARAMETER(fdo);
    lock(in);
    if (!in->stopping) {
        if (!p->reportBuffer || !p->reportBufferLen) status = STATUS_BUFFER_TOO_SMALL;
        else if (w->kind == get_feature) {
            size_t n = p->reportBufferLen;
            status = result_status(in, wn8030_get_feature(&in->reports, p->reportId, p->reportBuffer, &n));
            if (NT_SUCCESS(status)) p->reportBufferLen = (ULONG)n;
        } else if (w->kind == set_feature) {
            status = result_status(in, wn8030_set_feature(&in->reports, p->reportId, p->reportBuffer,
                                    p->reportBufferLen, scan_time(KeQueryInterruptTime()), send_report, in));
        } else if (p->reportId != WN8030_KEYBOARD) status = STATUS_NOT_SUPPORTED;
        else if (p->reportBufferLen < 2 || p->reportBuffer[0] != p->reportId) status = STATUS_INVALID_PARAMETER;
        else {
            BOOLEAN caps = (p->reportBuffer[1] & 1) != 0;
            status = in->reports.detached ? STATUS_SUCCESS : command(in, 0x2e, caps ? 0xfd : 0xfc);
            if (NT_SUCCESS(status)) in->caps = caps;
        }
    }
    KeReleaseMutex(&in->lock, FALSE);
    KIRQL irql;
    KeAcquireSpinLock(&in->queue_lock, &irql);
    RemoveEntryList(&w->entry);
    struct hid_work *next = IsListEmpty(&in->operations) ? NULL :
                            CONTAINING_RECORD(in->operations.Flink, struct hid_work, entry);
    KeReleaseSpinLock(&in->queue_lock, irql);
    IoFreeWorkItem(w->work);
    (void)VhfAsyncOperationComplete(w->op, status);
    if (next) IoQueueWorkItem(next->work, hid_worker, DelayedWorkQueue, next);
}

static void queue_op(WN8030_INPUT *in, VHFOPERATIONHANDLE op, struct hid_work *w,
                     PHID_XFER_PACKET p, enum hid_operation kind)
{
    w->work = IoAllocateWorkItem(in->fdo);
    if (!w->work) {
        (void)VhfAsyncOperationComplete(op, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    w->in = in;
    w->op = op;
    w->packet = p;
    w->kind = kind;
    KIRQL irql;
    KeAcquireSpinLock(&in->queue_lock, &irql);
    BOOLEAN start = IsListEmpty(&in->operations);
    InsertTailList(&in->operations, &w->entry);
    KeReleaseSpinLock(&in->queue_lock, irql);
    if (start) IoQueueWorkItem(w->work, hid_worker, DelayedWorkQueue, w);
}

static EVT_VHF_ASYNC_OPERATION get_op, set_op, write_op;
static VOID get_op(PVOID in, VHFOPERATIONHANDLE op, PVOID work, PHID_XFER_PACKET p)
{ queue_op(in, op, work, p, get_feature); }
static VOID set_op(PVOID in, VHFOPERATIONHANDLE op, PVOID work, PHID_XFER_PACKET p)
{ queue_op(in, op, work, p, set_feature); }
static VOID write_op(PVOID in, VHFOPERATIONHANDLE op, PVOID work, PHID_XFER_PACKET p)
{ queue_op(in, op, work, p, write_report); }

NTSTATUS Wn8030InputCreate(PDEVICE_OBJECT fdo, const CM_PARTIAL_RESOURCE_DESCRIPTOR *connection,
                          WN8030_INPUT **result)
{
    PAGED_CODE();
    *result = NULL;
    WN8030_INPUT *in = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*in), WN_TAG);
    if (!in) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeMutex(&in->lock, 0);
    KeInitializeSpinLock(&in->queue_lock);
    InitializeListHead(&in->operations);
    wn8030_reports_init(&in->reports);
    in->fdo = fdo;
    in->brightness = 80;
    NTSTATUS status = spb_open(connection, &in->spb);
    if (NT_SUCCESS(status)) status = Wn8030BootFirmware(fdo->DriverObject, in->spb);
    if (NT_SUCCESS(status)) {
        VHF_CONFIG cfg;
        VHF_CONFIG_INIT(&cfg, fdo, (USHORT)wn8030_descriptor_length, (PUCHAR)wn8030_descriptor);
        cfg.VhfClientContext = in;
        cfg.OperationContextSize = sizeof(struct hid_work);
        cfg.EvtVhfAsyncOperationGetFeature = get_op;
        cfg.EvtVhfAsyncOperationSetFeature = set_op;
        cfg.EvtVhfAsyncOperationWriteReport = write_op;
        status = VhfCreate(&cfg, &in->vhf);
        if (NT_SUCCESS(status)) status = VhfStart(in->vhf);
    }
    if (!NT_SUCCESS(status)) {
        if (in->vhf) VhfDelete(in->vhf, TRUE);
        if (in->spb) ZwClose(in->spb);
        ExFreePoolWithTag(in, WN_TAG);
    } else *result = in;
    return status;
}

NTSTATUS Wn8030InputRead(WN8030_INPUT *in, ULONGLONG interrupt_time)
{
    UCHAR frame[WN8030_FRAME_SIZE];
    PAGED_CODE();
    lock(in);
    NTSTATUS status = in->stopping ? STATUS_DELETE_PENDING : read_frame(in, frame);
    if (NT_SUCCESS(status)) {
        status = result_status(in, wn8030_frame(&in->reports, frame, sizeof(frame),
                               scan_time(interrupt_time), send_report, in));
        if (NT_SUCCESS(status) && needs_setup(in)) status = configure_cover(in);
    }
    KeReleaseMutex(&in->lock, FALSE);
    return status;
}

NTSTATUS Wn8030SetCoverPower(WN8030_INPUT *in, BOOLEAN enable)
{
    PAGED_CODE();
    lock(in);
    NTSTATUS status = in->stopping ? STATUS_DELETE_PENDING :
                      enable && needs_setup(in) ? configure_cover(in) : command(in, 0x25, !!enable);
    KeReleaseMutex(&in->lock, FALSE);
    return status;
}

NTSTATUS Wn8030SetBacklight(WN8030_INPUT *in, UCHAR brightness)
{
    PAGED_CODE();
    lock(in);
    NTSTATUS status = in->stopping ? STATUS_DELETE_PENDING :
                      in->reports.detached ? STATUS_SUCCESS : command(in, 0x23, brightness);
    if (NT_SUCCESS(status)) in->brightness = brightness;
    KeReleaseMutex(&in->lock, FALSE);
    return status;
}

VOID Wn8030InputDelete(WN8030_INPUT *in)
{
    PAGED_CODE();
    lock(in);
    in->stopping = TRUE;
    (void)wn8030_release(&in->reports, scan_time(KeQueryInterruptTime()), send_report, in);
    KeReleaseMutex(&in->lock, FALSE);
    VhfDelete(in->vhf, TRUE);
    ZwClose(in->spb);
    ExFreePoolWithTag(in, WN_TAG);
}
