// Reuse the existing read-only SFS fixture; exercise the real ESP selector,
// FileSource snapshot, CpuInput slices and launch-blob lifetime together.
#define main ExistingBootFileSourceFixtureMain
#include "PianoBootFileSourceTest.c"
#undef main
#include "../bootprofiles/os-boot/PianoEspBootSource.h"
#include <Protocol/PartitionInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>

EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiPartitionInfoProtocolGuid={.Data1=1},gEfiBlockIoProtocolGuid={.Data1=2},gEfiDevicePathProtocolGuid={.Data1=3};
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){return !memcmp(A,B,sizeof(*A));}
static CONST EFI_GUID EspGuid={0x5fd95c71,0xfea3,0x47af,{0xb0,0xdf,0x29,0xca,0x99,0x94,0xa2,0x3a}};
static CONST EFI_GUID EspType={0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b}};
static CONST EFI_GUID VendorGuid={0xa8675600,0x87d0,0x4a29,{0x9b,0x40,0x60,0,0,0,0,1}};
#pragma pack(push,1)
typedef struct {VENDOR_DEVICE_PATH Vendor;UFS_DEVICE_PATH Ufs;HARDDRIVE_DEVICE_PATH Hd;EFI_DEVICE_PATH_PROTOCOL End;} ESP_PATH;
#pragma pack(pop)
static EFI_PARTITION_INFO_PROTOCOL Partition;
static EFI_BLOCK_IO_PROTOCOL Block;
static EFI_BLOCK_IO_MEDIA Media;
static ESP_PATH DevicePath;
static EFI_HANDLE *HandleAllocation;
static UINTN HandleCount,HandleFrees,VolumeChecks;
static BOOLEAN ChangeAfterRead,MissingFile;

