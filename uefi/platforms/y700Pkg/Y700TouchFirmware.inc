// SPDX-License-Identifier: GPL-2.0-or-later
// NT36536 RAM-only bootstrap. Protocol flow follows Novatek's NT36XXX_SPI
// driver; addresses and the effective cascade header layout were checked
// against this tablet's original nvt_36xxx.ko. See work/reference/y700-touch.
// This include uses the parent's Read/Write/WriteByte SPI helpers, which take
// full byte addresses and expose payload bytes without command/dummy bytes.

typedef struct {
  UINT32 Bin, Sram, Size, Crc;
} Y7_TOUCH_FW_PART;

STATIC CONST Y7_TOUCH_FW_PART mY7TouchParts[] = {
  {0x000250,0x000000,0x01f19b,0x59d354d2},
  {0x022000,0x100000,0x0158bf,0x942dc244},
  {0x000000,0x1158c0,0x0000ff,0xc686ea7b},
  {0x000100,0x1159c0,0x0000ff,0xa5f9bfc6}
};

STATIC UINT32 Y7TouchLe32(CONST UINT8 *P) {
  return (UINT32)P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);
}
STATIC VOID Y7TouchWriteLe(UINT32 Address,UINT32 Value,UINTN Bytes) {
  UINT8 Data[4];
  for(UINTN I=0;I<Bytes;++I)Data[I]=(UINT8)(Value>>(I*8));
  Write(Address,Data,Bytes);
}
STATIC BOOLEAN Y7TouchWait(UINT32 Address,UINT8 Mask,UINT8 Value,UINTN Attempts,UINTN DelayUs) {
  UINT8 Data;
  for(UINTN I=0;I<Attempts;++I) {
    Read(Address,&Data,1);
    if((Data&Mask)==Value)return TRUE;
    MicroSecondDelay(DelayUs);
  }
  return FALSE;
}
STATIC BOOLEAN Y7TouchInfo(UINT8 *Version,UINT16 *Pid) {
  UINT8 Info[38];
  Read(0x130978,Info,sizeof(Info));
  *Version=Info[0];*Pid=(UINT16)(Info[34]|((UINT16)Info[35]<<8));
  return (UINT32)Info[0]+Info[1]==0xff;
}
STATIC BOOLEAN Y7TouchReady(VOID) {
  UINT8 State;
  for(UINTN I=0;I<50;++I) {
    Read(0x130960,&State,1);
    if(State>=0xa1&&State<=0xaf)return TRUE;
    MicroSecondDelay(10000);
  }
  return FALSE;
}
STATIC BOOLEAN Y7TouchValidateImage(VOID) {
  CONST UINT8 *F=mY700TouchFirmware;
  // This is intentionally restricted to the captured Y700/CSOT image. The
  // parent build also verifies its SHA256, 31382287ce3c5842f6df299589233459
  // 056043878deb1d27f7ef6b7f104ce271, before emitting the byte array.
  if(sizeof(mY700TouchFirmware)!=0x3e000||Y7TouchLe32(F)!=0x250||
     F[0x20]!=3||F[0x29]!=0||F[0x28]!=0||
     (UINT32)F[0x3d000]+F[0x3d001]!=0xff||
     !((F[0x3dffd]=='N'&&F[0x3dffe]=='V'&&F[0x3dfff]=='T')||
       (F[0x3dffd]=='M'&&F[0x3dffe]=='O'&&F[0x3dfff]=='D')))return FALSE;
  for(UINTN I=0;I<2;++I) {
    CONST Y7_TOUCH_FW_PART *P=&mY7TouchParts[I];
    if(Y7TouchLe32(F+I*12)!=P->Bin||Y7TouchLe32(F+I*12+4)!=P->Sram||
       Y7TouchLe32(F+I*12+8)!=P->Size||Y7TouchLe32(F+0x18+I*4)!=P->Crc)return FALSE;
  }
  // The real module stops the first header at the first nonempty section
  // whose BIN address is zero, then doubles that end for cascade. Here that
  // is 0x100 -> 0x200, not the ILM offset 0x250 (which includes metadata).
  for(UINTN I=2;I<4;++I) {
    CONST Y7_TOUCH_FW_PART *P=&mY7TouchParts[I];
    CONST UINT8 *H=F+(I==2?0xf0:0x1f0);
    if(Y7TouchLe32(H)!=P->Sram||Y7TouchLe32(H+4)!=P->Size||
       Y7TouchLe32(H+8)!=P->Bin||Y7TouchLe32(H+12)!=P->Crc)return FALSE;
  }
  return TRUE;
}
STATIC BOOLEAN Y7TouchCrc(VOID) {
  UINT8 Data[4],Flag[2];
  Read(0x1fb532,Flag,sizeof(Flag));
  Read(0x1fcc7c,Data,4);UINT32 Ilm=Y7TouchLe32(Data);
  Read(0x1fcc80,Data,4);UINT32 Dlm=Y7TouchLe32(Data);
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_CRC flag=%02x,%02x ilm=%08x dlm=%08x\n",Flag[0],Flag[1],Ilm,Dlm));
  // Original nvt_read_bld_hw_crc uses its use_gcm branch: crc_done is
  // byte1 bit3, ILM/DLM errors are byte0 bits4/5. The older 3-bit flag
  // decoder belongs to a different controller generation.
  return (Flag[1]&8)!=0&&(Flag[0]&0x30)==0&&
    Ilm==mY7TouchParts[0].Crc&&Dlm==mY7TouchParts[1].Crc;
}
STATIC EFI_STATUS InitializeController(VOID) {
  STATIC CONST UINT8 ExpectedTrim[6]={0x15,0x0e,0x00,0x23,0x65,0x03};
  UINT8 Trim[6],Version,EnbCascade;
  UINT16 Pid;
  mStep=20;
  mHeadersVerified=mFirmwareCrcVerified=FALSE;
  if(!Y7TouchValidateImage())return EFI_COMPROMISED_DATA;
  Read(0x1fb104,Trim,sizeof(Trim));
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_TRIM %02x %02x %02x %02x %02x %02x\n",
    Trim[0],Trim[1],Trim[2],Trim[3],Trim[4],Trim[5]));
  if(CompareMem(Trim,ExpectedTrim,sizeof(Trim))!=0)return EFI_UNSUPPORTED;
  Read(0x1fb12c,&EnbCascade,1);
  if(EnbCascade&1)return EFI_UNSUPPORTED;
  mStep=21;
  BOOLEAN HasInfo=Y7TouchInfo(&Version,&Pid);
  DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_FW_EXIST valid=%u version=%02x pid=%04x\n",HasInfo,Version,Pid));
  if(HasInfo&&Pid!=0x6097)return EFI_UNSUPPORTED;
  mMaxX=19040;mMaxY=30400;
  // A valid old reset-state byte does not prove the inherited firmware is
  // awake: Android suspend can leave it in deep sleep. Like the original
  // resume path, reload after checking identity rather than inventing a wake.
  mStep=22;
  // No-flash controllers can have no valid running firmware after a power
  // cycle. Exact trim/cascade identity plus the board-checked, fixed image
  // authorizes this SRAM download; a valid foreign PID is always rejected.
  for(UINTN Attempt=0;Attempt<2;++Attempt) {
    mStep=23;
    WriteByte(0x1fb43e,0x69);MicroSecondDelay(5000);
    // Three-byte lengths are length-minus-one, as stored in the image.
    Y7TouchWriteLe(0x1fcc30,mY7TouchParts[0].Sram,3);
    Y7TouchWriteLe(0x1fcc34,mY7TouchParts[0].Size,3);
    Y7TouchWriteLe(0x1fcc38,mY7TouchParts[0].Crc,4);
    Y7TouchWriteLe(0x1fcc40,mY7TouchParts[1].Sram,3);
    Y7TouchWriteLe(0x1fcc44,mY7TouchParts[1].Size,3);
    Y7TouchWriteLe(0x1fcc48,mY7TouchParts[1].Crc,4);
    mStep=24;
    // auto_copy=2 on the original NT36536 hw_info: the touch controller's
    // internal cascade copier, independent of host GENI/GPI/DMA.
    WriteByte(0x1fc825,0x56);
    for(UINTN I=0;I<sizeof(mY7TouchParts)/sizeof(mY7TouchParts[0]);++I) {
      CONST Y7_TOUCH_FW_PART *P=&mY7TouchParts[I];
      UINT32 Done=0,Bytes=P->Size+1;
      while(Done<Bytes) {
        // Match the original module's 0x3c00 payload bytes per CS, including
        // its cascade streaming boundaries. Do not subdivide these bursts.
        UINTN Chunk=Bytes-Done;if(Chunk>0x3c00)Chunk=0x3c00;
        Write(P->Sram+Done,mY700TouchFirmware+P->Bin+Done,Chunk);
        Done+=(UINT32)Chunk;
      }
    }
    mStep=25;
    if(!Y7TouchWait(0x1fc825,0xff,0,200,1000))continue;
    // Hardware CRC does not cover these two cascade information headers.
    // Verify them before starting the controller CPU, while SRAM is static.
    UINT8 Header[256];BOOLEAN HeadersValid=TRUE;
    for(UINTN I=2;I<4;++I) {
      Read(mY7TouchParts[I].Sram,Header,sizeof(Header));
      if(CompareMem(Header,mY700TouchFirmware+mY7TouchParts[I].Bin,sizeof(Header)))HeadersValid=FALSE;
    }
    DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_HEADERS verified=%u\n",HeadersValid));
    mHeadersVerified=HeadersValid;
    if(!HeadersValid)continue;
    // Exact original flow: clear six reset bytes; host command AE 00;
    // boot-ready=1; 5ms; wait for firmware reset state. No flash commands.
    UINT8 Clear[6]={0},Enable[2]={0xae,0};
    Write(0x130960,Clear,sizeof(Clear));Write(0x130950,Enable,sizeof(Enable));
    WriteByte(0x1fb50d,1);MicroSecondDelay(5000);
    mStep=26;
    if(!Y7TouchReady())continue;
    mStep=27;
    mFirmwareCrcVerified=Y7TouchCrc();
    if(!mFirmwareCrcVerified)continue;
    mStep=28;
    if(!Y7TouchInfo(&Version,&Pid)||Pid!=0x6097)continue;
    mVersion=Version;
    DEBUG((DEBUG_WARN,"SUNUEFI_Y700_TOUCH_BOOTSTRAP success=1 version=%02x pid=%04x attempt=%u\n",Version,Pid,(UINT32)Attempt));
    return EFI_SUCCESS;
  }
  return EFI_DEVICE_ERROR;
}
