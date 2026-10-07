// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoNvFvb.h"
#include <Guid/VariableFormat.h>
#include <Guid/SystemNvDataGuid.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#define SIG SIGNATURE_32('P','N','F','V')
STATIC EFI_GUID NvGuid=EFI_SYSTEM_NV_DATA_FV_GUID,VarGuid=EFI_AUTHENTICATED_VARIABLE_GUID;
STATIC PIANO_NV_FVB *Owner(CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *P){if(!P)return NULL;PIANO_NV_FVB *S=BASE_CR(P,PIANO_NV_FVB,Protocol);return S->Signature==SIG?S:NULL;}
STATIC EFI_STATUS EFIAPI Attr(CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *P,EFI_FVB_ATTRIBUTES_2 *A){PIANO_NV_FVB*S=Owner(P);if(!S||!A)return EFI_INVALID_PARAMETER;*A=S->Attributes;if(S->Runtime)*A&=~EFI_FVB2_WRITE_STATUS;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI SetAttr(CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *P,EFI_FVB_ATTRIBUTES_2 *A){PIANO_NV_FVB*S=Owner(P);if(!S||!A)return EFI_INVALID_PARAMETER;if(S->Runtime||S->Busy)return EFI_ACCESS_DENIED;if(((*A^S->Attributes)&~(EFI_FVB2_READ_STATUS|EFI_FVB2_WRITE_STATUS))!=0)return EFI_INVALID_PARAMETER;S->Attributes=*A;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Phys(CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *P,EFI_PHYSICAL_ADDRESS *A){PIANO_NV_FVB*S=Owner(P);if(!S||!A)return EFI_INVALID_PARAMETER;*A=S->PhysicalBase;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI BlockSize(CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *P,EFI_LBA Lba,UINTN *Bytes,UINTN *Count){PIANO_NV_FVB*S=Owner(P);if(!S||!Bytes||!Count||Lba>=PIANO_NV_SNAPSHOT_BLOCKS)return EFI_INVALID_PARAMETER;*Bytes=4096;*Count=PIANO_NV_SNAPSHOT_BLOCKS-(UINTN)Lba;return EFI_SUCCESS;}
STATIC EFI_STATUS Bounds(PIANO_NV_FVB*S,EFI_LBA Lba,UINTN Offset,UINTN *Bytes,CONST VOID *Buffer){if(!S||!Bytes||!Buffer||!*Bytes||Lba>=PIANO_NV_SNAPSHOT_BLOCKS||Offset>=4096)return EFI_INVALID_PARAMETER;if(*Bytes>4096-Offset){*Bytes=4096-Offset;return EFI_BAD_BUFFER_SIZE;}return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Read(CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *P,EFI_LBA Lba,UINTN Offset,UINTN *Bytes,UINT8 *Buffer){PIANO_NV_FVB*S=Owner(P);EFI_STATUS E=Bounds(S,Lba,Offset,Bytes,Buffer);if(E!=EFI_SUCCESS&&E!=EFI_BAD_BUFFER_SIZE)return E;if(S->ConversionFailed||!(S->Attributes&EFI_FVB2_READ_STATUS)||S->Busy)return EFI_ACCESS_DENIED;CopyMem(Buffer,S->Journal->Mirror+(UINTN)Lba*4096+Offset,*Bytes);return E;}
STATIC EFI_STATUS EFIAPI Write(CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *P,EFI_LBA Lba,UINTN Offset,UINTN *Bytes,UINT8 *Buffer){
  PIANO_NV_FVB*S=Owner(P);if(S&&S->Runtime)return EFI_UNSUPPORTED;
  EFI_STATUS E=Bounds(S,Lba,Offset,Bytes,Buffer);if(E!=EFI_SUCCESS&&E!=EFI_BAD_BUFFER_SIZE)return E;
  if(!(S->Attributes&EFI_FVB2_WRITE_STATUS)||S->Busy||!S->Journal->Ready)return EFI_ACCESS_DENIED;
  UINTN At=(UINTN)Lba*4096+Offset;for(UINTN I=0;I<*Bytes;++I)if((S->Journal->Mirror[At+I]&Buffer[I])!=Buffer[I])return EFI_WRITE_PROTECTED;
  S->Busy=TRUE;CopyMem(S->Journal->Scratch,S->Journal->Mirror,PIANO_NV_SNAPSHOT_BYTES);CopyMem(S->Journal->Scratch+At,Buffer,*Bytes);
  EFI_STATUS Commit=PianoNvJournalCommit(S->Journal,S->Journal->Scratch);S->Busy=FALSE;return Commit==EFI_SUCCESS?E:Commit;
}
STATIC EFI_STATUS EFIAPI Erase(CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *P,...){
  PIANO_NV_FVB*S=Owner(P);if(!S)return EFI_INVALID_PARAMETER;if(S->Runtime)return EFI_UNSUPPORTED;
  if(!(S->Attributes&EFI_FVB2_WRITE_STATUS)||S->Busy||!S->Journal->Ready)return EFI_ACCESS_DENIED;
  // Validate the complete vararg list before even changing the candidate.
  VA_LIST Args;VA_START(Args,P);UINTN Pairs=0;EFI_LBA Lba;UINTN Count;
  for(;;){Lba=VA_ARG(Args,EFI_LBA);if(Lba==EFI_LBA_LIST_TERMINATOR)break;Count=VA_ARG(Args,UINTN);if(++Pairs>PIANO_NV_SNAPSHOT_BLOCKS||!Count||Lba>=PIANO_NV_SNAPSHOT_BLOCKS||Count>PIANO_NV_SNAPSHOT_BLOCKS-Lba){VA_END(Args);return EFI_INVALID_PARAMETER;}}
  VA_END(Args);if(!Pairs)return EFI_INVALID_PARAMETER;S->Busy=TRUE;CopyMem(S->Journal->Scratch,S->Journal->Mirror,PIANO_NV_SNAPSHOT_BYTES);
  VA_START(Args,P);for(UINTN I=0;I<Pairs;++I){Lba=VA_ARG(Args,EFI_LBA);Count=VA_ARG(Args,UINTN);SetMem(S->Journal->Scratch+(UINTN)Lba*4096,Count*4096,0xff);}VA_END(Args);
  EFI_STATUS E=PianoNvJournalCommit(S->Journal,S->Journal->Scratch);S->Busy=FALSE;return E;
}
EFI_STATUS PianoNvFvbBuildBlank(VOID *Buffer,UINTN Bytes){
  if(!Buffer||Bytes!=PIANO_NV_SNAPSHOT_BYTES)return EFI_INVALID_PARAMETER;SetMem(Buffer,Bytes,0xff);
  EFI_FIRMWARE_VOLUME_HEADER *H=Buffer;UINTN HeaderBytes=sizeof(*H)+sizeof(EFI_FV_BLOCK_MAP_ENTRY);ZeroMem(H,HeaderBytes);
  H->FileSystemGuid=NvGuid;H->FvLength=Bytes;H->Signature=EFI_FVH_SIGNATURE;H->HeaderLength=(UINT16)HeaderBytes;H->Revision=2;
  H->Attributes=EFI_FVB2_READ_DISABLED_CAP|EFI_FVB2_WRITE_DISABLED_CAP|EFI_FVB2_READ_ENABLED_CAP|EFI_FVB2_READ_STATUS|EFI_FVB2_WRITE_ENABLED_CAP|EFI_FVB2_WRITE_STATUS|EFI_FVB2_MEMORY_MAPPED|EFI_FVB2_ERASE_POLARITY|EFI_FVB2_ALIGNMENT_4K;
  H->BlockMap[0]=(EFI_FV_BLOCK_MAP_ENTRY){PIANO_NV_SNAPSHOT_BLOCKS,4096};H->BlockMap[1]=(EFI_FV_BLOCK_MAP_ENTRY){0,0};
  UINT16 Sum=0;for(UINTN I=0;I<HeaderBytes/2;++I)Sum=(UINT16)(Sum+((UINT16*)H)[I]);H->Checksum=(UINT16)(0-Sum);
  VARIABLE_STORE_HEADER *V=(VOID*)((UINT8*)H+HeaderBytes);ZeroMem(V,sizeof(*V));V->Signature=VarGuid;V->Size=PIANO_NV_VARIABLE_BYTES-(UINT32)HeaderBytes;V->Format=VARIABLE_STORE_FORMATTED;V->State=VARIABLE_STORE_HEALTHY;return EFI_SUCCESS;
}
EFI_STATUS PianoNvFvbInitialize(PIANO_NV_FVB *S,PIANO_NV_JOURNAL *J){
  if(!S||!J||!J->Mirror||((UINTN)J->Mirror&4095)||!J->Ready||J->Runtime||J->Dirty||J->Quarantined)return EFI_NOT_READY;if(S->Signature)return EFI_ALREADY_STARTED;
  EFI_FIRMWARE_VOLUME_HEADER *H=(VOID*)J->Mirror;UINTN N=sizeof(*H)+sizeof(EFI_FV_BLOCK_MAP_ENTRY);UINT16 Sum=0;
  if(H->Signature!=EFI_FVH_SIGNATURE||H->HeaderLength!=N||H->FvLength!=PIANO_NV_SNAPSHOT_BYTES||CompareMem(&H->FileSystemGuid,&NvGuid,sizeof(NvGuid))||
     H->BlockMap[0].NumBlocks!=PIANO_NV_SNAPSHOT_BLOCKS||H->BlockMap[0].Length!=4096||H->BlockMap[1].NumBlocks||H->BlockMap[1].Length)return EFI_VOLUME_CORRUPTED;
  for(UINTN I=0;I<N/2;++I)Sum=(UINT16)(Sum+((UINT16*)H)[I]);if(Sum)return EFI_CRC_ERROR;
  VARIABLE_STORE_HEADER *V=(VOID*)((UINT8*)H+N);if(CompareMem(&V->Signature,&VarGuid,sizeof(VarGuid))||V->Size!=PIANO_NV_VARIABLE_BYTES-N||V->Format!=VARIABLE_STORE_FORMATTED||V->State!=VARIABLE_STORE_HEALTHY)return EFI_VOLUME_CORRUPTED;
  ZeroMem(S,sizeof(*S));S->Signature=SIG;S->Journal=J;S->Attributes=H->Attributes;S->PhysicalBase=(UINTN)J->Mirror;
  S->Protocol=(EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL){Attr,SetAttr,Phys,BlockSize,Read,Write,Erase,NULL};return EFI_SUCCESS;
}
VOID PianoNvFvbFenceRuntime(PIANO_NV_FVB *S){if(S&&S->Signature==SIG){S->Runtime=TRUE;PianoNvJournalFenceRuntime(S->Journal);}}
EFI_STATUS PianoNvFvbConvertVirtual(PIANO_NV_FVB *S,EFI_CONVERT_POINTER Convert){
  if(!S||S->Signature!=SIG||!S->Runtime||!Convert)return EFI_INVALID_PARAMETER;
  if(S->VirtualConverted||S->ConversionFailed)return EFI_ALREADY_STARTED;
  // Boot-only backing callbacks are gone. Converted Read methods use mirror only.
  ZeroMem(&S->Journal->Io,sizeof(S->Journal->Io));
  EFI_STATUS E=Convert(0,(VOID**)&S->Journal->Mirror);if(E!=EFI_SUCCESS){S->ConversionFailed=TRUE;return E;}
  S->Journal->Scratch=NULL;
  E=Convert(0,(VOID**)&S->Journal);if(E!=EFI_SUCCESS){S->ConversionFailed=TRUE;return E;}
  VOID **Fns[]={ (VOID**)&S->Protocol.GetAttributes,(VOID**)&S->Protocol.SetAttributes,(VOID**)&S->Protocol.GetPhysicalAddress,(VOID**)&S->Protocol.GetBlockSize,(VOID**)&S->Protocol.Read,(VOID**)&S->Protocol.Write,(VOID**)&S->Protocol.EraseBlocks };
  for(UINTN I=0;I<ARRAY_SIZE(Fns);++I){E=Convert(0,Fns[I]);if(E!=EFI_SUCCESS){S->ConversionFailed=TRUE;return E;}}S->VirtualConverted=TRUE;return EFI_SUCCESS;
}
EFI_STATUS PianoNvRuntimeWriteCheck(BOOLEAN Runtime,UINT32 Attributes,BOOLEAN ExistingNv){return Runtime&&((Attributes&EFI_VARIABLE_NON_VOLATILE)||ExistingNv)?EFI_UNSUPPORTED:EFI_SUCCESS;}
