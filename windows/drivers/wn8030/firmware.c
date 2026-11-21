/* SPDX-License-Identifier: BSD-2-Clause */
#include "firmware.h"
#include "boot.h"
#include <spb.h>

struct loader {
    HANDLE image, spb;
    NTSTATUS last_io;
};

static int completed(struct loader *loader, NTSTATUS status, const IO_STATUS_BLOCK *io,
                     ULONG bytes)
{
    if (NT_SUCCESS(status) && io->Information != bytes) status = STATUS_DEVICE_DATA_ERROR;
    loader->last_io = status;
    return NT_SUCCESS(status);
}

static int read_image(void *context, uint64_t offset, unsigned char *data, uint32_t bytes)
{
    struct loader *loader = context;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    NTSTATUS status = ZwReadFile(loader->image, NULL, NULL, NULL, &io, data, bytes, &pos, NULL);
    return completed(loader, status, &io, bytes);
}

static int write_data(void *context, const unsigned char *data, uint32_t bytes)
{
    struct loader *loader = context;
    IO_STATUS_BLOCK io;
    NTSTATUS status = ZwWriteFile(loader->spb, NULL, NULL, NULL, &io, (PVOID)data,
                                  bytes, NULL, NULL);
    return completed(loader, status, &io, bytes);
}

static int read_register(void *context, unsigned char reg, unsigned char *value)
{
    struct loader *loader = context;
    IO_STATUS_BLOCK io;
    SPB_TRANSFER_LIST_AND_ENTRIES(2) tx;
    SPB_TRANSFER_LIST_INIT(&tx.List, 2);
    tx.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0,
                                                               &reg, sizeof(reg));
    tx.ExtraTransfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0,
                                                              value, sizeof(*value));
    NTSTATUS status = ZwDeviceIoControlFile(loader->spb, NULL, NULL, NULL, &io,
                                            IOCTL_SPB_EXECUTE_SEQUENCE, &tx, sizeof(tx), NULL, 0);
    return completed(loader, status, &io, sizeof(reg) + sizeof(*value));
}

static void delay(void *context, unsigned int ms)
{
    LARGE_INTEGER interval;
    (void)context;
    interval.QuadPart = -(LONGLONG)ms * 10000;
    (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

NTSTATUS Wn8030BootFirmware(PDRIVER_OBJECT driver, HANDLE spb)
{
    PAGED_CODE();
    HANDLE directory;
    NTSTATUS status = IoGetDriverDirectory(driver, DriverDirectoryImage, 0, &directory);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    struct loader loader = {.spb = spb};
    RtlInitUnicodeString(&name, L"MCU_Upgrade.bin");
    InitializeObjectAttributes(&attr, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, directory, NULL);
    status = ZwOpenFile(&loader.image, FILE_READ_DATA | SYNCHRONIZE, &attr, &io, FILE_SHARE_READ,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    ZwClose(directory);
    if (!NT_SUCCESS(status)) return status;
    FILE_STANDARD_INFORMATION info;
    status = ZwQueryInformationFile(loader.image, &io, &info, sizeof(info), FileStandardInformation);
    if (NT_SUCCESS(status)) {
        const struct wn8030_boot_io bus = {&loader, read_image, write_data, read_register, delay};
        switch (wn8030_boot_ram(&bus, (uint64_t)info.EndOfFile.QuadPart)) {
        case WN_BOOT_OK:              break;
        case WN_BOOT_IO:              status = loader.last_io; break;
        case WN_BOOT_INVALID_IMAGE:   status = STATUS_INVALID_IMAGE_FORMAT; break;
        case WN_BOOT_ID:              status = STATUS_NO_SUCH_DEVICE; break;
        case WN_BOOT_STATE:           status = STATUS_INVALID_DEVICE_STATE; break;
        case WN_BOOT_HEADER_REJECTED: status = STATUS_DEVICE_PROTOCOL_ERROR; break;
        case WN_BOOT_VERIFY_FAILED:   status = STATUS_CRC_ERROR; break;
        }
    }
    ZwClose(loader.image);
    return status;
}
