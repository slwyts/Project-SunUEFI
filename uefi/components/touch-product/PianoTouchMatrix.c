// SPDX-License-Identifier: BSD-2-Clause-Patent
// Integer port of the touch-view decoding steps
#include "PianoTouchMatrix.h"

STATIC UINT16 Le16(CONST UINT8 *P){return (UINT16)(P[0]|(P[1]<<8));}
STATIC UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}

VOID PianoTouchMatrixReset(PIANO_TOUCH_MATRIX *M) {
  if(M==NULL)return;
  for(UINTN I=0;I<sizeof(*M);++I)((UINT8 *)M)[I]=0;
  M->RefType=-1;
}

INTN PianoTouchPayloadChecksum(CONST UINT8 *P,UINTN Len) {
  if(P==NULL || Len<20)return -1;
  UINT16 Csum=Le16(P+4),CsumInv=Le16(P+12);
  INT32 CrcLen=(INT32)Le32(P+8),CrcLenInv=(INT32)Le32(P+16);
  if(CsumInv!=(UINT16)~Csum || CrcLenInv!=~CrcLen)return -1;
  if(CrcLen<=0 || 20+(UINT64)CrcLen*4>Len)return -1;
  UINT32 Sum=0;
  for(UINT64 I=0;I<(UINT64)CrcLen*2;++I)Sum+=Le16(P+20+I*2);
  return (UINT16)(~Sum+1)==Csum?1:0;
}

STATIC VOID Sort(INT32 *V,UINTN N) {
  for(UINTN I=1;I<N;++I){INT32 X=V[I];UINTN J=I;while(J>0 && V[J-1]>X){V[J]=V[J-1];--J;}V[J]=X;}
}

STATIC VOID BuildReference(PIANO_TOUCH_MATRIX *M) {
  INT32 Column[PIANO_TOUCH_REF_FRAMES];
  for(UINT32 N=0;N<M->Nodes;++N) {
    for(UINT32 K=0;K<M->RefCount;++K)Column[K]=M->Samples[K][N];
    Sort(Column,M->RefCount);M->Reference[N]=Column[M->RefCount/2];
  }
  M->HaveReference=TRUE;
}

// Line noise: subtract the per-column median, then the per-row median
STATIC VOID RemoveLineNoise(PIANO_TOUCH_MATRIX *M) {
  INT32 Line[256];
  for(UINT32 C=0;C<M->Cols;++C) {
    for(UINT32 R=0;R<M->Rows;++R)Line[R]=M->Delta[R*M->Cols+C];
    Sort(Line,M->Rows);INT32 Common=Line[M->Rows/2];
    for(UINT32 R=0;R<M->Rows;++R)M->Delta[R*M->Cols+C]-=Common;
  }
  for(UINT32 R=0;R<M->Rows;++R) {
    for(UINT32 C=0;C<M->Cols;++C)Line[C]=M->Delta[R*M->Cols+C];
    Sort(Line,M->Cols);INT32 Common=Line[M->Cols/2];
    for(UINT32 C=0;C<M->Cols;++C)M->Delta[R*M->Cols+C]-=Common;
  }
  M->MaxDelta=0;
  for(UINT32 N=0;N<M->Nodes;++N)if(M->Delta[N]>M->MaxDelta)M->MaxDelta=M->Delta[N];
}

typedef struct {INT64 X,Y;INT32 Peak;} POINT;   // 1/1024 sensor pitch
#define MAX_POINTS 10U

STATIC UINT32 FindPoints(PIANO_TOUCH_MATRIX *M,POINT *Pts) {
  UINT32 Count=0;
  for(INT32 R=0;R<(INT32)M->Rows;++R)for(INT32 C=0;C<(INT32)M->Cols;++C) {
    INT32 V=M->Delta[R*M->Cols+C];BOOLEAN Peak=TRUE;
    if(V<PIANO_TOUCH_THRESHOLD)continue;
    for(INT32 Dr=-1;Dr<=1 && Peak;++Dr)for(INT32 Dc=-1;Dc<=1;++Dc) {
      INT32 Rr=R+Dr,Cc=C+Dc;
      if((!Dr && !Dc) || Rr<0 || Cc<0 || Rr>=(INT32)M->Rows || Cc>=(INT32)M->Cols)continue;
      INT32 W=M->Delta[Rr*M->Cols+Cc];
      // Ties go to the first node in scan order
      if(W>V || (W==V && (Dr<0 || (Dr==0 && Dc<0)))){Peak=FALSE;break;}
    }
    if(!Peak)continue;
    INT64 Sw=0,Sx=0,Sy=0;
    for(INT32 Dr=-1;Dr<=1;++Dr)for(INT32 Dc=-1;Dc<=1;++Dc) {
      INT32 Rr=R+Dr,Cc=C+Dc;
      if(Rr<0 || Cc<0 || Rr>=(INT32)M->Rows || Cc>=(INT32)M->Cols)continue;
      INT32 W=M->Delta[Rr*M->Cols+Cc];if(W<=0)continue;
      Sw+=W;Sx+=(INT64)W*(Cc*1024+512);Sy+=(INT64)W*(Rr*1024+512);
    }
    if(Count<MAX_POINTS && Sw>0){Pts[Count].X=Sx/Sw;Pts[Count].Y=Sy/Sw;Pts[Count].Peak=V;++Count;}
  }
  return Count;
}

