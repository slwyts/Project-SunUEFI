// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "ProductSplash.h"
#include "ProductSplashAssets.h"

typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  PIANO_SPLASH_ALIVE Alive;
  PIANO_SPLASH_REPORT *Report;
} CANVAS;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color(UINT8 R,UINT8 G,UINT8 B) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL P={B,G,R,0};return P;
}
STATIC EFI_STATUS Rect(CANVAS *C,EFI_GRAPHICS_OUTPUT_BLT_PIXEL P,UINT32 X,UINT32 Y,UINT32 W,UINT32 H) {
  if(!W||!H)return EFI_SUCCESS;
  if(X>=C->Report->Width||Y>=C->Report->Height||W>C->Report->Width-X||H>C->Report->Height-Y)return EFI_BAD_BUFFER_SIZE;
  if(!C->Alive())return EFI_ABORTED;
  ++C->Report->DrawCalls;
  EFI_STATUS S=C->Gop->Blt(C->Gop,&P,EfiBltVideoFill,0,0,X,Y,W,H,0);
  if(!C->Alive())return EFI_ABORTED;
  return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
}
STATIC UINT32 Sqrt(UINT32 N) {
  UINT32 Root=0,Bit=1U<<30;
  while(Bit>N)Bit>>=2;
  while(Bit){if(N>=Root+Bit){N-=Root+Bit;Root=(Root>>1)+Bit;}else Root>>=1;Bit>>=2;}
  return Root;
}
STATIC EFI_STATUS Disc(CANVAS *C,UINT32 X,UINT32 Y,UINT32 Radius,EFI_GRAPHICS_OUTPUT_BLT_PIXEL P) {
  for(INT32 Dy=-(INT32)Radius;Dy<=(INT32)Radius;++Dy){
    UINT32 Dx=Sqrt(Radius*Radius-(UINT32)(Dy*Dy));
    EFI_STATUS S=Rect(C,P,X-Dx,(UINT32)((INT32)Y+Dy),2*Dx+1,1);if(S!=EFI_SUCCESS)return S;
  }return EFI_SUCCESS;
}
STATIC EFI_STATUS Polygon(CANVAS *C,CONST SPLASH_POINT V[4],EFI_GRAPHICS_OUTPUT_BLT_PIXEL P) {
  INT32 Top=V[0].Y,Bottom=Top;
  for(UINTN I=1;I<4;++I){if(V[I].Y<Top)Top=V[I].Y;if(V[I].Y>Bottom)Bottom=V[I].Y;}
  for(INT32 Y=Top;Y<Bottom;++Y){
    INT32 Left=MAX_INT32,Right=MIN_INT32;
    for(UINTN I=0;I<4;++I){SPLASH_POINT A=V[I],B=V[(I+1)%4];
      if(A.Y>B.Y){SPLASH_POINT T=A;A=B;B=T;}
      if(A.Y==B.Y||Y<A.Y||Y>=B.Y)continue;
      INT32 X=A.X+(INT32)(((INT64)(B.X-A.X)*(Y-A.Y))/(B.Y-A.Y));
      if(X<Left)Left=X;if(X>Right)Right=X;
    }
    if(Left<=Right){EFI_STATUS S=Rect(C,P,(UINT32)Left,(UINT32)Y,(UINT32)(Right-Left+1),1);if(S!=EFI_SUCCESS)return S;}
  }return EFI_SUCCESS;
}
STATIC EFI_STATUS Stroke(CANVAS *C,SPLASH_POINT A,SPLASH_POINT B,UINT32 Radius,EFI_GRAPHICS_OUTPUT_BLT_PIXEL P){
  INT32 Dx=B.X-A.X,Dy=B.Y-A.Y;UINT32 L=Sqrt((UINT32)(Dx*Dx+Dy*Dy));
  if(!L)return Disc(C,(UINT32)A.X,(UINT32)A.Y,Radius,P);
  INT32 Nx=(INT32)(((INT64)Dy*Radius)/L),Ny=(INT32)(-((INT64)Dx*Radius)/L);
  SPLASH_POINT V[4]={{A.X+Nx,A.Y+Ny},{B.X+Nx,B.Y+Ny},{B.X-Nx,B.Y-Ny},{A.X-Nx,A.Y-Ny}};
  EFI_STATUS S=Polygon(C,V,P);if(S!=EFI_SUCCESS)return S;
  S=Disc(C,(UINT32)A.X,(UINT32)A.Y,Radius,P);if(S!=EFI_SUCCESS)return S;
  return Disc(C,(UINT32)B.X,(UINT32)B.Y,Radius,P);
}
STATIC UINT32 BrandWidth(UINT32 Height){
  UINT32 Width=Height*2/3,Total=(Height/25)*8;
  for(UINTN I=0;I<9;++I)Total+=Width*(mBrand[I]=='i'?38:mBrand[I]=='t'?76:mBrand[I]=='r'?82:100)/100;
  return Total;
}
STATIC EFI_STATUS Brand(CANVAS *C,UINT32 X,UINT32 Y,UINT32 Height,EFI_GRAPHICS_OUTPUT_BLT_PIXEL P){
  UINT32 BaseWidth=Height*2/3,Gap=Height/25,Radius=Height/32;
  if(Radius<1)Radius=1;
  for(UINTN I=0;I<9;++I){SPLASH_POINT Path[40];UINT32 Count=0;CHAR8 Ch=mBrand[I];
    UINT32 Width=BaseWidth*(Ch=='i'?38:Ch=='t'?76:Ch=='r'?82:100)/100;
    if(Ch=='o'||Ch=='a'||Ch=='c'||Ch=='e'){
      UINT32 First=(Ch=='c'?4:Ch=='e'?2:0),Last=(Ch=='c'?28:Ch=='e'?30:32);
      for(UINT32 J=First;J<=Last;++J)Path[Count++]=(SPLASH_POINT){500+mCircle[J%32].X*390/1000,750+mCircle[J%32].Y*450/1000};
    }else if(Ch=='n'){
      Path[Count++]=(SPLASH_POINT){110,1200};Path[Count++]=(SPLASH_POINT){110,300};Path[Count++]=(SPLASH_POINT){110,700};
      for(UINT32 J=16;J<=32;++J)Path[Count++]=(SPLASH_POINT){500+mCircle[J%32].X*390/1000,700+mCircle[J%32].Y*400/1000};
      Path[Count++]=(SPLASH_POINT){890,1200};
    }else if(Ch=='t'){
      Path[Count++]=(SPLASH_POINT){430,0};Path[Count++]=(SPLASH_POINT){430,1050};Path[Count++]=(SPLASH_POINT){500,1150};
      Path[Count++]=(SPLASH_POINT){640,1200};Path[Count++]=(SPLASH_POINT){840,1200};
    }else if(Ch=='i'){
      Path[Count++]=(SPLASH_POINT){500,350};Path[Count++]=(SPLASH_POINT){500,1200};
    }else if(Ch=='r'){
      Path[Count++]=(SPLASH_POINT){110,1200};Path[Count++]=(SPLASH_POINT){110,300};Path[Count++]=(SPLASH_POINT){110,700};
      Path[Count++]=(SPLASH_POINT){270,430};Path[Count++]=(SPLASH_POINT){520,300};Path[Count++]=(SPLASH_POINT){760,320};
    }else return EFI_COMPROMISED_DATA;
    for(UINTN J=1;J<Count;++J){SPLASH_POINT A=Path[J-1],B=Path[J];
      A.X=(INT32)X+A.X*(INT32)Width/1000;B.X=(INT32)X+B.X*(INT32)Width/1000;
      A.Y=(INT32)Y+A.Y*(INT32)Height/1200;B.Y=(INT32)Y+B.Y*(INT32)Height/1200;
      EFI_STATUS S=Stroke(C,A,B,Radius,P);if(S!=EFI_SUCCESS)return S;
    }
    if(Ch=='t'||Ch=='a'||Ch=='e'){
      SPLASH_POINT A=Ch=='t'?(SPLASH_POINT){100,350}:Ch=='a'?(SPLASH_POINT){890,300}:(SPLASH_POINT){200,1050},
                   B=Ch=='t'?(SPLASH_POINT){850,350}:Ch=='a'?(SPLASH_POINT){890,1200}:(SPLASH_POINT){855,500};
      A.X=(INT32)X+A.X*(INT32)Width/1000;B.X=(INT32)X+B.X*(INT32)Width/1000;
      A.Y=(INT32)Y+A.Y*(INT32)Height/1200;B.Y=(INT32)Y+B.Y*(INT32)Height/1200;
      EFI_STATUS S=Stroke(C,A,B,Radius,P);if(S!=EFI_SUCCESS)return S;
    }
    if(Ch=='i'){EFI_STATUS S=Disc(C,X+Width/2,Y+Height/12,Radius*2,P);if(S!=EFI_SUCCESS)return S;}
    X+=Width+Gap;
  }return EFI_SUCCESS;
}
STATIC CONST UINT8 *Glyph(CHAR8 Ch) {
  STATIC CONST UINT8 Space[5]={0},Plus[5]={8,8,0x3e,8,8},Minus[5]={8,8,8,8,8};
  STATIC CONST UINT8 Slash[5]={0x20,0x10,8,4,2},Colon[5]={0,0x36,0x36,0,0};
  if(Ch>='a'&&Ch<='z')Ch=(CHAR8)(Ch-'a'+'A');
  if(Ch>='A'&&Ch<='Z')return mLetters[Ch-'A'];if(Ch>='0'&&Ch<='9')return mDigits[Ch-'0'];
  if(Ch=='+')return Plus;if(Ch=='-')return Minus;if(Ch=='/')return Slash;if(Ch==':')return Colon;return Space;
}
STATIC UINT32 Length(CONST CHAR8 *Text){UINT32 N=0;while(Text[N])++N;return N;}
STATIC EFI_STATUS Text(CANVAS *C,CONST CHAR8 *String,UINT32 X,UINT32 Y,UINT32 Scale,EFI_GRAPHICS_OUTPUT_BLT_PIXEL P) {
  UINT32 N=Length(String),Width=(N*6-1)*Scale;
  while(Width>C->Report->Width-X-16&&Scale>1){--Scale;Width=(N*6-1)*Scale;}
  for(UINT32 I=0;I<N;++I){CONST UINT8 *G=Glyph(String[I]);
    for(UINT32 Column=0;Column<5;++Column){UINT32 Row=0;
      while(Row<7){if(!(G[Column]&(1U<<Row))){++Row;continue;}UINT32 First=Row;
        while(Row<7&&(G[Column]&(1U<<Row)))++Row;
        EFI_STATUS S=Rect(C,P,X+(I*6+Column)*Scale,Y+First*Scale,Scale,(Row-First)*Scale);if(S!=EFI_SUCCESS)return S;
      }
    }
  }return EFI_SUCCESS;
}
EFI_STATUS PianoProductDrawSplash(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,PIANO_SPLASH_ALIVE Alive,PIANO_SPLASH_REPORT *R) {
  if(!Gop||!Gop->Blt||!Gop->Mode||!Gop->Mode->Info||!Alive||!R)return EFI_INVALID_PARAMETER;
  if(!Alive())return EFI_ABORTED;
  *R=(PIANO_SPLASH_REPORT){0};R->Width=Gop->Mode->Info->HorizontalResolution;R->Height=Gop->Mode->Info->VerticalResolution;
  if(R->Width<480||R->Height<320||R->Width>8192||R->Height>8192)return R->Status=EFI_UNSUPPORTED;
  UINT32 Short=R->Width<R->Height?R->Width:R->Height;
  R->LogoSize=Short*20/100;R->WordmarkScale=Short/50;R->HintScale=Short/400;R->KeysScale=Short/500;
  if(!R->WordmarkScale)R->WordmarkScale=1;if(!R->HintScale)R->HintScale=1;if(!R->KeysScale)R->KeysScale=1;
  UINT32 TitleHeight=R->WordmarkScale*7,GroupWidth=R->LogoSize+Short/20+BrandWidth(TitleHeight),CenterY=R->Height*33/100;
  while(GroupWidth>R->Width-32&&R->WordmarkScale>1){--R->WordmarkScale;TitleHeight=R->WordmarkScale*7;GroupWidth=R->LogoSize+Short/20+BrandWidth(TitleHeight);}
  R->LogoX=(R->Width-GroupWidth)/2;R->LogoY=CenterY-R->LogoSize/2;
  R->WordmarkX=R->LogoX+R->LogoSize+Short/20;R->WordmarkY=CenterY-TitleHeight/2;
  R->HintX=R->Width*6/100;R->HintY=R->Height*89/100;R->KeysY=R->Height*94/100;
  CANVAS C={Gop,Alive,R};EFI_STATUS S;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Bg=Color(255,255,255),Gold=Color(255,102,0),Shade=Color(214,82,0),White=Color(64,68,74),Muted=Color(112,116,122),Cyan=Color(67,73,82);
  S=Rect(&C,Bg,0,0,R->Width,R->Height);if(S!=EFI_SUCCESS)goto Done;
  for(UINTN I=0;I<6;++I){SPLASH_POINT D=mSpokes[I],N={-D.Y,D.X};
    SPLASH_POINT Center={(INT32)R->LogoX+(INT32)R->LogoSize/2,(INT32)R->LogoY+(INT32)R->LogoSize/2};
    SPLASH_POINT End={Center.X+D.X*(INT32)R->LogoSize*43/100000,Center.Y+D.Y*(INT32)R->LogoSize*43/100000};
    INT32 Inner=(INT32)R->LogoSize*20/1000,Outer=(INT32)R->LogoSize*48/1000;
    SPLASH_POINT P[4]={{Center.X+N.X*Inner/1000,Center.Y+N.Y*Inner/1000},
      {End.X+N.X*Outer/1000,End.Y+N.Y*Outer/1000},{End.X-N.X*Outer/1000,End.Y-N.Y*Outer/1000},
      {Center.X-N.X*Inner/1000,Center.Y-N.Y*Inner/1000}};
    S=Disc(&C,(UINT32)End.X,(UINT32)End.Y,(UINT32)Outer,Gold);if(S!=EFI_SUCCESS)goto Done;
    S=Polygon(&C,P,Gold);if(S!=EFI_SUCCESS)goto Done;
    SPLASH_POINT Face[4]={Center,End,P[2],P[3]};S=Polygon(&C,Face,Shade);if(S!=EFI_SUCCESS)goto Done;
  }
  S=Brand(&C,R->WordmarkX,R->WordmarkY,R->WordmarkScale*7,White);if(S!=EFI_SUCCESS)goto Done;
  UINT32 SubScale=Short/350;if(!SubScale)SubScale=1;
  S=Text(&C,mSubtitle,R->WordmarkX,R->WordmarkY+R->WordmarkScale*7+Short/65,SubScale,Muted);if(S!=EFI_SUCCESS)goto Done;
  S=Text(&C,mSetupHint,R->HintX,R->HintY,R->HintScale,Cyan);if(S!=EFI_SUCCESS)goto Done;
  S=Text(&C,mKeysHint,R->HintX,R->KeysY,R->KeysScale,Muted);
Done:R->Status=S;return S;
}
