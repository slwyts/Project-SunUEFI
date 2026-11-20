/* SPDX-License-Identifier: MIT */
#include "firmware.h"
#include "gmu_firmware.h"

#define FIRMWARE_TAG 'wFrA'

static NTSTATUS read_image(HANDLE directory, PCWSTR name, ADRENO_IMAGE *image)
{
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION info;
    HANDLE file;
    UNICODE_STRING filename;

    RtlInitUnicodeString(&filename, name);
    InitializeObjectAttributes(&attr, &filename, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               directory, NULL);
    NTSTATUS status = ZwOpenFile(&file, FILE_READ_DATA | SYNCHRONIZE, &attr, &io, FILE_SHARE_READ,
                                 FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status))
        return status;

    status = ZwQueryInformationFile(file, &io, &info, sizeof(info), FileStandardInformation);
    if (NT_SUCCESS(status)) {
        if (info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > MAXULONG)
            status = STATUS_INVALID_IMAGE_FORMAT;
        else {
            ULONG length = (ULONG)info.EndOfFile.QuadPart;
            void *data = ExAllocatePool2(POOL_FLAG_PAGED, length, FIRMWARE_TAG);
            if (!data)
                status = STATUS_INSUFFICIENT_RESOURCES;
            else {
                status = ZwReadFile(file, NULL, NULL, NULL, &io, data, length, NULL, NULL);
                if (NT_SUCCESS(status) && io.Information != length)
                    status = STATUS_END_OF_FILE;
                if (NT_SUCCESS(status)) {
                    image->data = data;
                    image->bytes = length;
                } else
                    ExFreePoolWithTag(data, FIRMWARE_TAG);
            }
        }
    }
    ZwClose(file);
    return status;
}

void AdrenoFreeFirmware(ADRENO_FIRMWARE *firmware)
{
    if (firmware->gmu.data) ExFreePoolWithTag(firmware->gmu.data, FIRMWARE_TAG);
    if (firmware->sqe.data) ExFreePoolWithTag(firmware->sqe.data, FIRMWARE_TAG);
    if (firmware->aqe.data) ExFreePoolWithTag(firmware->aqe.data, FIRMWARE_TAG);
    RtlZeroMemory(firmware, sizeof(*firmware));
}

NTSTATUS AdrenoReadFirmware(PDRIVER_OBJECT driver, ADRENO_FIRMWARE *firmware)
{
    HANDLE directory;
    PAGED_CODE();
    RtlZeroMemory(firmware, sizeof(*firmware));
    NTSTATUS status = IoGetDriverDirectory(driver, DriverDirectoryImage, 0, &directory);
    if (!NT_SUCCESS(status))
        return status;
    status = read_image(directory, L"gen80000_gmu.bin", &firmware->gmu);
    if (NT_SUCCESS(status)) status = read_image(directory, L"gen80000_sqe.fw", &firmware->sqe);
    if (NT_SUCCESS(status)) status = read_image(directory, L"gen80000_aqe.fw", &firmware->aqe);
    ZwClose(directory);
    if (!NT_SUCCESS(status)) AdrenoFreeFirmware(firmware);
    return status;
}

struct gmu_target {
    volatile ULONG *tcm[2];
    unsigned char *cache;
};

static void upload_segment(void *context, const struct adreno_gmu_segment *segment)
{
    struct gmu_target *target = context;
    if (segment->region < RTL_NUMBER_OF(target->tcm)) {
        /* TCM is an MMIO aperture, not cacheable system memory. */
        WRITE_REGISTER_BUFFER_ULONG(target->tcm[segment->region] + segment->offset / sizeof(ULONG),
                                    (ULONG *)segment->data, (ULONG)(segment->size / sizeof(ULONG)));
    } else
        RtlCopyMemory(target->cache + segment->offset, segment->data, segment->size);
}

NTSTATUS AdrenoUploadGmu(const ADRENO_IMAGE *image, volatile ULONG *tcm,
                         PMDL cache)
{
    PAGED_CODE();
    if (!tcm || !cache) return STATUS_INVALID_PARAMETER;
    void *address = MmGetSystemAddressForMdlSafe(cache, NormalPagePriority | MdlMappingNoExecute);
    if (!address)
        return STATUS_INSUFFICIENT_RESOURCES;
    /* These are addresses in the A830 GMU firmware ABI. The CPU mappings are
     * supplied by the device, not calculated from a phone's physical map.
     */
    const struct adreno_gmu_region regions[] = {
        {0, ADRENO_GMU_TCM_BYTES}, {0x10004000, ADRENO_GMU_TCM_BYTES},
        {ADRENO_GMU_TCM_BYTES, MmGetMdlByteCount(cache)}
    };
    struct gmu_target target = {{tcm, tcm + ADRENO_GMU_TCM_BYTES / sizeof(*tcm)}, address};
    if (!adreno_gmu_load(image->data, image->bytes, regions, RTL_NUMBER_OF(regions),
                          upload_segment, &target))
        return STATUS_INVALID_IMAGE_FORMAT;
    KeFlushIoBuffers(cache, FALSE, TRUE);
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoCopyMicrocode(const ADRENO_IMAGE *image, PMDL destination, SIZE_T *copied)
{
    /* The first word is the file header; the following words go to the GPU. */
    *copied = 0;
    if (!image->data || image->bytes <= sizeof(ULONG) || image->bytes % sizeof(ULONG))
        return STATUS_INVALID_IMAGE_FORMAT;
    SIZE_T bytes = image->bytes - sizeof(ULONG);
    if (!destination)
        return STATUS_INVALID_PARAMETER;
    if (MmGetMdlByteCount(destination) < bytes)
        return STATUS_BUFFER_TOO_SMALL;
    void *address = MmGetSystemAddressForMdlSafe(destination, NormalPagePriority | MdlMappingNoExecute);
    if (!address)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(address, (const unsigned char *)image->data + sizeof(ULONG), bytes);
    KeFlushIoBuffers(destination, FALSE, TRUE);
    *copied = bytes;
    return STATUS_SUCCESS;
}