// touch-view to_screen defaults: no swap, no X flip, Y flipped
STATIC VOID ToScreen(PIANO_TOUCH_MATRIX *M,CONST POINT *P,INT32 *X,INT32 *Y) {
  INT64 U=P->X*PIANO_TOUCH_SCREEN_W/((INT64)M->Cols*1024);
  INT64 V=PIANO_TOUCH_SCREEN_H-P->Y*PIANO_TOUCH_SCREEN_H/((INT64)M->Rows*1024);
  *X=(INT32)(U<0?0:U>=PIANO_TOUCH_SCREEN_W?PIANO_TOUCH_SCREEN_W-1:U);
  *Y=(INT32)(V<0?0:V>=PIANO_TOUCH_SCREEN_H?PIANO_TOUCH_SCREEN_H-1:V);
}

EFI_STATUS PianoTouchMatrixFeed(PIANO_TOUCH_MATRIX *M,CONST UINT8 *P,UINTN Len) {
  if(M==NULL || P==NULL)return EFI_INVALID_PARAMETER;
  if(Len<64){++M->Skipped;return EFI_NOT_READY;}
  INTN Csum=PianoTouchPayloadChecksum(P,Len);
  UINT32 Type=P[56],Cols=P[48],Rows=P[49],Nodes=Rows*Cols;
  if(Csum<=0){++M->BadChecksum;return EFI_NOT_READY;}
  if(!Nodes || Nodes>PIANO_TOUCH_MAX_NODES || 64+(UINT64)Nodes*2>Len){++M->Skipped;return EFI_NOT_READY;}
  M->LastFrameNo=Le16(P+28);
  if(M->RefType<0) {
    // Lock onto the dominant non-pen matrix type
    if(Type==6 || Type==7 || Type==9 || Type==0x1D)return EFI_NOT_READY;
    ++M->ProbeTypes[Type];
    if(++M->ProbeCount<PIANO_TOUCH_PROBE_FRAMES)return EFI_NOT_READY;
    for(UINT32 N=0;N<256;++N)if(M->RefType<0 || M->ProbeTypes[N]>M->ProbeTypes[M->RefType])M->RefType=(INT32)N;
    return EFI_NOT_READY;
  }
  if((INT32)Type!=M->RefType)return EFI_NOT_READY;
  if(!M->HaveReference) {
    if(!M->RefCount){M->Rows=Rows;M->Cols=Cols;M->Nodes=Nodes;}
    if(Rows!=M->Rows || Cols!=M->Cols)return EFI_NOT_READY;
    for(UINT32 N=0;N<Nodes;++N)M->Samples[M->RefCount][N]=(INT16)Le16(P+64+N*2);
    if(++M->RefCount>=PIANO_TOUCH_REF_FRAMES)BuildReference(M);
    return EFI_NOT_READY;
  }
  if(Rows!=M->Rows || Cols!=M->Cols)return EFI_NOT_READY;
  for(UINT32 N=0;N<Nodes;++N)M->Delta[N]=(INT16)Le16(P+64+N*2)-M->Reference[N];
  RemoveLineNoise(M);
  // Follow baseline drift while nothing is near the threshold
  if(M->MaxDelta<PIANO_TOUCH_THRESHOLD/2)
    for(UINT32 N=0;N<Nodes;++N)M->Reference[N]+=((INT16)Le16(P+64+N*2)-M->Reference[N])/8;
  POINT Pts[MAX_POINTS];UINT32 Count=FindPoints(M,Pts);
  ++M->Frames;
  if(!Count){M->Touching=FALSE;return EFI_SUCCESS;}
  // Track the contact nearest the last one, else take the strongest peak
  INT32 BestX=0,BestY=0;BOOLEAN Found=FALSE;INT64 BestD=(INT64)PIANO_TOUCH_TRACK_DIST*PIANO_TOUCH_TRACK_DIST;INT32 BestPeak=-1;
  for(UINT32 I=0;I<Count;++I) {
    INT32 X,Y;ToScreen(M,&Pts[I],&X,&Y);
    if(M->Touching) {
      INT64 D=(INT64)(X-M->X)*(X-M->X)+(INT64)(Y-M->Y)*(Y-M->Y);
      if(D<BestD){BestD=D;BestX=X;BestY=Y;Found=TRUE;}
    } else if(Pts[I].Peak>BestPeak){BestPeak=Pts[I].Peak;BestX=X;BestY=Y;Found=TRUE;}
  }
  if(!Found)for(UINT32 I=0;I<Count;++I)if(Pts[I].Peak>BestPeak){BestPeak=Pts[I].Peak;ToScreen(M,&Pts[I],&BestX,&BestY);Found=TRUE;}
  M->Touching=Found;M->X=BestX;M->Y=BestY;
  return EFI_SUCCESS;
}
