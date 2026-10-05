// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../bootprofiles/product-support/Library/ProductBootManagerLib/ProductSplash.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
STATIC UINT32 Width,Height,Calls,FailAt,LoseAt;
STATIC BOOLEAN Live;
STATIC EFI_STATUS Inject;
STATIC BOOLEAN EFIAPI Alive(VOID){return Live;}
STATIC EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P,
 EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN X,UINTN Y,UINTN W,UINTN H,UINTN Delta){
 (VOID)G;assert(Live);assert(P);assert(Op==EfiBltVideoFill);assert(!Sx&&!Sy&&!Delta);
 assert(X<Width&&Y<Height&&W&&H&&W<=Width-X&&H<=Height-Y);
 ++Calls;if(Calls==LoseAt)Live=FALSE;if(Calls==FailAt)return Inject;
 for(UINTN Row=Y;Row<Y+H;++Row)for(UINTN Col=X;Col<X+W;++Col)Pixels[Row*Width+Col]=*P;
 return EFI_SUCCESS;
}
STATIC VOID Preview(CONST CHAR8 *Path){
 FILE *F=fopen(Path,"wb");assert(F);assert(fprintf(F,"P6\n%u %u\n255\n",Width,Height)>0);
 for(UINTN I=0;I<(UINTN)Width*Height;++I){UINT8 Rgb[3]={Pixels[I].Red,Pixels[I].Green,Pixels[I].Blue};assert(fwrite(Rgb,1,3,F)==3);}
 assert(!fclose(F));
}
STATIC VOID Reset(UINT32 W,UINT32 H){
 free(Pixels);Width=W;Height=H;Pixels=calloc((UINTN)W*H,sizeof(*Pixels));assert(Pixels);
 Calls=FailAt=LoseAt=0;Live=TRUE;Inject=EFI_DEVICE_ERROR;
}
int main(int Argc,char **Argv){
 EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info={0};EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode={0};
 EFI_GRAPHICS_OUTPUT_PROTOCOL Gop={0};PIANO_SPLASH_REPORT R;
 Mode.Info=&Info;Mode.SizeOfInfo=sizeof(Info);Gop.Mode=&Mode;Gop.Blt=Blt;
 Reset(3200,2136);Info.HorizontalResolution=Width;Info.VerticalResolution=Height;
 assert(PianoProductDrawSplash(&Gop,Alive,&R)==EFI_SUCCESS);
 assert(R.LogoSize>=420&&R.LogoSize<440&&R.HintScale>=5&&R.WordmarkScale>=40);
 assert(R.WordmarkX>R.LogoX+R.LogoSize&&R.HintX==Width*6/100&&R.HintY>Height*88/100);
 assert(R.HintY>R.WordmarkY&&R.KeysY>R.HintY&&R.DrawCalls==Calls&&Calls<15000);
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL Center=Pixels[(R.LogoY+R.LogoSize/2)*Width+R.LogoX+R.LogoSize/2];
 assert(Center.Red>=200&&Center.Green>=70&&Center.Green<=120&&Center.Blue==0);
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL Corner=Pixels[0];assert(Corner.Red==255&&Corner.Green==255&&Corner.Blue==255);
 UINTN HintPixels=0,KeysPixels=0;
 for(UINT32 Y=R.HintY;Y<R.HintY+7*R.HintScale;++Y)for(UINT32 X=0;X<Width;++X)
   if(Pixels[Y*Width+X].Green==73)++HintPixels;
 for(UINT32 Y=R.KeysY;Y<R.KeysY+7*R.KeysScale;++Y)for(UINT32 X=0;X<Width;++X)
   if(Pixels[Y*Width+X].Red==112)++KeysPixels;
 assert(HintPixels>3000&&KeysPixels>8000);
 if(Argc==2)Preview(Argv[1]);
 for(UINT32 Case=0;Case<4;++Case){
   Reset(800,600);Info.HorizontalResolution=Width;Info.VerticalResolution=Height;
   if(Case==0){assert(PianoProductDrawSplash(&Gop,Alive,&R)==EFI_SUCCESS);continue;}
   if(Case==1){FailAt=20;Inject=EFI_WARN_UNKNOWN_GLYPH;assert(PianoProductDrawSplash(&Gop,Alive,&R)==EFI_DEVICE_ERROR);assert(Calls==20);}
   if(Case==2){FailAt=20;Inject=EFI_NOT_READY;assert(PianoProductDrawSplash(&Gop,Alive,&R)==EFI_NOT_READY);assert(Calls==20);}
   if(Case==3){LoseAt=20;assert(PianoProductDrawSplash(&Gop,Alive,&R)==EFI_ABORTED);assert(Calls==20);}
 }
 Reset(480,320);Info.HorizontalResolution=Width;Info.VerticalResolution=Height;
 assert(PianoProductDrawSplash(&Gop,Alive,&R)==EFI_SUCCESS);
 Info.HorizontalResolution=400;Calls=0;assert(PianoProductDrawSplash(&Gop,Alive,&R)==EFI_UNSUPPORTED);assert(!Calls);
 Live=FALSE;Info.HorizontalResolution=480;assert(PianoProductDrawSplash(&Gop,Alive,&R)==EFI_ABORTED);assert(!Calls);
 assert(PianoProductDrawSplash(NULL,Alive,&R)==EFI_INVALID_PARAMETER);
 free(Pixels);puts("Actual GOP vector pixels, hints, scaled modes and failure/EBS fences passed");return 0;
}