static EFI_DEVICE_PATH_PROTOCOL Header(UINT8 T,UINT8 S,UINTN N){
  return (EFI_DEVICE_PATH_PROTOCOL){T,S,{(UINT8)N,(UINT8)(N>>8)}};
}
static EFI_STATUS EFIAPI BlockRead(EFI_BLOCK_IO_PROTOCOL *B,UINT32 Id,EFI_LBA Lba,UINTN N,VOID *Buffer){
  (void)B;(void)Id;(void)Lba;(void)N;(void)Buffer;assert(!"selector must not read raw blocks");return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *Key,UINTN *N,EFI_HANDLE **Out){
  Check();assert(T==ByProtocol&&G==&gEfiSimpleFileSystemProtocolGuid&&!Key);
  *N=HandleCount;*Out=HandleAllocation=malloc(MAX(HandleCount,1)*sizeof(EFI_HANDLE));
  for(UINTN I=0;I<HandleCount;++I)(*Out)[I]=(VOID *)(7+I);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EspHandle(EFI_HANDLE H,EFI_GUID *G,VOID **Out){
  Check();assert(H==(VOID *)7||H==(VOID *)8);
  if(G==&gEfiPartitionInfoProtocolGuid){++VolumeChecks;if(ChangeAfterRead&&Reads)Partition.Info.Gpt.UniquePartitionGUID.Data1^=1;*Out=&Partition;}
  else if(G==&gEfiBlockIoProtocolGuid)*Out=&Block;
  else if(G==&gEfiDevicePathProtocolGuid)*Out=&DevicePath;
  else {assert(G==&gEfiSimpleFileSystemProtocolGuid);*Out=&Sfs;}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EspFree(VOID *P){
  if(P==HandleAllocation){Check();++HandleFrees;free(P);HandleAllocation=NULL;return EFI_SUCCESS;}
  return Free(P);
}
static EFI_STATUS EFIAPI EspOpen(EFI_FILE_PROTOCOL *P,EFI_FILE_PROTOCOL **Out,CHAR16 *Path,UINT64 Mode,UINT64 Attr){
  static CONST CHAR16 ExpectedPath[]=L"\\EFI\\Piano\\stable\\boot.img";
  assert(!memcmp(Path,ExpectedPath,sizeof(ExpectedPath))&&Mode==EFI_FILE_MODE_READ&&!Attr);
  if(MissingFile){++Opens;*Out=NULL;return EFI_NOT_FOUND;}return Open(P,Out,Path,Mode,Attr);
}
static VOID EspSetup(VOID){
  Case=Reads=Seeks=Opens=Roots=FileCloses=RootCloses=Allocations=Frees=Closes=Calls=Infos=0;
  Slices=MemoryChecks=MemoryValidations=BufferValidations=HandleFrees=VolumeChecks=0;
  Services=TRUE;ChangeAfterRead=MissingFile=FALSE;HandleCount=1;HandleAllocation=NULL;Fence=NULL;
  memset(Pools,0,sizeof(Pools));Setup();gBS=&Bs;Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=EspHandle;Bs.FreePool=EspFree;Root.Open=EspOpen;
  Partition=(EFI_PARTITION_INFO_PROTOCOL){.Revision=EFI_PARTITION_INFO_PROTOCOL_REVISION,.Type=PARTITION_TYPE_GPT};
  Partition.Info.Gpt.UniquePartitionGUID=EspGuid;Partition.Info.Gpt.PartitionTypeGUID=EspType;
  Partition.Info.Gpt.StartingLBA=107774464;Partition.Info.Gpt.EndingLBA=107905535;
  static CONST CHAR16 Name[36]=L"sunuefi_esp";memcpy(Partition.Info.Gpt.PartitionName,Name,sizeof(Name));
  Media=(EFI_BLOCK_IO_MEDIA){.MediaPresent=TRUE,.LogicalPartition=TRUE,.ReadOnly=TRUE,.BlockSize=4096,.LastBlock=131071};
  Block=(EFI_BLOCK_IO_PROTOCOL){.Media=&Media,.ReadBlocks=BlockRead};
  memset(&DevicePath,0,sizeof(DevicePath));
  DevicePath.Vendor.Header=Header(HARDWARE_DEVICE_PATH,HW_VENDOR_DP,sizeof(VENDOR_DEVICE_PATH));DevicePath.Vendor.Guid=VendorGuid;
  DevicePath.Ufs.Header=Header(MESSAGING_DEVICE_PATH,MSG_UFS_DP,sizeof(UFS_DEVICE_PATH));
  DevicePath.Hd.Header=Header(MEDIA_DEVICE_PATH,MEDIA_HARDDRIVE_DP,sizeof(HARDDRIVE_DEVICE_PATH));
  DevicePath.Hd.PartitionNumber=35;DevicePath.Hd.PartitionStart=107774464;DevicePath.Hd.PartitionSize=131072;
  memcpy(DevicePath.Hd.Signature,&EspGuid,sizeof(EspGuid));DevicePath.Hd.MBRType=MBR_TYPE_EFI_PARTITION_TABLE_HEADER;DevicePath.Hd.SignatureType=SIGNATURE_TYPE_GUID;
  DevicePath.End=Header(END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,sizeof(EFI_DEVICE_PATH_PROTOCOL));
}
int main(void){
  PIANO_CPU_INPUT_ENV Cpu={.BootServicesAlive=Live,.ServiceSlice=CpuSlice};
  EspSetup();PIANO_ESP_BOOT_SOURCE S={0};assert(PianoEspBootLoad(&S,&Cpu)==EFI_SUCCESS);
  assert(HandleFrees==1&&Roots==1&&Opens==1&&FileCloses==1&&RootCloses==1&&!S.File.File&&!S.File.Root);
  assert(S.File.Bytes==ContentBytes&&!memcmp(S.File.Sha256,Expected,32)&&Slices>=3&&VolumeChecks==2);
  assert(PianoEspBootOwned(&S,S.Owner)&&!PianoEspBootOwned(&S,(VOID *)0xbad));
  UINTN OldReads=Reads;
  assert(S.Blob.BorrowView(S.Blob.Context,S.Owner,(PIANO_BOOT_RANGE){0,S.Blob.Bytes},&S.View,&S.Loan)==EFI_SUCCESS);
  assert(S.View==S.File.Data&&S.Loan==S.File.Loan&&!memcmp(S.View,Content,ContentBytes));
  assert(PianoEspBootRelease(&S)==EFI_SUCCESS&&PianoEspBootReleased(&S)&&Frees==1&&Closes==1&&Reads==OldReads);
  assert(!S.File.Data&&!S.File.Exit&&!S.File.Loan&&!S.Owner&&!S.Loan&&!S.View);
  UINTN Before=Calls;assert(PianoEspBootRelease(&S)==EFI_SUCCESS&&Calls==Before);
  assert(PianoEspBootLoad(&S,&Cpu)==EFI_ALREADY_STARTED&&Calls==Before);
  // Each mismatch must fail before even opening the volume or allocating a snapshot.
  for(UINTN I=0;I<24;++I){EspSetup();S=(PIANO_ESP_BOOT_SOURCE){0};switch(I){
    case 0:Partition.Info.Gpt.UniquePartitionGUID.Data1^=1;break;
    case 1:Partition.Info.Gpt.PartitionName[0]='X';break;
    case 2:Partition.Info.Gpt.PartitionTypeGUID.Data1^=1;break;
    case 3:DevicePath.Vendor.Guid.Data1^=1;break;
    case 4:DevicePath.Ufs.Lun=1;break;
    case 5:DevicePath.Ufs.Pun=1;break;
    case 6:DevicePath.Hd.PartitionNumber=36;break;
    case 7:DevicePath.Hd.Signature[0]^=1;break;
    case 8:DevicePath.Hd.PartitionStart++;break;
    case 9:DevicePath.Hd.PartitionSize--;break;
    case 10:DevicePath.Hd.SignatureType=0;break;
    case 11:DevicePath.Hd.MBRType=0;break;
    case 12:DevicePath.End.SubType=1;break;
    case 13:DevicePath.Ufs.Header.Length[0]--;break;
    case 14:Partition.Info.Gpt.StartingLBA++;break;
    case 15:Partition.Info.Gpt.EndingLBA--;break;
    case 16:Media.LastBlock--;break;
    case 17:Media.BlockSize=512;break;
    case 18:Block.ReadBlocks=NULL;break;
    case 19:Media.LogicalPartition=FALSE;break;
    case 20:Media.MediaPresent=FALSE;break;
    case 21:HandleCount=2;break;
    case 22:HandleCount=0;break;
    case 23:Partition.Type=PARTITION_TYPE_MBR;break;
  }
    EFI_STATUS E=PianoEspBootLoad(&S,&Cpu);assert(E==(I==21||I==22?EFI_COMPROMISED_DATA:EFI_NOT_FOUND));
    assert(!Opens&&!Roots&&!Reads&&!Allocations&&!S.Loaded&&!S.Owner&&!S.Retained&&HandleFrees==1);
  }
  EspSetup();S=(PIANO_ESP_BOOT_SOURCE){0};ChangeAfterRead=TRUE;
  assert(PianoEspBootLoad(&S,&Cpu)==EFI_MEDIA_CHANGED&&!S.Loaded&&!S.Retained&&S.File.Consumed&&Frees==1&&Closes==1);
  EspSetup();S=(PIANO_ESP_BOOT_SOURCE){0};Media.ReadOnly=FALSE;
  assert(PianoEspBootLoad(&S,&Cpu)==EFI_SUCCESS&&Opens==1&&PianoEspBootOwned(&S,S.Owner));
  assert(PianoEspBootRelease(&S)==EFI_SUCCESS&&PianoEspBootReleased(&S));
  EspSetup();S=(PIANO_ESP_BOOT_SOURCE){0};MissingFile=TRUE;
  assert(PianoEspBootLoad(&S,&Cpu)==EFI_NOT_FOUND&&!S.Retained&&S.File.Consumed&&RootCloses==1&&Closes==1&&!Allocations);
  assert(!S.File.File&&!S.File.Root&&!S.File.Exit&&!S.File.Data&&!S.File.Owner&&!S.File.Loan);
  MissingFile=FALSE;assert(PianoEspBootLoad(&S,&Cpu)==EFI_SUCCESS&&Opens==2&&PianoEspBootOwned(&S,S.Owner));
  assert(PianoEspBootRelease(&S)==EFI_SUCCESS&&PianoEspBootReleased(&S)&&Closes==2);
  // A failed event retirement is terminal, even after the active loan was
  // successfully removed. Nothing may retry closure or free its live buffer.
  EspSetup();S=(PIANO_ESP_BOOT_SOURCE){0};assert(PianoEspBootLoad(&S,&Cpu)==EFI_SUCCESS);
  assert(S.Blob.BorrowView(S.Blob.Context,S.Owner,(PIANO_BOOT_RANGE){0,S.Blob.Bytes},&S.View,&S.Loan)==EFI_SUCCESS);
  Case=16;assert(PianoEspBootRelease(&S)==EFI_DEVICE_ERROR&&S.Retained&&S.File.Retained);
  assert(!S.Loan&&!S.View&&!S.File.Loan&&S.File.Exit&&S.File.Data&&!Frees&&Closes==1&&!PianoEspBootReleased(&S));
  Before=Calls;assert(PianoEspBootRelease(&S)==EFI_ACCESS_DENIED&&Calls==Before);
  // Test-fixture reclamation only, after proving production release cannot retry.
  free(S.File.Exit);free(S.File.Data);
  puts("Actual ESP + FileSource + CpuInput: 29 cases PASS; fixed volume/path, writable ESP read-only open, missing-file retry, SHA, ownership, release and partial-failure retention; no device");
  return 0;
}
