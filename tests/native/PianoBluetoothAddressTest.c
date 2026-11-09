// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../uefi/components/board-identity/PianoBluetoothAddress.c"
#include <assert.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef FDT_TAGSIZE
#include <libfdt.h>

EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = {.Data1 = 1};
EFI_GUID gEfiPartitionInfoProtocolGuid = {.Data1 = 2};
EFI_GUID gEfiBlockIoProtocolGuid = {.Data1 = 3};
EFI_GUID gEfiFileInfoGuid = {.Data1 = 4};
STATIC EFI_BOOT_SERVICES Services;
STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Filesystem;
STATIC EFI_PARTITION_INFO_PROTOCOL Partition;
STATIC EFI_BLOCK_IO_PROTOCOL Block;
STATIC EFI_BLOCK_IO_MEDIA Media;
STATIC EFI_FILE_PROTOCOL Root, File;
STATIC EFI_CHIPINFO_PROTOCOL ChipInfo;
STATIC UINT8 Tree[8192] __attribute__((aligned(8)));
STATIC UINT8 Factory[6] = {0x4c, 0x11, 0x22, 0x33, 0x44, 0x55};
STATIC CONST UINT8 QtiDefault[6] = {0x00, 0xbd, 0x00, 0x00, 0xbd, 0x00};
STATIC CONST UINT8 QtiDefaultOther[6] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
STATIC CONST UINT8 FactoryDt[6] = {0x55, 0x44, 0x33, 0x22, 0x11, 0x4c};
// Independent SHA256 fixture for chip 618, serial 0x12345678 and the v1 domain.
STATIC CONST UINT8 SocDt[6] = {0xeb, 0x1c, 0xd7, 0x06, 0xd8, 0xe2};
STATIC UINTN RootClosed, FileClosed, Reads, SerialReads;
STATIC BOOLEAN NoSerial;

VOID *EFIAPI CopyMem(VOID *To, CONST VOID *From, UINTN Bytes)
{
  return memcpy(To, From, Bytes);
}
VOID *EFIAPI ZeroMem(VOID *To, UINTN Bytes)
{
  return memset(To, 0, Bytes);
}
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN Bytes)
{
  return memcmp(A, B, Bytes);
}
INTN EFIAPI StrnCmp(CONST CHAR16 *A, CONST CHAR16 *B, UINTN Count)
{
  while (Count--) {
    if (*A != *B)
      return (INTN)*A - *B;
    if (!*A)
      return 0;
    ++A;
    ++B;
  }
  return 0;
}
VOID EFIAPI FreePool(VOID *Buffer)
{
  free(Buffer);
}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *Data, UINTN Bytes, UINT8 *Digest)
{
  return SHA256(Data, Bytes, Digest) != NULL;
}

