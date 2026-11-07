// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synthetic THP payloads through the real matrix decoder: type lock, median
// reference, line-noise removal, centroid, screen mapping and release
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "PianoTouchMatrix.h"

#define LEN 5160U
#define ROWS 40U
#define COLS 60U
static UINT8 P[LEN];
static PIANO_TOUCH_MATRIX M;

static void Put16(UINTN O,UINT16 V){P[O]=(UINT8)V;P[O+1]=(UINT8)(V>>8);}
static void Put32(UINTN O,UINT32 V){Put16(O,(UINT16)V);Put16(O+2,(UINT16)(V>>16));}
// Baseline 1000 with deterministic +-4 noise, plus Extra(r, c)
static void Frame(UINT8 Type,UINT16 No,INT32 (*Extra)(UINT32,UINT32),BOOLEAN BadSum){
  memset(P,0,sizeof(P));P[48]=COLS;P[49]=ROWS;P[56]=Type;Put16(28,No);
  for(UINT32 R=0;R<ROWS;++R)for(UINT32 C=0;C<COLS;++C)
    Put16(64+(R*COLS+C)*2,(UINT16)(1000+(INT32)((R*7+C*13+No*5)%9)-4+(Extra?Extra(R,C):0)));
  UINT32 Words=(LEN-20)/4;UINT32 Sum=0;
  for(UINT32 I=0;I<Words*2;++I)Sum+=P[20+I*2]|((UINT32)P[21+I*2]<<8);
  UINT16 Csum=(UINT16)(~Sum+1)^(BadSum?1:0);
  Put16(4,Csum);Put32(8,Words);Put16(12,(UINT16)~Csum);Put32(16,~Words);
}
static INT32 Blob(UINT32 R,UINT32 C){
  if(R==10 && C==45)return 600;
  return (R>=9 && R<=11 && C>=44 && C<=46)?300:0;
}
static INT32 BlobAndNoisyRow(UINT32 R,UINT32 C){return Blob(R,C)+(R==30?150:0);}
static INT32 Drift(UINT32 R,UINT32 C){(void)R;(void)C;return 40;}

int main(void){
  UINT16 No=0;
  PianoTouchMatrixReset(&M);
  Frame(1,No++,NULL,TRUE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_NOT_READY && M.BadChecksum==1);
  // Pen frames never become the reference type
  for(UINT32 N=0;N<8;++N){Frame(6,No++,NULL,FALSE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_NOT_READY);}
  for(UINT32 N=0;N<PIANO_TOUCH_PROBE_FRAMES;++N){Frame(1,No++,NULL,FALSE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_NOT_READY);}
  assert(M.RefType==1);
  for(UINT32 N=0;N<PIANO_TOUCH_REF_FRAMES;++N){Frame(1,No++,NULL,FALSE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_NOT_READY);}
  assert(M.HaveReference && M.Rows==ROWS && M.Cols==COLS);
  Frame(1,No++,NULL,FALSE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_SUCCESS && !M.Touching);
  // Symmetric blob at row 10, column 45: centroid at the node centre, Y flipped
  Frame(1,No++,BlobAndNoisyRow,FALSE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_SUCCESS && M.Touching);
  INT32 X=(INT32)((45*1024+512)*3200LL/(COLS*1024)),Y=2136-(INT32)((10*1024+512)*2136LL/(ROWS*1024));
  printf("touch x=%d y=%d expected %d %d\n",M.X,M.Y,X,Y);
  assert(M.X>=X-2 && M.X<=X+2 && M.Y>=Y-2 && M.Y<=Y+2);
  Frame(2,No++,Blob,FALSE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_NOT_READY && M.Touching);
  Frame(1,No++,NULL,FALSE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_SUCCESS && !M.Touching);
  // A uniform baseline shift is not a touch
  for(UINT32 N=0;N<20;++N){Frame(1,No++,Drift,FALSE);assert(PianoTouchMatrixFeed(&M,P,LEN)==EFI_SUCCESS && !M.Touching);}
  assert(PianoTouchPayloadChecksum(P,10)==-1 && PianoTouchMatrixFeed(&M,P,32)==EFI_NOT_READY);
  printf("Touch matrix: %u frames decoded\n",M.Frames);
  return 0;
}
