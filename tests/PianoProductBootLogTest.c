// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../bootprofiles/uefi-app/PianoProductBootLog.h"
#include "../bootprofiles/product-support/Library/ProductBootManagerLib/ProductSplash.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
STATIC UINT32 Width,Height,Calls,FailAt,LoseAt,PanelX,PanelY,Pitch,PanelClears;
STATIC BOOLEAN Live;
STATIC EFI_STATUS Inject;
STATIC BOOLEAN EFIAPI Alive(VOID){return Live;}
STATIC EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixel,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN X,UINTN Y,UINTN W,UINTN H,UINTN Delta){
  (VOID)G;assert(Live&&Pixel);assert(Op==EfiBltVideoFill&&!Sx&&!Sy&&!Delta);
  assert(X<Width&&Y<Height&&W&&H&&W<=Width-X&&H<=Height-Y);
  ++Calls;if(Calls==LoseAt)Live=FALSE;if(Calls==FailAt)return Inject;
  if(Pixel->Red==255&&Pixel->Green==255&&Pixel->Blue==255&&H%10==0&&W==402*(H/10)){
    if(!PanelClears){PanelX=(UINT32)X;PanelY=(UINT32)Y;Pitch=(UINT32)H;}++PanelClears;
  }
  for(UINTN Row=Y;Row<Y+H;++Row)for(UINTN Column=X;Column<X+W;++Column)Pixels[Row*Width+Column]=*Pixel;
  return EFI_SUCCESS;
}
STATIC VOID Reset(UINT32 W,UINT32 H){
  free(Pixels);Width=W;Height=H;Pixels=malloc((UINTN)W*H*sizeof(*Pixels));assert(Pixels);
  memset(Pixels,0xa5,(UINTN)W*H*sizeof(*Pixels));
  Calls=FailAt=LoseAt=PanelX=PanelY=Pitch=PanelClears=0;Live=TRUE;Inject=EFI_DEVICE_ERROR;
}
// Decode the rendered 5x7 pixels independently to check the text contract,
// including the actual error bit and the full UINT64 elapsed value.
typedef struct {CHAR8 Ch;UINT8 Columns[5];} TEST_GLYPH;
STATIC CONST TEST_GLYPH Alphabet[]={
  {'0',{0x3e,0x51,0x49,0x45,0x3e}},{'1',{0x00,0x42,0x7f,0x40,0x00}},
  {'2',{0x42,0x61,0x51,0x49,0x46}},{'3',{0x21,0x41,0x45,0x4b,0x31}},
  {'4',{0x18,0x14,0x12,0x7f,0x10}},{'5',{0x27,0x45,0x45,0x45,0x39}},
  {'6',{0x3c,0x4a,0x49,0x49,0x30}},{'7',{0x01,0x71,0x09,0x05,0x03}},
  {'8',{0x36,0x49,0x49,0x49,0x36}},{'9',{0x06,0x49,0x49,0x29,0x1e}},
  {'A',{0x7e,0x11,0x11,0x11,0x7e}},{'B',{0x7f,0x49,0x49,0x49,0x36}},
  {'D',{0x7f,0x41,0x41,0x22,0x1c}},{'E',{0x7f,0x49,0x49,0x49,0x41}},
  {'F',{0x7f,0x09,0x09,0x09,0x01}},{'I',{0x00,0x41,0x7f,0x41,0x00}},
  {'K',{0x7f,0x08,0x14,0x22,0x41}},{'L',{0x7f,0x40,0x40,0x40,0x40}},
  {'M',{0x7f,0x02,0x0c,0x02,0x7f}},{'N',{0x7f,0x04,0x08,0x10,0x7f}},
  {'O',{0x3e,0x41,0x41,0x41,0x3e}},{'R',{0x7f,0x09,0x19,0x29,0x46}},
  {'S',{0x46,0x49,0x49,0x49,0x31}},{'T',{0x01,0x01,0x7f,0x01,0x01}},
  {'U',{0x3f,0x40,0x40,0x40,0x3f}},{'V',{0x1f,0x20,0x40,0x20,0x1f}},
  {'W',{0x3f,0x40,0x38,0x40,0x3f}},{'X',{0x63,0x14,0x08,0x14,0x63}},
  {'Y',{0x07,0x08,0x70,0x08,0x07}},{'-',{8,8,8,8,8}},{' ',{0,0,0,0,0}}
};
STATIC VOID ExpectText(UINT32 Row,UINT32 CharacterColumn,CONST CHAR8 *Expected){
  UINT32 Scale=Pitch/10,Y=PanelY+Row*Pitch,X=PanelX+6*Scale+CharacterColumn*6*Scale;
  for(UINT32 I=0;Expected[I];++I){UINT8 Columns[5]={0};
    for(UINT32 C=0;C<5;++C)for(UINT32 R=0;R<7;++R){
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL P=Pixels[(Y+R*Scale)*Width+X+(I*6+C)*Scale];
      if(P.Red!=255||P.Green!=255||P.Blue!=255)Columns[C]|=(UINT8)(1U<<R);
    }
    BOOLEAN Matched=FALSE;
    for(UINTN G=0;G<ARRAY_SIZE(Alphabet);++G)if(Alphabet[G].Ch==Expected[I]){
      Matched=memcmp(Columns,Alphabet[G].Columns,sizeof(Columns))==0;break;
    }
    assert(Matched);
  }
}
STATIC VOID ExpectCode(UINT32 Row,CONST CHAR8 *Elapsed,CONST CHAR8 *Expected){
  UINT32 Columns=(UINT32)strlen(Elapsed);if(Columns<8)Columns=8;
  ExpectText(Row,23+Columns+2,Expected);
}
STATIC VOID Preview(CONST CHAR8 *Path){
  FILE *F=fopen(Path,"wb");assert(F);assert(fprintf(F,"P6\n%u %u\n255\n",Width,Height)>0);
  for(UINTN I=0;I<(UINTN)Width*Height;++I){
    UINT8 Rgb[3]={Pixels[I].Red,Pixels[I].Green,Pixels[I].Blue};assert(fwrite(Rgb,1,3,F)==3);
  }assert(!fclose(F));
}
int main(int Argc,char **Argv){
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info={0};EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode={0};
  EFI_GRAPHICS_OUTPUT_PROTOCOL Gop={0};Mode.Info=&Info;Gop.Mode=&Mode;Gop.Blt=Blt;
  assert(PianoProductBootLogStage("DISPLAY",EFI_SUCCESS,0)==EFI_NOT_STARTED);
  Reset(3200,2136);Info.HorizontalResolution=Width;Info.VerticalResolution=Height;
  PIANO_SPLASH_REPORT Splash;assert(PianoProductDrawSplash(&Gop,Alive,&Splash)==EFI_SUCCESS);
  assert(PianoProductBootLogInitialize(&Gop,Alive)==EFI_SUCCESS&&PanelClears==7);
  assert(PanelX==Width*6/100&&PanelX==Splash.HintX&&Pitch==50&&PanelY==1281&&PanelY+7*Pitch<Splash.HintY);
  assert(PanelY>Splash.WordmarkY+7*Splash.WordmarkScale+Height/65+7*(Height/350));
  for(UINT32 Row=0;Row<7;++Row)ExpectText(Row,9,"--");
  assert(PianoProductBootLogStage("DISPLAY",EFI_SUCCESS,0)==EFI_SUCCESS);ExpectText(0,9,"OK");ExpectText(0,23,"0 MS");ExpectCode(0,"0 MS","--");
  assert(PianoProductBootLogStage("PAYLOAD",EFI_SUCCESS,MAX_UINT64)==EFI_SUCCESS);ExpectText(1,23,"18446744073709551615 MS");ExpectCode(1,"18446744073709551615 MS","--");
  assert(PianoProductBootLogStage("SMEM",EFI_NOT_READY,412)==EFI_SUCCESS);ExpectText(2,9,"NOT READY");ExpectText(2,23,"412 MS");ExpectCode(2,"412 MS","0X8000000000000006");
  assert(PianoProductBootLogStage("INPUT",EFI_NOT_STARTED,500)==EFI_SUCCESS);ExpectText(3,9,"WAIT");ExpectCode(3,"500 MS","--");
  assert(PianoProductBootLogStage("UFS",EFI_UNSUPPORTED,600)==EFI_SUCCESS);ExpectText(4,9,"UNAVAILABLE");ExpectCode(4,"600 MS","0X8000000000000003");
  assert(PianoProductBootLogStage("USB",EFI_DEVICE_ERROR,710)==EFI_SUCCESS);ExpectText(5,9,"FAILED");ExpectCode(5,"710 MS","0X8000000000000007");
  assert(PianoProductBootLogStage("MENU",EFI_WARN_UNKNOWN_GLYPH,725)==EFI_SUCCESS);ExpectText(6,9,"FAILED");ExpectCode(6,"725 MS","0X0000000000000001");
  UINT32 Previous=Calls;assert(PianoProductBootLogStage("not-a-stage",EFI_SUCCESS,1)==EFI_INVALID_PARAMETER&&Calls==Previous);
  assert(PianoProductBootLogStage(NULL,EFI_SUCCESS,1)==EFI_INVALID_PARAMETER&&Calls==Previous);
  if(Argc==2){
    assert(PianoProductBootLogStage("PAYLOAD",EFI_SUCCESS,18)==EFI_SUCCESS);
    assert(PianoProductBootLogStage("INPUT",EFI_SUCCESS,426)==EFI_SUCCESS);
    assert(PianoProductBootLogStage("UFS",EFI_SUCCESS,967)==EFI_SUCCESS);
    assert(PianoProductBootLogStage("USB",EFI_SUCCESS,1132)==EFI_SUCCESS);
    assert(PianoProductBootLogStage("MENU",EFI_NOT_STARTED,1140)==EFI_SUCCESS);Preview(Argv[1]);
  }
  CONST UINT32 Sizes[][2]={{480,320},{800,600},{3200,320},{480,2136}};
  for(UINTN I=0;I<ARRAY_SIZE(Sizes);++I){
    Reset(Sizes[I][0],Sizes[I][1]);Info.HorizontalResolution=Width;Info.VerticalResolution=Height;
    assert(PianoProductDrawSplash(&Gop,Alive,&Splash)==EFI_SUCCESS);
    assert(PianoProductBootLogInitialize(&Gop,Alive)==EFI_SUCCESS&&PanelClears==7);
    assert(PanelY+7*Pitch<Splash.HintY);
    assert(PanelX==Width*6/100&&PanelX==Splash.HintX);
    assert(PianoProductBootLogStage("UFS",EFI_NOT_FOUND,MAX_UINT64)==EFI_SUCCESS);ExpectText(4,9,"UNAVAILABLE");ExpectText(4,23,"18446744073709551615 MS");ExpectCode(4,"18446744073709551615 MS","0X800000000000000E");
  }
  for(UINT32 Case=0;Case<4;++Case){
    Reset(800,600);Info.HorizontalResolution=Width;Info.VerticalResolution=Height;
    assert(PianoProductBootLogInitialize(&Gop,Alive)==EFI_SUCCESS);
    if(Case==0){FailAt=Calls+10;Inject=EFI_DEVICE_ERROR;}
    if(Case==1){FailAt=Calls+10;Inject=EFI_WARN_UNKNOWN_GLYPH;}
    if(Case==2)LoseAt=Calls+10;
    if(Case==3)Live=FALSE;
    EFI_STATUS Expected=Case<2?EFI_DEVICE_ERROR:EFI_ABORTED;
    assert(PianoProductBootLogStage("USB",EFI_SUCCESS,1)==Expected);
    Previous=Calls;assert(PianoProductBootLogStage("USB",EFI_SUCCESS,1)==Expected&&Calls==Previous);
  }
  Reset(800,600);Info.HorizontalResolution=Width;Info.VerticalResolution=Height;FailAt=10;Inject=EFI_NOT_READY;
  assert(PianoProductBootLogInitialize(&Gop,Alive)==EFI_NOT_READY);Previous=Calls;
  assert(PianoProductBootLogStage("DISPLAY",EFI_SUCCESS,0)==EFI_NOT_READY&&Calls==Previous);
  assert(PianoProductBootLogInitialize(NULL,Alive)==EFI_INVALID_PARAMETER);
  assert(PianoProductBootLogInitialize(&Gop,NULL)==EFI_INVALID_PARAMETER);
  Info.HorizontalResolution=400;Previous=Calls;
  assert(PianoProductBootLogInitialize(&Gop,Alive)==EFI_UNSUPPORTED&&Calls==Previous);
  Live=FALSE;assert(PianoProductBootLogInitialize(&Gop,Alive)==EFI_ABORTED&&Calls==Previous);
  free(Pixels);puts("Actual boot-log GOP pixels/status hex/elapsed, seven fixed rows, bounds and terminal Blt/EBS fences passed");return 0;
}