STATIC EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Guid, VOID *Key,
                                UINTN *Count, EFI_HANDLE **Handles)
{
  assert(Type == ByProtocol && Guid == &gEfiSimpleFileSystemProtocolGuid && !Key);
  *Count = 1;
  *Handles = malloc(sizeof(**Handles));
  assert(*Handles);
  **Handles = &Filesystem;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Protocol(EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Interface)
{
  assert(Handle == &Filesystem);
  if (Guid == &gEfiPartitionInfoProtocolGuid)
    *Interface = &Partition;
  else if (Guid == &gEfiBlockIoProtocolGuid)
    *Interface = &Block;
  else if (Guid == &gEfiSimpleFileSystemProtocolGuid)
    *Interface = &Filesystem;
  else
    abort();
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI LocateChip(EFI_GUID *Guid, VOID *Registration, VOID **Interface)
{
  assert(!Registration && !memcmp(Guid, &mChipInfoGuid, sizeof(*Guid)));
  *Interface = &ChipInfo;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Serial(EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoSerialNumType *Value)
{
  assert(This == &ChipInfo);
  ++SerialReads;
  *Value = NoSerial ? 0 : 0x12345678;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Chip(EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoIdType *Value)
{
  assert(This == &ChipInfo);
  *Value = 618;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *This)
{
  if (This == &Root)
    ++RootClosed;
  else {
    assert(This == &File);
    ++FileClosed;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **Out, CHAR16 *Name,
                              UINT64 Mode, UINT64 Attributes)
{
  STATIC CONST CHAR16 Expected[] = L"\\bluetooth\\.bt_nv.bin";
  assert(This == &Root && !StrnCmp(Name, Expected, ARRAY_SIZE(Expected)) &&
         Mode == EFI_FILE_MODE_READ && !Attributes);
  *Out = &File;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Out)
{
  assert(This == &Filesystem);
  *Out = &Root;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *This, EFI_GUID *Guid, UINTN *Bytes, VOID *Buffer)
{
  assert(This == &File && Guid == &gEfiFileInfoGuid && *Bytes >= SIZE_OF_EFI_FILE_INFO);
  EFI_FILE_INFO *Information = Buffer;
  memset(Information, 0, *Bytes);
  Information->FileSize = sizeof(Factory);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *This, UINTN *Bytes, VOID *Buffer)
{
  assert(This == &File && *Bytes == sizeof(Factory));
  ++Reads;
  memcpy(Buffer, Factory, sizeof(Factory));
  return EFI_SUCCESS;
}
STATIC VOID SetExisting(CONST UINT8 Forward[6], BOOLEAN Broken)
{
  UINT8 Little[6];
  for (UINTN I = 0; I < 6; ++I)
    Little[I] = Forward[5 - I];
  int Node = fdt_path_offset(Tree, "/bluetooth");
  assert(Node >= 0);
  assert(!fdt_setprop(Tree, Node, "local-bd-address", Little, sizeof(Little)));
  if (Broken)
    assert(!fdt_setprop(Tree, Node, "qcom,local-bd-address-broken", NULL, 0));
}

int main(int Argc, char **Argv)
{
  assert(Argc == 2);
  int Case = atoi(Argv[1]);
  assert(Case >= 0 && Case < 8);
  gBS = &Services;
  Services.LocateHandleBuffer = Locate;
  Services.HandleProtocol = Protocol;
  Services.LocateProtocol = LocateChip;
  Filesystem.OpenVolume = Volume;
  Root.Open = Open;
  Root.Close = File.Close = Close;
  File.GetInfo = Info;
  File.Read = Read;
  ChipInfo.Revision = EFI_CHIPINFO_PROTOCOL_REVISION_3;
  ChipInfo.GetSerialNumber = Serial;
  ChipInfo.GetChipId = Chip;
  Partition.Revision = EFI_PARTITION_INFO_PROTOCOL_REVISION;
  Partition.Type = PARTITION_TYPE_GPT;
  memcpy(Partition.Info.Gpt.PartitionName, L"persist", sizeof(L"persist"));
  Block.Media = &Media;
  Media.MediaPresent = Media.LogicalPartition = Media.ReadOnly = TRUE;
  assert(!fdt_create_empty_tree(Tree, sizeof(Tree)));
  int Node = fdt_add_subnode(Tree, 0, "bluetooth");
  assert(Node >= 0);
  assert(!fdt_setprop_string(Tree, Node, "compatible", "qcom,wcn7861-bt"));
  if (Case == 1 || Case == 6)
    memcpy(Factory, QtiDefault, sizeof(Factory));
  if (Case == 2)
    memcpy(Factory, QtiDefaultOther, sizeof(Factory));
  if (Case == 3)
    SetExisting(QtiDefault, TRUE);
  if (Case == 4)
    SetExisting(QtiDefaultOther, TRUE);
  STATIC CONST UINT8 Existing[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
  if (Case == 5)
    SetExisting(Existing, FALSE);
  if (Case == 6)
    NoSerial = TRUE;
  if (Case == 7)
    Media.ReadOnly = FALSE;
  EFI_STATUS Status = PianoBluetoothAddressInitialize();
  assert(PianoBluetoothAddressInitialize() == Status);
  assert(Status == (Case == 6 ? EFI_NOT_FOUND : EFI_SUCCESS));
  assert(Reads == (Case == 7 ? 0 : 1) && RootClosed == Reads && FileClosed == Reads);
  assert(SerialReads == ((Case == 1 || Case == 2 || Case >= 6) ? 1 : 0));
  assert(PianoBluetoothAddressApplyDtb(Tree, sizeof(Tree)) == EFI_SUCCESS);
  Node = fdt_path_offset(Tree, "/bluetooth");
  int Bytes;
  CONST UINT8 *Address = fdt_getprop(Tree, Node, "local-bd-address", &Bytes);
  if (Case == 6) {
    assert(!Address);
    return 0;
  }
  UINT8 ExpectedExisting[6];
  for (UINTN I = 0; I < 6; ++I)
    ExpectedExisting[I] = Existing[5 - I];
  CONST UINT8 *Expected = Case == 5                               ? ExpectedExisting
                          : (Case == 1 || Case == 2 || Case == 7) ? SocDt
                                                                  : FactoryDt;
  assert(Address && Bytes == 6 && !memcmp(Address, Expected, 6));
  assert(!fdt_getprop(Tree, Node, "qcom,local-bd-address-broken", NULL));
  assert(PianoBluetoothAddressApplyDtb(Tree, sizeof(Tree)) == EFI_SUCCESS);
  puts("Actual factory/SoC Bluetooth identity and standard DT byte order passed");
  return 0;
}
