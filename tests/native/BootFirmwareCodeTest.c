/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <string.h>
#undef NULL
#include <PiDxe.h>
#include <IndustryStandard/PeImage.h>
#include <Library/BaseLib.h>
#include "../../uefi/components/product-support/Drivers/BootFirmwareCodeDxe/FirmwareVolume.h"

static _Alignas(4096) UINT8 image[0x9000];
static EFI_MEMORY_ATTRIBUTE_PROTOCOL protocol;
static UINT64 attrs[9];
static EFI_IMAGE_SECTION_HEADER *sections;
static UINTN writes, fail_clear;

UINT8 EFIAPI CalculateSum8(CONST UINT8 *p, UINTN n) {
    UINT8 sum=0; while (n--) sum+=*p++; return sum;
}
UINT16 EFIAPI CalculateSum16(CONST UINT16 *p, UINTN n) {
    UINT16 sum=0; for (UINTN i=0; i<n/2; ++i) sum+=*p++; return sum;
}
static UINTN page(UINT64 addr, UINT64 size) {
    assert(addr>=(UINTN)image && size && !(size&4095) && !(addr&4095));
    UINTN index=(addr-(UINTN)image)/4096;
    assert(index+size/4096<=ARRAY_SIZE(attrs)); return index;
}
static EFI_STATUS EFIAPI get(EFI_MEMORY_ATTRIBUTE_PROTOCOL *p, EFI_PHYSICAL_ADDRESS addr, UINT64 size, UINT64 *out) {
    UINTN i=page(addr,size); assert(p==&protocol);
    *out=attrs[i]; for (UINTN j=1; j<size/4096; ++j) if (attrs[i+j]!=*out) return EFI_NO_MAPPING;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set(EFI_MEMORY_ATTRIBUTE_PROTOCOL *p, EFI_PHYSICAL_ADDRESS addr, UINT64 size, UINT64 mask) {
    UINTN i=page(addr,size); assert(p==&protocol && mask==EFI_MEMORY_RO); ++writes;
    for (UINTN j=0; j<size/4096; ++j) attrs[i+j]|=mask;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI clear(EFI_MEMORY_ATTRIBUTE_PROTOCOL *p, EFI_PHYSICAL_ADDRESS addr, UINT64 size, UINT64 mask) {
    UINTN i=page(addr,size); assert(p==&protocol && mask==EFI_MEMORY_XP);
    if (fail_clear) return EFI_ACCESS_DENIED;
    for (UINTN j=0; j<size/4096; ++j) {
        assert(attrs[i+j]&EFI_MEMORY_RO); attrs[i+j]&=~mask;
    }
    return EFI_SUCCESS;
}
static EFI_MEMORY_ATTRIBUTE_PROTOCOL protocol={get,set,clear};

static void store24(UINT8 *p, UINTN n) { p[0]=n; p[1]=n>>8; p[2]=n>>16; }
static void init(void) {
    memset(image,0,sizeof(image)); writes=fail_clear=0;
    for (UINTN i=0;i<ARRAY_SIZE(attrs);++i) attrs[i]=EFI_MEMORY_XP;
    EFI_FIRMWARE_VOLUME_HEADER *fv=(VOID *)image;
    fv->Signature=EFI_FVH_SIGNATURE; fv->FvLength=0x300000; fv->HeaderLength=0x48;
    fv->Attributes=EFI_FVB2_ERASE_POLARITY;
    fv->Checksum=(UINT16)-CalculateSum16((CONST UINT16 *)fv,fv->HeaderLength);
    EFI_FFS_FILE_HEADER *ffs=(VOID *)(image+0x48);
    ffs->Type=EFI_FV_FILETYPE_SECURITY_CORE; store24(ffs->Size,sizeof(image)-0x48);
    ffs->IntegrityCheck.Checksum.Header=(UINT8)-CalculateSum8((CONST UINT8 *)ffs,sizeof(*ffs));
    ffs->State=0xf8; ffs->IntegrityCheck.Checksum.File=0xaa;
    EFI_COMMON_SECTION_HEADER *padding=(VOID *)(ffs+1);
    padding->Type=EFI_SECTION_RAW; store24(padding->Size,0xf4);
    EFI_COMMON_SECTION_HEADER *payload=(VOID *)(image+0x154);
    payload->Type=EFI_SECTION_TE; store24(payload->Size,sizeof(image)-0x154);
    EFI_TE_IMAGE_HEADER *te=(VOID *)(payload+1);
    te->Signature=EFI_TE_IMAGE_HEADER_SIGNATURE; te->Machine=IMAGE_FILE_MACHINE_ARM64;
    te->NumberOfSections=3; te->AddressOfEntryPoint=0x1110;
    te->ImageBase=(UINTN)image; te->StrippedSize=0x158+sizeof(*te);
    sections=(VOID *)(te+1);
    const UINT32 base[]={0x1000,0x3000,0x7000};
    for (UINTN i=0;i<ARRAY_SIZE(base);++i) {
        sections[i].VirtualAddress=sections[i].PointerToRawData=base[i];
        sections[i].SizeOfRawData=i?0x1000:0x2000;
        sections[i].Misc.VirtualSize=i==1?0x3100:0x900;
        sections[i].Characteristics=i==1?EFI_IMAGE_SCN_MEM_WRITE:EFI_IMAGE_SCN_MEM_EXECUTE;
    }
}
static void init_pe(void) {
    init();
    EFI_FFS_FILE_HEADER *ffs=(VOID *)(image+0x48);
    EFI_COMMON_SECTION_HEADER *padding=(VOID *)(ffs+1);
    store24(padding->Size,0xffc-(size_t)((UINT8 *)padding-image));
    EFI_COMMON_SECTION_HEADER *payload=(VOID *)(image+0xffc);
    payload->Type=EFI_SECTION_PE32; store24(payload->Size,sizeof(image)-0xffc);
    EFI_IMAGE_DOS_HEADER *dos=(VOID *)(payload+1);
    dos->e_magic=EFI_IMAGE_DOS_SIGNATURE; dos->e_lfanew=0x80;
    EFI_IMAGE_NT_HEADERS64 *pe=(VOID *)((UINT8 *)dos+dos->e_lfanew);
    pe->Signature=EFI_IMAGE_NT_SIGNATURE; pe->FileHeader.Machine=IMAGE_FILE_MACHINE_ARM64;
    pe->FileHeader.NumberOfSections=2; pe->FileHeader.SizeOfOptionalHeader=sizeof(pe->OptionalHeader);
    pe->OptionalHeader.Magic=EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    pe->OptionalHeader.ImageBase=(UINTN)dos; pe->OptionalHeader.AddressOfEntryPoint=0x1030;
    sections=(VOID *)(pe+1);
    for (unsigned i=0;i<2;++i) {
        sections[i].VirtualAddress=sections[i].PointerToRawData=(i+1)*0x1000;
        sections[i].SizeOfRawData=0x1000; sections[i].Misc.VirtualSize=i?0x1500:0x900;
        sections[i].Characteristics=i?EFI_IMAGE_SCN_MEM_WRITE:EFI_IMAGE_SCN_MEM_EXECUTE;
    }
}
int main(void) {
    init();
    assert(BootFirmwareProtectSec((UINTN)image,sizeof(image),&protocol)==EFI_SUCCESS);
    assert(writes==2);
    for (UINTN i=0;i<ARRAY_SIZE(attrs);++i)
        assert(attrs[i]==((i==1||i==2||i==7)?EFI_MEMORY_RO:EFI_MEMORY_XP));
    init(); sections[2].Characteristics|=EFI_IMAGE_SCN_MEM_WRITE;
    assert(EFI_ERROR(BootFirmwareProtectSec((UINTN)image,sizeof(image),&protocol)) && !writes);
    init(); sections[1].VirtualAddress=0x2900;
    assert(EFI_ERROR(BootFirmwareProtectSec((UINTN)image,sizeof(image),&protocol)) && !writes);
    init(); sections[2].PointerToRawData=0x1000;
    assert(EFI_ERROR(BootFirmwareProtectSec((UINTN)image,sizeof(image),&protocol)) && !writes);
    init(); sections[2].SizeOfRawData=0x400;
    store24(((EFI_COMMON_SECTION_HEADER *)(image+0x154))->Size,0x7400-0x154);
    assert(EFI_ERROR(BootFirmwareProtectSec((UINTN)image,sizeof(image),&protocol)) && !writes);
    init();
    assert(EFI_ERROR(BootFirmwareProtectSec((UINTN)image,sizeof(image)-4096,&protocol)) && !writes);
    init(); fail_clear=1;
    assert(BootFirmwareProtectSec((UINTN)image,sizeof(image),&protocol)==EFI_ACCESS_DENIED);
    assert(writes==1 && attrs[1]==(EFI_MEMORY_XP|EFI_MEMORY_RO) && attrs[7]==EFI_MEMORY_XP);
    init_pe();
    assert(BootFirmwareProtectSec((UINTN)image,sizeof(image),&protocol)==EFI_SUCCESS && writes==1);
    for (UINTN i=0;i<ARRAY_SIZE(attrs);++i) assert(attrs[i]==(i==2?EFI_MEMORY_RO:EFI_MEMORY_XP));
    init_pe(); ((EFI_IMAGE_DOS_HEADER *)(image+4096))->e_lfanew=MAX_UINT32;
    assert(EFI_ERROR(BootFirmwareProtectSec((UINTN)image,sizeof(image),&protocol)) && !writes);
}
