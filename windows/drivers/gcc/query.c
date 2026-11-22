/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include <initguid.h>
#include <wdmguid.h>
#include "query.h"

/* No consumer mutex in the callback: unregistering can wait for it to
 * finish while the caller serializes interface teardown under that mutex. */
static NTSTATUS change(PVOID notice, PVOID context)
{
    PTARGET_DEVICE_REMOVAL_NOTIFICATION n = notice;
    SM8750_CLOCK_TARGET *t = context;
    if (IsEqualGUID(&n->Event, &GUID_TARGET_DEVICE_REMOVE_COMPLETE)) InterlockedExchange(&t->removing, 2);
    else if (IsEqualGUID(&n->Event, &GUID_TARGET_DEVICE_QUERY_REMOVE))
        (void)InterlockedCompareExchange(&t->removing, 1, 0);
    else if (IsEqualGUID(&n->Event, &GUID_TARGET_DEVICE_REMOVE_CANCELLED))
        (void)InterlockedCompareExchange(&t->removing, 0, 1);
    return STATUS_SUCCESS;
}

NTSTATUS Sm8750ClockTargetStatus(SM8750_CLOCK_TARGET *t)
{
    LONG state = InterlockedCompareExchange(&t->removing, 0, 0);
    return state == 2 ? STATUS_DEVICE_REMOVED : state ? STATUS_DELETE_PENDING : STATUS_SUCCESS;
}

void Sm8750CloseClockTarget(SM8750_CLOCK_TARGET *t)
{
    if (t->notification) (void)IoUnregisterPlugPlayNotificationEx(t->notification);
    if (t->pdo) ObDereferenceObject(t->pdo);
    RtlZeroMemory(t, sizeof(*t));
}

static NTSTATUS completed(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS Sm8750QueryClockInterface(const GUID *category, const GUID *interface_id, PINTERFACE result,
                                   USHORT size, PDRIVER_OBJECT driver, SM8750_CLOCK_TARGET *watched)
{
    PAGED_CODE();
    if (!category || !interface_id || !result || size < sizeof(*result) || !watched || !driver ||
        watched->notification || watched->pdo) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(result, size);
    PWSTR links;
    NTSTATUS s = IoGetDeviceInterfaces(category, NULL, 0, &links);
    if (!NT_SUCCESS(s)) return s;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, links);
    if (!name.Length) {
        s = STATUS_DEVICE_NOT_READY;
    } else if (links[name.Length / sizeof(WCHAR) + 1]) {
        /* Piano has one provider of each kind. A second active instance
         * needs an explicit firmware connection; never pick by list order. */
        s = STATUS_DEVICE_CONFIGURATION_ERROR;
    } else {
        PFILE_OBJECT file;
        PDEVICE_OBJECT target;
        s = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES, &file, &target);
        if (NT_SUCCESS(s)) {
            watched->pdo = IoGetDeviceAttachmentBaseRef(target);
            s = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange, 0, file, driver,
                                               change, watched, &watched->notification);
            PIRP irp = NT_SUCCESS(s) ? IoAllocateIrp(target->StackSize, FALSE) : NULL;
            if (NT_SUCCESS(s) && !irp) {
                s = STATUS_INSUFFICIENT_RESOURCES;
            } else if (irp) {
                KEVENT event;
                KeInitializeEvent(&event, NotificationEvent, FALSE);
                irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
                PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
                stack->MajorFunction = IRP_MJ_PNP;
                stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
                stack->Parameters.QueryInterface.InterfaceType = interface_id;
                stack->Parameters.QueryInterface.Size = size;
                stack->Parameters.QueryInterface.Version = 1;
                stack->Parameters.QueryInterface.Interface = result;
                stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;
                IoSetCompletionRoutine(irp, completed, &event, TRUE, TRUE, TRUE);
                (void)IoCallDriver(target, irp);
                KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                s = irp->IoStatus.Status;
                IoFreeIrp(irp);
                if (NT_SUCCESS(s)) {
                    if (!result->InterfaceReference || !result->InterfaceDereference ||
                        result->Version != 1 || result->Size < size) {
                        if (result->InterfaceDereference) result->InterfaceDereference(result->Context);
                        s = STATUS_DEVICE_CONFIGURATION_ERROR;
                    } else if (!NT_SUCCESS(s = Sm8750ClockTargetStatus(watched)))
                        result->InterfaceDereference(result->Context);
                }
            }
            ObDereferenceObject(file);
        }
    }
    ExFreePool(links);
    if (!NT_SUCCESS(s)) {
        Sm8750CloseClockTarget(watched);
        RtlZeroMemory(result, size);
    }
    return s;
}
