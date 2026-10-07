// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoSmemDescriptor.h"
#include <Library/BaseMemoryLib.h>
STATIC UINT16 D16(CONST UINT8*P){return P[0]|((UINT16)P[1]<<8);}
STATIC UINT32 D32(CONST UINT8*P){return D16(P)|((UINT32)D16(P+2)<<16);}
STATIC UINT64 D64(CONST UINT8*P){return D32(P)|((UINT64)D32(P+4)<<32);}
STATIC BOOLEAN DBounds(UINT64 Address,UINTN Bytes){return Address>=PIANO_SMEM_BASE&&Address-PIANO_SMEM_BASE<PIANO_SMEM_BYTES&&Bytes<=PIANO_SMEM_BYTES-(Address-PIANO_SMEM_BASE);}
STATIC BOOLEAN DAlias(CONST VOID*A,UINTN An,CONST VOID*B,UINTN Bn){UINTN X=(UINTN)A,Y=(UINTN)B;return X>MAX_UINTN-An||Y>MAX_UINTN-Bn||(X<Y+Bn&&Y<X+An);}
STATIC EFI_STATUS DRead(CONST PIANO_SMEM_READER*R,UINT64 Address,UINTN Bytes,VOID*Out){
 if(!Bytes||Bytes>PIANO_SMEM_READ_MAX||(Address&3)||(Bytes&3)||!DBounds(Address,Bytes))return EFI_ACCESS_DENIED;
 EFI_STATUS E=R->TryRead(R->Context,Address,Bytes,Out);return E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS DSnapshot(CONST PIANO_SMEM_READER*R,UINT64 Address,UINT8*Out,UINT32*Bytes){
 EFI_STATUS E=DRead(R,Address,20,Out);if(E!=EFI_SUCCESS)return E;
 if(D32(Out)!=0x49494953U)return EFI_COMPROMISED_DATA;
 UINT16 Count=D16(Out+18);if(Count>64)return EFI_BAD_BUFFER_SIZE;
 UINT32 At=20;
 for(UINT16 I=0;I<Count;++I){
  if(At>PIANO_SMEM_DESCRIPTOR_MAX-4)return EFI_BAD_BUFFER_SIZE;
  E=DRead(R,Address+At,4,Out+At);if(E!=EFI_SUCCESS)return E;
  UINT16 Length=D16(Out+At+2);
  // Native bf/c0 walker advances by this exact TLV length (header included).
  // Unaligned future encodings remain unsupported by the aligned SEC adapter.
  if(Length<4||(Length&3)||Length>PIANO_SMEM_DESCRIPTOR_MAX-At)return EFI_COMPROMISED_DATA;
  UINT32 Copied=4;
  while(Copied<Length){UINT32 N=MIN(PIANO_SMEM_READ_MAX,Length-Copied);
   E=DRead(R,Address+At+Copied,N,Out+At+Copied);if(E!=EFI_SUCCESS)return E;Copied+=N;}
  At+=Length;
 }
 *Bytes=At;return EFI_SUCCESS;
}
EFI_STATUS PianoSmemDescriptorCollect(CONST PIANO_SMEM_READER*R,UINT64 Cookie,PIANO_SMEM_DESCRIPTOR_WORK*W,PIANO_SMEM_DESCRIPTOR_REPORT*P){
 if(!R||!R->TryRead||!W||!P||DAlias(R,sizeof(*R),W,sizeof(*W))||DAlias(R,sizeof(*R),P,sizeof(*P))||DAlias(W,sizeof(*W),P,sizeof(*P)))return EFI_INVALID_PARAMETER;if(W->Busy)return EFI_ALREADY_STARTED;
 ZeroMem(P,sizeof(*P));P->Address=Cookie;
 if(!DBounds(Cookie,20)||(Cookie&3))return P->Status=EFI_ACCESS_DENIED;
 W->Busy=TRUE;UINT32 A=0,B=0;EFI_STATUS E=DSnapshot(R,Cookie,W->Bytes[0],&A);
 if(E==EFI_SUCCESS)E=DSnapshot(R,Cookie,W->Bytes[1],&B);
 if(E==EFI_SUCCESS&&(A!=B||CompareMem(W->Bytes[0],W->Bytes[1],A)))E=EFI_MEDIA_CHANGED;
 if(E==EFI_SUCCESS){
  P->RepeatedEqual=TRUE;P->SnapshotBytes=A;P->SmemBytes=D32(W->Bytes[0]+4);P->SmemBase=D64(W->Bytes[0]+8);
  P->ItemCount=D16(W->Bytes[0]+16);P->TlvCount=D16(W->Bytes[0]+18);
  P->RegionMatchesKnownWindow=P->SmemBase==PIANO_SMEM_BASE&&P->SmemBytes==PIANO_SMEM_BYTES;
  CopyMem(P->Prefix,W->Bytes[0],MIN((UINT32)sizeof(P->Prefix),A));
  UINT32 At=20;for(UINT16 I=0;I<P->TlvCount;++I){UINT16 L=D16(W->Bytes[0]+At+2);
    if(D16(W->Bytes[0]+At)==0x4853){if(P->HostInfoBytes){E=EFI_COMPROMISED_DATA;break;}P->HostInfoBytes=L;}At+=L;}
  UINT32 C=0xffffffffU;for(UINT32 I=0;I<A;++I){C^=W->Bytes[0][I];for(UINTN J=0;J<8;++J)C=(C>>1)^((C&1)?0xedb88320U:0);}P->Crc32=~C;
 }
 W->Busy=FALSE;return P->Status=E;
}
