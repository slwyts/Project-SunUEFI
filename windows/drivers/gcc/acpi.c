/* SPDX-License-Identifier: MIT */
#include <ntddk.h>
#include "acpi.h"
#include <acpiioct.h>

static const GUID properties = {0xdaffd814, 0x6eba, 0x4d8c, {0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01}};

typedef struct {
    const UCHAR *at, *end;
} ARGUMENTS;

static PACPI_METHOD_ARGUMENT next(ARGUMENTS *args)
{
    if ((SIZE_T)(args->end - args->at) < FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data)) return NULL;
    PACPI_METHOD_ARGUMENT a = (PACPI_METHOD_ARGUMENT)args->at;
    SIZE_T length = ACPI_METHOD_ARGUMENT_LENGTH_FROM_ARGUMENT(a);
    if (length > (SIZE_T)(args->end - args->at)) return NULL;
    args->at += length;
    return a;
}

static BOOLEAN is_value(PACPI_METHOD_ARGUMENT a, USHORT type, const VOID *value, SIZE_T size)
{
    return a && a->Type == type && a->DataLength == size && RtlEqualMemory((const UCHAR *)a->Data, value, size);
}

static BOOLEAN is_gcc(PACPI_EVAL_OUTPUT_BUFFER out, SIZE_T returned)
{
    if (returned < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) ||
        out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || out->Length > returned || (out->Count & 1) ||
        out->Length < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument)) return FALSE;
    ARGUMENTS sections = {(const UCHAR *)out->Argument, (const UCHAR *)out + out->Length};
    BOOLEAN compatible = FALSE;
    /* Acpi.sys returns the top-level package as its array of arguments. */
    for (ULONG i = 0; i < out->Count; i += 2) {
        PACPI_METHOD_ARGUMENT uuid = next(&sections), values = next(&sections);
        if (!uuid || uuid->Type != ACPI_METHOD_ARGUMENT_BUFFER || uuid->DataLength != sizeof(GUID) ||
            !values || values->Type != ACPI_METHOD_ARGUMENT_PACKAGE) return FALSE;
        if (!is_value(uuid, ACPI_METHOD_ARGUMENT_BUFFER, &properties, sizeof(properties))) continue;
        const UCHAR *data = (const UCHAR *)values->Data;
        ARGUMENTS list = {data, data + values->DataLength};
        while (list.at < list.end) {
            PACPI_METHOD_ARGUMENT pair = next(&list);
            if (!pair || pair->Type != ACPI_METHOD_ARGUMENT_PACKAGE) return FALSE;
            data = (const UCHAR *)pair->Data;
            ARGUMENTS entry = {data, data + pair->DataLength};
            PACPI_METHOD_ARGUMENT name = next(&entry), value = next(&entry);
            if (!name || name->Type != ACPI_METHOD_ARGUMENT_STRING || !value || entry.at != entry.end) return FALSE;
            if (is_value(name, ACPI_METHOD_ARGUMENT_STRING, "compatible", sizeof("compatible"))) {
                if (compatible || !is_value(value, ACPI_METHOD_ARGUMENT_STRING, "qcom,sm8750-gcc",
                                            sizeof("qcom,sm8750-gcc"))) return FALSE;
                compatible = TRUE;
            }
        }
    }
    return compatible && sections.at == sections.end;
}

NTSTATUS Sm8750CheckGccModel(WDFIOTARGET target)
{
    ACPI_EVAL_INPUT_BUFFER method = {.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE,
                                     .MethodName = {'_', 'D', 'S', 'D'}};
    ULONG stack[64];
    PACPI_EVAL_OUTPUT_BUFFER out = (PACPI_EVAL_OUTPUT_BUFFER)stack;
    ULONG length = sizeof(stack);
    WDF_MEMORY_DESCRIPTOR request, reply;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&request, &method, sizeof(method));
    ULONG_PTR bytes;
    NTSTATUS s;
    for (;;) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&reply, (PVOID)out, length);
        out->Length = 0;
        bytes = 0;
        s = WdfIoTargetSendIoctlSynchronously(target, NULL, IOCTL_ACPI_EVAL_METHOD, &request, &reply, NULL, &bytes);
        if (s != STATUS_BUFFER_OVERFLOW || (PVOID)out != stack || out->Length <= length) break;
        length = out->Length;
        out = ExAllocatePool2(POOL_FLAG_PAGED, length, 'dCcG');
        if (!out) return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(s) && (bytes > length || !is_gcc(out, bytes))) s = STATUS_DEVICE_CONFIGURATION_ERROR;
    if ((PVOID)out != stack) ExFreePoolWithTag((PVOID)out, 'dCcG');
    return s;
}
