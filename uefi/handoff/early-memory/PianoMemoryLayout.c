// SPDX-License-Identifier: BSD-2-Clause-Patent
// One bounded parser for the actual factory tree and later Linux reservation
// validation. No allocations, physical address dereferences, or owner claims.
#include "PianoMemoryLayout.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
typedef struct {PIANO_MEMORY_LAYOUT_READ Read;VOID *Context;UINT32 Bytes,Strings,StringBytes;} READER;
STATIC UINT64 Be(CONST UINT8 *P,UINTN N){UINT64 V=0;for(UINTN I=0;I<N;++I)V=(V<<8)|P[I];return V;}
STATIC EFI_STATUS Read(READER *R,UINT32 At,UINTN N,VOID *Out){if(!N||N>256||At>R->Bytes||N>R->Bytes-At)return EFI_COMPROMISED_DATA;return R->Read(R->Context,At,N,Out);}
STATIC EFI_STATUS Word(READER *R,UINT32 At,UINT32 *V){UINT8 B[4];EFI_STATUS S=Read(R,At,4,B);if(S==EFI_SUCCESS)*V=(UINT32)Be(B,4);return S;}
STATIC EFI_STATUS String(READER *R,UINT32 At,UINT32 End,CHAR8 *Out,UINT32 Capacity,UINT32 *Used){
 for(UINT32 X=0;X<Capacity&&At<End&&X<End-At;++X){EFI_STATUS S=Read(R,At+X,1,&Out[X]);if(S!=EFI_SUCCESS)return S;if(!Out[X]){*Used=X+1;return EFI_SUCCESS;}}return EFI_COMPROMISED_DATA;
}
STATIC EFI_STATUS Add(PIANO_MEMORY_SPAN *Rows,UINT32 *Count,UINT32 Max,UINT64 Base,UINT64 Bytes,UINT32 Flags){
 if(!Bytes)return EFI_SUCCESS;if(Base>MAX_UINT64-Bytes||*Count>=Max)return EFI_COMPROMISED_DATA;
 Rows[(*Count)++]=(PIANO_MEMORY_SPAN){Base,Bytes,Flags,0};return EFI_SUCCESS;
}
STATIC EFI_STATUS Pairs(READER *R,UINT32 At,UINT32 N,PIANO_MEMORY_SPAN *Rows,UINT32 *Count,UINT32 Max,UINT32 Flags){
 if(!N||N%16)return EFI_COMPROMISED_DATA;for(UINT32 X=0;X<N;X+=16){UINT8 B[16];EFI_STATUS S=Read(R,At+X,16,B);if(S!=EFI_SUCCESS)return S;S=Add(Rows,Count,Max,Be(B,8),Be(B+8,8),Flags);if(S!=EFI_SUCCESS)return S;}return EFI_SUCCESS;
}
STATIC BOOLEAN SharedPool(CONST CHAR8 *P,UINT32 N){
 CONST CHAR8 Wanted[]="shared-dma-pool";for(UINT32 At=0;At<N;){UINT32 End=At;while(End<N&&P[End])++End;if(End==N)return FALSE;if(End-At==sizeof(Wanted)-1&&!CompareMem(P+At,Wanted,sizeof(Wanted)-1))return TRUE;At=End+1;}return FALSE;
}
EFI_STATUS PianoMemoryLayoutRead(PIANO_MEMORY_LAYOUT_READ Callback,VOID *Context,UINT32 Bytes,PIANO_MEMORY_LAYOUT *L){
 if(!Callback||!L||Bytes<40||Bytes>0x200000)return EFI_INVALID_PARAMETER;ZeroMem(L,sizeof(*L));READER R={Callback,Context,Bytes,0,0};UINT8 H[40];EFI_STATUS S=Read(&R,0,40,H);if(S!=EFI_SUCCESS)return S;
 UINT32 Struct=(UINT32)Be(H+8,4),Strings=(UINT32)Be(H+12,4),Reserve=(UINT32)Be(H+16,4),StringBytes=(UINT32)Be(H+32,4),StructBytes=(UINT32)Be(H+36,4);
 if(Be(H,4)!=0xd00dfeed||Be(H+4,4)!=Bytes||Be(H+20,4)!=17||Be(H+24,4)>17||Struct<40||(Struct&3)||Struct>Bytes||StructBytes>Bytes-Struct||!StructBytes||Strings<40||Strings>Bytes||StringBytes>Bytes-Strings||!StringBytes||Reserve<40||(Reserve&7)||Reserve>Bytes||Bytes-Reserve<16||
    (Struct<Strings+StringBytes&&Strings<Struct+StructBytes))return EFI_COMPROMISED_DATA;
 R.Strings=Strings;R.StringBytes=StringBytes;UINT32 ReserveEnd=MIN(Struct,Strings);BOOLEAN EndReserve=FALSE;
 for(UINT32 At=Reserve;At<=ReserveEnd&&ReserveEnd-At>=16;At+=16){UINT8 B[16];S=Read(&R,At,16,B);if(S!=EFI_SUCCESS)return S;UINT64 Base=Be(B,8),N=Be(B+8,8);if(!Base&&!N){EndReserve=TRUE;break;}S=Add(L->Fixed,&L->ReservedCount,PIANO_MEMORY_LAYOUT_MAX_RESERVED,Base,N,0);if(S!=EFI_SUCCESS)return S;}
 if(!EndReserve)return EFI_COMPROMISED_DATA;
 UINT32 At=Struct,End=Struct+StructBytes,Depth=0,RootAddress=0,RootSize=0,ReservedAddress=0,ReservedSize=0,ReservedNodes=0;
 BOOLEAN Memory=FALSE,Reserved=FALSE,MemoryType=FALSE,Reuse=FALSE,NoMap=FALSE,Pool=FALSE,Enabled=TRUE,Dynamic=FALSE,Done=FALSE;
 UINT32 RegAt=0,RegBytes=0;
 while(At<=End&&End-At>=4){UINT32 Token;S=Word(&R,At,&Token);if(S!=EFI_SUCCESS)return S;At+=4;
  if(Token==1){CHAR8 Name[128];UINT32 Used;S=String(&R,At,End,Name,sizeof(Name),&Used);if(S!=EFI_SUCCESS)return S;if(!Depth&&Name[0])return EFI_COMPROMISED_DATA;
   if(Depth==1){Memory=!AsciiStrCmp(Name,"memory")||(AsciiStrLen(Name)>=7&&!CompareMem(Name,"memory@",7));Reserved=!AsciiStrCmp(Name,"reserved-memory");if(Reserved&&++ReservedNodes!=1)return EFI_COMPROMISED_DATA;MemoryType=FALSE;RegAt=RegBytes=0;}
   if(Reserved&&Depth==2){Reuse=NoMap=Pool=Dynamic=FALSE;Enabled=TRUE;RegAt=RegBytes=0;}
   if(++Depth>64)return EFI_COMPROMISED_DATA;At=ALIGN_VALUE(At+Used,4);
  }else if(Token==2){
   if(!Depth)return EFI_COMPROMISED_DATA;
   if(Depth==2&&Memory){if(!MemoryType)return EFI_COMPROMISED_DATA;S=Pairs(&R,RegAt,RegBytes,L->Ram,&L->RamCount,PIANO_MEMORY_LAYOUT_MAX_RAM,0);if(S!=EFI_SUCCESS)return S;}
   if(Depth==3&&Reserved){if(Reuse&&NoMap)return EFI_COMPROMISED_DATA;if(RegBytes){S=Pairs(&R,RegAt,RegBytes,L->Fixed,&L->ReservedCount,PIANO_MEMORY_LAYOUT_MAX_RESERVED,Enabled&&Reuse&&!NoMap&&Pool?PIANO_MEMORY_LAYOUT_CMA:0);if(S!=EFI_SUCCESS)return S;}else if(Dynamic)++L->FutureLinuxDynamicConstraints;}
   if(Depth==2)Memory=Reserved=FALSE;--Depth;
  }else if(Token==3){UINT32 N,Offset;if(!Depth||End-At<8)return EFI_COMPROMISED_DATA;S=Word(&R,At,&N);if(S!=EFI_SUCCESS)return S;S=Word(&R,At+4,&Offset);if(S!=EFI_SUCCESS)return S;At+=8;if(N>End-At||Offset>=StringBytes)return EFI_COMPROMISED_DATA;
   if(Depth==1||(Memory&&Depth==2)||(Reserved&&(Depth==2||Depth==3))){CHAR8 Name[128];UINT32 Used;S=String(&R,Strings+Offset,Strings+StringBytes,Name,sizeof(Name),&Used);if(S!=EFI_SUCCESS)return S;
    if((Depth==1||(Reserved&&Depth==2))&&(!AsciiStrCmp(Name,"#address-cells")||!AsciiStrCmp(Name,"#size-cells"))){UINT32 V;if(N!=4)return EFI_COMPROMISED_DATA;S=Word(&R,At,&V);if(S!=EFI_SUCCESS)return S;if(Depth==1){if(!AsciiStrCmp(Name,"#address-cells"))RootAddress=V;else RootSize=V;}else{if(!AsciiStrCmp(Name,"#address-cells"))ReservedAddress=V;else ReservedSize=V;}}
    else if(Memory&&Depth==2&&!AsciiStrCmp(Name,"device_type")){CHAR8 Value[7];if(N!=7)return EFI_COMPROMISED_DATA;S=Read(&R,At,N,Value);if(S!=EFI_SUCCESS)return S;MemoryType=!CompareMem(Value,"memory",7);}
    else if(((Memory&&Depth==2)||(Reserved&&Depth==3))&&!AsciiStrCmp(Name,"reg")){if(RegBytes)return EFI_COMPROMISED_DATA;RegAt=At;RegBytes=N;}
    else if(Reserved&&Depth==2&&!AsciiStrCmp(Name,"ranges")&&N)return EFI_UNSUPPORTED;
    else if(Reserved&&Depth==3){
     if(!AsciiStrCmp(Name,"reusable")){if(N)return EFI_COMPROMISED_DATA;Reuse=TRUE;}
     else if(!AsciiStrCmp(Name,"no-map")){if(N)return EFI_COMPROMISED_DATA;NoMap=TRUE;}
     else if(!AsciiStrCmp(Name,"size")){UINT8 Value[8];if(N!=8)return EFI_COMPROMISED_DATA;S=Read(&R,At,N,Value);if(S!=EFI_SUCCESS)return S;if(!Be(Value,8))return EFI_COMPROMISED_DATA;Dynamic=TRUE;}
     else if(!AsciiStrCmp(Name,"compatible")){CHAR8 Value[256];if(!N||N>sizeof(Value))return EFI_COMPROMISED_DATA;S=Read(&R,At,N,Value);if(S!=EFI_SUCCESS)return S;Pool=SharedPool(Value,N);}
     else if(!AsciiStrCmp(Name,"status")){CHAR8 Value[16];if(!N||N>sizeof(Value))return EFI_COMPROMISED_DATA;S=Read(&R,At,N,Value);if(S!=EFI_SUCCESS)return S;Enabled=(N==5&&!CompareMem(Value,"okay",5))||(N==3&&!CompareMem(Value,"ok",3));}
    }
   }At=ALIGN_VALUE(At+N,4);if(At>End)return EFI_COMPROMISED_DATA;
  }else if(Token==4)continue;
  else if(Token==9){if(Depth)return EFI_COMPROMISED_DATA;Done=TRUE;break;}
  else return EFI_COMPROMISED_DATA;
 }
 if(!Done||!L->RamCount||RootAddress!=2||RootSize!=2||ReservedNodes!=1||ReservedAddress!=2||ReservedSize!=2)return EFI_COMPROMISED_DATA;
 for(UINT32 I=0;I<L->RamCount;++I){PIANO_MEMORY_SPAN *A=&L->Ram[I];if(A->Bytes>MAX_UINT64-L->DramBytes)return EFI_COMPROMISED_DATA;L->DramBytes+=A->Bytes;for(UINT32 J=0;J<I;++J){PIANO_MEMORY_SPAN *B=&L->Ram[J];if(A->Base<B->Base+B->Bytes&&B->Base<A->Base+A->Bytes)return EFI_COMPROMISED_DATA;}}
 return EFI_SUCCESS;
}
