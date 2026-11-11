// Reuse the read-only SFS fixture for actual ESP identity and UKI discovery.
#define main ExistingBootFileSourceFixtureMain
#include "PianoBootFileSourceTest.c"
#undef main
#include "../../uefi/components/os-boot/PianoEspBootSource.h"
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
typedef struct {VENDOR_DEVICE_PATH Vendor;UFS_DEVICE_PATH Ufs;EFI_DEVICE_PATH_PROTOCOL End;} PARENT_PATH;
#pragma pack(pop)
static EFI_PARTITION_INFO_PROTOCOL Partition;
static EFI_BLOCK_IO_PROTOCOL Block;
static EFI_BLOCK_IO_MEDIA Media;
static ESP_PATH DevicePath;
static EFI_BLOCK_IO_PROTOCOL ParentBlock;
static EFI_BLOCK_IO_MEDIA ParentMedia;
static PARENT_PATH ParentPath;
static EFI_HANDLE *HandleAllocation,*ParentHandleAllocation;
static UINTN HandleCount,ParentHandleCount,HandleFrees,ParentFrees,VolumeChecks;
static BOOLEAN MissingFile;

static EFI_DEVICE_PATH_PROTOCOL Header(UINT8 T,UINT8 S,UINTN N){
  return (EFI_DEVICE_PATH_PROTOCOL){T,S,{(UINT8)N,(UINT8)(N>>8)}};
}
static EFI_STATUS EFIAPI BlockRead(EFI_BLOCK_IO_PROTOCOL *B,UINT32 Id,EFI_LBA Lba,UINTN N,VOID *Buffer){
  (void)B;(void)Id;(void)Lba;(void)N;(void)Buffer;assert(!"selector must not read raw blocks");return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI BlockWrite(EFI_BLOCK_IO_PROTOCOL *B,UINT32 Id,EFI_LBA Lba,UINTN N,VOID *Buffer){
  (void)B;(void)Id;(void)Lba;(void)N;(void)Buffer;assert(!"read-only loader must never write raw blocks");return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *Key,UINTN *N,EFI_HANDLE **Out){
  Check();assert(T==ByProtocol&&!Key);
  if(G==&gEfiBlockIoProtocolGuid){
    *N=ParentHandleCount;*Out=ParentHandleAllocation=malloc(MAX(ParentHandleCount,1)*sizeof(EFI_HANDLE));
    for(UINTN I=0;I<ParentHandleCount;++I)(*Out)[I]=(VOID *)(9+I);return EFI_SUCCESS;
  }
  assert(G==&gEfiSimpleFileSystemProtocolGuid);
  *N=HandleCount;*Out=HandleAllocation=malloc(MAX(HandleCount,1)*sizeof(EFI_HANDLE));
  for(UINTN I=0;I<HandleCount;++I)(*Out)[I]=(VOID *)(7+I);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EspHandle(EFI_HANDLE H,EFI_GUID *G,VOID **Out){
  Check();assert(H==(VOID *)7||H==(VOID *)8||H==(VOID *)9||H==(VOID *)10);
  if(H==(VOID *)9||H==(VOID *)10){
    if(G==&gEfiBlockIoProtocolGuid){*Out=&ParentBlock;}
    else{assert(G==&gEfiDevicePathProtocolGuid);*Out=&ParentPath;}return EFI_SUCCESS;
  }
  if(G==&gEfiPartitionInfoProtocolGuid){++VolumeChecks;*Out=&Partition;}
  else if(G==&gEfiBlockIoProtocolGuid)*Out=&Block;
  else if(G==&gEfiDevicePathProtocolGuid)*Out=&DevicePath;
  else {assert(G==&gEfiSimpleFileSystemProtocolGuid);*Out=&Sfs;}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EspFree(VOID *P){
  if(P==HandleAllocation){Check();++HandleFrees;free(P);HandleAllocation=NULL;return EFI_SUCCESS;}
  if(P==ParentHandleAllocation){Check();++ParentFrees;free(P);ParentHandleAllocation=NULL;return EFI_SUCCESS;}
  return Free(P);
}
static EFI_STATUS EFIAPI EspOpen(EFI_FILE_PROTOCOL *P,EFI_FILE_PROTOCOL **Out,CHAR16 *Path,UINT64 Mode,UINT64 Attr){
  static CONST CHAR16 EfiPath[]=L"\\EFI\\Linux\\sunuefi.efi";
  assert(!memcmp(Path,EfiPath,sizeof(EfiPath))&&Mode==EFI_FILE_MODE_READ&&!Attr);
  if(MissingFile){++Opens;*Out=NULL;return EFI_NOT_FOUND;}return Open(P,Out,Path,Mode,Attr);
}
static VOID EspSetup(VOID){
  Case=Reads=Seeks=Opens=Roots=FileCloses=RootCloses=Allocations=Frees=Closes=Calls=Infos=0;
  Slices=MemoryChecks=MemoryValidations=BufferValidations=HandleFrees=ParentFrees=VolumeChecks=0;
  Services=TRUE;MissingFile=FALSE;
  HandleCount=ParentHandleCount=1;HandleAllocation=ParentHandleAllocation=NULL;Fence=NULL;
  memset(Pools,0,sizeof(Pools));Setup();gBS=&Bs;Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=EspHandle;Bs.FreePool=EspFree;Root.Open=EspOpen;
  Partition=(EFI_PARTITION_INFO_PROTOCOL){.Revision=EFI_PARTITION_INFO_PROTOCOL_REVISION,.Type=PARTITION_TYPE_GPT};
  Partition.Info.Gpt.UniquePartitionGUID=EspGuid;Partition.Info.Gpt.PartitionTypeGUID=EspType;
  Partition.Info.Gpt.StartingLBA=107774464;Partition.Info.Gpt.EndingLBA=107905535;
  static CONST CHAR16 Name[36]=L"sunuefi_esp";memcpy(Partition.Info.Gpt.PartitionName,Name,sizeof(Name));
  Media=(EFI_BLOCK_IO_MEDIA){.MediaPresent=TRUE,.LogicalPartition=TRUE,.ReadOnly=TRUE,.BlockSize=4096,.LastBlock=131071};
  Block=(EFI_BLOCK_IO_PROTOCOL){.Media=&Media,.ReadBlocks=BlockRead,.WriteBlocks=BlockWrite};
  memset(&DevicePath,0,sizeof(DevicePath));
  DevicePath.Vendor.Header=Header(HARDWARE_DEVICE_PATH,HW_VENDOR_DP,sizeof(VENDOR_DEVICE_PATH));DevicePath.Vendor.Guid=VendorGuid;
  DevicePath.Ufs.Header=Header(MESSAGING_DEVICE_PATH,MSG_UFS_DP,sizeof(UFS_DEVICE_PATH));
  DevicePath.Hd.Header=Header(MEDIA_DEVICE_PATH,MEDIA_HARDDRIVE_DP,sizeof(HARDDRIVE_DEVICE_PATH));
  DevicePath.Hd.PartitionNumber=35;DevicePath.Hd.PartitionStart=107774464;DevicePath.Hd.PartitionSize=131072;
  memcpy(DevicePath.Hd.Signature,&EspGuid,sizeof(EspGuid));DevicePath.Hd.MBRType=MBR_TYPE_EFI_PARTITION_TABLE_HEADER;DevicePath.Hd.SignatureType=SIGNATURE_TYPE_GUID;
  DevicePath.End=Header(END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,sizeof(EFI_DEVICE_PATH_PROTOCOL));
  ParentMedia=(EFI_BLOCK_IO_MEDIA){.MediaPresent=TRUE,.LogicalPartition=FALSE,.ReadOnly=TRUE,.BlockSize=4096,.LastBlock=124552191};
  ParentBlock=(EFI_BLOCK_IO_PROTOCOL){.Media=&ParentMedia,.ReadBlocks=BlockRead,.WriteBlocks=BlockWrite};
  ParentPath=(PARENT_PATH){.Vendor=DevicePath.Vendor,.Ufs=DevicePath.Ufs,.End=DevicePath.End};
}
int main(void){
  EspSetup();EFI_HANDLE Esp=(VOID *)0xbad;MissingFile=TRUE;
  assert(PianoEspBootFind(NULL)==EFI_INVALID_PARAMETER&&!Calls);
  assert(PianoEspBootFind(&Esp)==EFI_SUCCESS&&Esp==(VOID *)7&&HandleFrees==1&&ParentFrees==1);
  assert(!Roots&&!Opens&&!Reads&&!Allocations&&PianoEspBootDiscovery()->Open==EFI_NOT_STARTED);
  EspSetup();HandleCount=257;Esp=(VOID *)0xbad;
  assert(PianoEspBootFind(&Esp)==EFI_COMPROMISED_DATA&&!Esp&&HandleFrees==1&&!Roots&&!Opens);
  EspSetup();
  assert(PianoEspBootPresent()==EFI_SUCCESS&&Opens==1&&!Reads&&!Allocations&&FileCloses==1&&RootCloses==1);
  EspSetup();MissingFile=TRUE;
  assert(PianoEspBootPresent()==EFI_NOT_FOUND&&Opens==1&&!Reads&&!Allocations&&!FileCloses&&RootCloses==1);
  // New installations choose fresh UUIDs, positions, capacities and GPT slots.
  for(UINTN I=0;I<3;++I){
    EspSetup();EFI_GUID Id={.Data1=(UINT32)(0xabc000+I),.Data4={1,2,3,4,5,6,7,8}};
    UINT64 Start=2048+I*1000000,Blocks=I==0?65536:I==1?262144:524288;
    Partition.Info.Gpt.UniquePartitionGUID=Id;Partition.Info.Gpt.StartingLBA=Start;Partition.Info.Gpt.EndingLBA=Start+Blocks-1;
    DevicePath.Hd.PartitionNumber=(UINT32)I+2;DevicePath.Hd.PartitionStart=Start;DevicePath.Hd.PartitionSize=Blocks;
    memcpy(DevicePath.Hd.Signature,&Id,sizeof(Id));Media.LastBlock=Blocks-1;
    assert(PianoEspBootPresent()==EFI_SUCCESS&&!Reads&&!Allocations&&ParentFrees==1);
  }
  // Each mismatch must fail before even opening the volume or allocating a snapshot.
  for(UINTN I=0;I<33;++I){EspSetup();switch(I){
    case 0:Partition.Info.Gpt.UniquePartitionGUID.Data1^=1;break;
    case 1:Partition.Info.Gpt.PartitionName[0]='X';break;
    case 2:Partition.Info.Gpt.PartitionTypeGUID.Data1^=1;break;
    case 3:DevicePath.Vendor.Guid.Data1^=1;break;
    case 4:DevicePath.Ufs.Lun=1;break;
    case 5:DevicePath.Ufs.Pun=1;break;
    case 6:DevicePath.Hd.PartitionNumber=0;break;
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
    case 24:memset(&Partition.Info.Gpt.UniquePartitionGUID,0,sizeof(EFI_GUID));memset(DevicePath.Hd.Signature,0,16);break;
    case 25:Partition.Info.Gpt.EndingLBA=Partition.Info.Gpt.StartingLBA-1;break;
    case 26:Partition.Info.Gpt.StartingLBA=DevicePath.Hd.PartitionStart=MAX_UINT64-10;
      Partition.Info.Gpt.EndingLBA=MAX_UINT64;DevicePath.Hd.PartitionSize=11;Media.LastBlock=10;break;
    case 27:ParentMedia.LastBlock=Partition.Info.Gpt.EndingLBA-1;break;
    case 28:ParentMedia.MediaPresent=FALSE;break;
    case 29:ParentPath.Ufs.Lun=1;break;
    case 30:ParentHandleCount=0;break;
    case 31:ParentHandleCount=2;break;
    case 32:ParentMedia.BlockSize=512;break;
  }
    Esp=(VOID *)0xbad;EFI_STATUS E=PianoEspBootFind(&Esp);assert(E==(I==21||I==22?EFI_COMPROMISED_DATA:EFI_NOT_FOUND)&&!Esp&&HandleFrees==1);
    assert(!Opens&&!Roots&&!Reads&&!Allocations);
  }
  EspSetup();Media.ReadOnly=ParentMedia.ReadOnly=FALSE;
  assert(PianoEspBootPresent()==EFI_SUCCESS&&Opens==1&&!Reads&&!Allocations);
  puts("Actual ESP discovery PASS: fresh UUID/geometry, parent bounds, ambiguity and read-only UKI check; no device");
  return 0;
}
