/* SPDX-License-Identifier: MIT */
#include "gmu_memory.h"
#include "gmu_memory_layout.h"

#define GMU_MEMORY_TAG 'mGrA'

static NTSTATUS map_buffer(ADRENO_GMU_MEMORY *m, ADRENO_GMU_BUFFER *b,
                           ULONG address, ULONG bytes, BOOLEAN map_cpu)
{
    PHYSICAL_ADDRESS low = {0}, high, skip = {0};
    high.QuadPart = MAXLONGLONG;
    /* Firmware and the CPU exchange data on a non-coherent bus. Keeping the
     * boot buffers noncached avoids cached aliases and per-packet cache ops.
     * The complete, zeroed MDL stays locked until HAL releases the domain.
     */
    b->mdl = MmAllocatePagesForMdlEx(low, high, skip, bytes, MmNonCached, MM_ALLOCATE_FULLY_REQUIRED);
    if (!b->mdl || MmGetMdlByteCount(b->mdl) != bytes) return STATUS_INSUFFICIENT_RESOURCES;
    b->bytes = bytes;
    if (map_cpu) {
        b->cpu = MmMapLockedPagesSpecifyCache(b->mdl, KernelMode, MmNonCached, NULL, FALSE,
                                               NormalPagePriority | MdlMappingNoExecute);
        if (!b->cpu) return STATUS_INSUFFICIENT_RESOURCES;
    }
    IOMMU_MAP_PHYSICAL_ADDRESS source = { .MapType = MapPhysicalAddressTypeMdl };
    source.Mdl.Mdl = b->mdl;
    IOMMU_DMA_LOGICAL_ADDRESS requested = address;
    NTSTATUS status = m->iommu.V2.MapLogicalRangeEx(m->domain, IOMMU_ACCESS_READ | IOMMU_ACCESS_WRITE,
                                                    &source, &requested, NULL, NULL, &b->iova);
    if (NT_SUCCESS(status)) {
        b->mapped = TRUE;
        if (b->iova != requested) return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return status;
}

static NTSTATUS unmap_buffer(ADRENO_GMU_MEMORY *m, ADRENO_GMU_BUFFER *b)
{
    if (!b->mapped) return STATUS_SUCCESS;
    NTSTATUS status = m->iommu.V2.UnmapLogicalRange(m->domain, b->iova, b->bytes >> PAGE_SHIFT);
    if (NT_SUCCESS(status)) b->mapped = FALSE;
    return status;
}

static void free_buffer(ADRENO_GMU_BUFFER *b)
{
    if (b->cpu) MmUnmapLockedPages(b->cpu, b->mdl);
    if (b->mdl) {
        MmFreePagesFromMdl(b->mdl);
        ExFreePool(b->mdl);
    }
}

NTSTATUS AdrenoGmuMemoryClose(ADRENO_GMU_MEMORY *m)
{
    PAGED_CODE();
    NTSTATUS status;
    if (m->attached) {
        status = m->iommu.V2.DetachDeviceEx(m->requester);
        if (!NT_SUCCESS(status)) return status;
        m->attached = FALSE;
    }
    if (m->domain) {
        status = unmap_buffer(m, &m->hfi);
        if (!NT_SUCCESS(status)) return status;
        for (SIZE_T i = 0; i < m->reserved_count; ++i) {
            status = unmap_buffer(m, &m->reserved[i]);
            if (!NT_SUCCESS(status)) return status;
        }
        status = unmap_buffer(m, &m->cache);
        if (!NT_SUCCESS(status)) return status;
        status = m->iommu.V2.DeleteDomain(m->domain);
        if (!NT_SUCCESS(status)) return status;
        m->domain = NULL;
    }
    if (m->requester) {
        status = m->iommu.V2.DeleteDevice(m->requester);
        if (!NT_SUCCESS(status)) return status;
        m->requester = NULL;
    }
    free_buffer(&m->hfi);
    for (SIZE_T i = 0; i < m->reserved_count; ++i) free_buffer(&m->reserved[i]);
    if (m->reserved) ExFreePoolWithTag(m->reserved, GMU_MEMORY_TAG);
    free_buffer(&m->cache);
    RtlZeroMemory(m, sizeof(*m));
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoGmuMemoryOpen(ADRENO_GMU_MEMORY *m, PDEVICE_OBJECT pdo,
                            ULONG input_mapping, const ADRENO_IMAGE *firmware)
{
    PAGED_CODE();
    if (!pdo || !firmware) return STATUS_INVALID_PARAMETER;
    if (m->requester || m->domain || m->reserved) return STATUS_ALREADY_INITIALIZED;
    struct adreno_gmu_layout layout;
    struct adreno_gmu_range *ranges = NULL;
    NTSTATUS status;
    enum adreno_gmu_layout_status parsed =
        adreno_gmu_memory_layout(firmware->data, firmware->bytes, NULL, 0, &layout);
    if (parsed == ADRENO_GMU_LAYOUT_NEEDS_SPACE) {
        SIZE_T count = layout.reserved_count;  /* At most one per 16-byte file header. */
        ranges = ExAllocatePool2(POOL_FLAG_PAGED, count * sizeof(*ranges), GMU_MEMORY_TAG);
        if (!ranges) return STATUS_INSUFFICIENT_RESOURCES;
        parsed = adreno_gmu_memory_layout(firmware->data, firmware->bytes, ranges, count, &layout);
    }
    if (parsed != ADRENO_GMU_LAYOUT_OK) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }
    status = IoGetIommuInterfaceEx(DMA_IOMMU_INTERFACE_EX_VERSION_2, 0, &m->iommu);
    if (!NT_SUCCESS(status)) goto done;
    IOMMU_DEVICE_CREATION_CONFIGURATION cfg = { .ConfigType = IommuDeviceCreationConfigTypeAcpi };
    InitializeListHead(&cfg.NextConfiguration);
    cfg.Acpi.InputMappingBase = input_mapping;
    cfg.Acpi.MappingsCount = 1;
    PIOMMU_DMA_DEVICE requester;
    status = m->iommu.V2.CreateDevice(pdo, &cfg, &requester);
    if (!NT_SUCCESS(status)) goto done;
    m->requester = requester;
    ULONG types;
    m->iommu.V2.QueryAvailableDomainTypes(requester, &types);
    IOMMU_DMA_DOMAIN_TYPE type;
    if (types & (1u << DomainTypeTranslate)) type = DomainTypeTranslate;
    else if (types & (1u << DomainTypeTranslateS1)) type = DomainTypeTranslateS1;
    else {
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    /* The GMU image has fixed virtual addresses. The HAL buddy allocator
     * cannot accept explicit mappings. HAL still owns the page tables.
     */
    IOMMU_DMA_LOGICAL_ALLOCATOR_CONFIG allocator = { .LogicalAllocatorType = IommuDmaLogicalAllocatorNone };
    IOMMU_DMA_DOMAIN_CREATION_FLAGS flags = {0};
    PIOMMU_DMA_DOMAIN domain;
    status = m->iommu.V2.CreateDomainEx(type, flags, &allocator, NULL, &domain);
    if (!NT_SUCCESS(status)) goto done;
    m->domain = domain;
    if (layout.cache_bytes) {
        status = map_buffer(m, &m->cache, ADRENO_GMU_CACHE_BASE, layout.cache_bytes, TRUE);
        if (!NT_SUCCESS(status)) goto done;
    }
    if (layout.reserved_count) {
        m->reserved = ExAllocatePool2(POOL_FLAG_PAGED, layout.reserved_count * sizeof(*m->reserved),
                                      GMU_MEMORY_TAG);
        if (!m->reserved) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        m->reserved_count = layout.reserved_count;
        for (SIZE_T i = 0; i < layout.reserved_count; ++i) {
            status = map_buffer(m, &m->reserved[i], ranges[i].address, ranges[i].bytes, FALSE);
            if (!NT_SUCCESS(status)) goto done;
        }
    }
    status = map_buffer(m, &m->hfi, layout.hfi.address, layout.hfi.bytes, TRUE);
    if (!NT_SUCCESS(status)) goto done;
    status = m->iommu.V2.AttachDeviceEx(domain, requester);
    if (NT_SUCCESS(status)) m->attached = TRUE;

done:
    if (ranges) ExFreePoolWithTag(ranges, GMU_MEMORY_TAG);
    if (!NT_SUCCESS(status)) {
        NTSTATUS cleanup = AdrenoGmuMemoryClose(m);
        if (!NT_SUCCESS(cleanup)) return cleanup;
    }
    return status;
}
