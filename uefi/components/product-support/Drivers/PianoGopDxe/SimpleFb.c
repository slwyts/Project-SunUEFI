#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/CacheMaintenanceLib.h>

#include <Configuration/BootDevices.h>

//
// Global Variables
//
STATIC FRAME_BUFFER_CONFIGURE *FrameBufferConfiguration;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mFrameBuffer;
STATIC UINTN mFrameBase,mFrameBytes,mFrameStride,mFramePixelBytes;
STATIC UINT32 mFrameWidth,mFrameHeight,mBltErrors;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mRowScratch;
STATIC BOOLEAN
CalculateFrameBytes (UINTN Width,UINTN Height,UINTN PixelBytes,UINTN *Bytes)
{
  if(!Width || !Height || !PixelBytes || Width>MAX_UINTN/PixelBytes)return FALSE;
  UINTN Stride=Width*PixelBytes;if(Height>MAX_UINTN/Stride)return FALSE;
  *Bytes=Stride*Height;return TRUE;
}

STATIC BOOLEAN
FrameRectValid (UINTN X,UINTN Y,UINTN Width,UINTN Height)
{
  return Width && Height && X<mFrameWidth && Y<mFrameHeight &&
    Width<=mFrameWidth-X && Height<=mFrameHeight-Y;
}

STATIC VOID
PublishFrameRect (UINTN X,UINTN Y,UINTN Width,UINTN Height)
{
  UINTN Offset=Y*mFrameStride+X*mFramePixelBytes,Bytes=Width*mFramePixelBytes;
  // Only the initialized framebuffer allocation is involved. No invalidate,
  // whole-cache maintenance, MMU change or device-DMA ownership is performed.
  if(X==0 && Bytes==mFrameStride){
    WriteBackDataCacheRange((VOID *)(mFrameBase+Offset),Height*mFrameStride);
  }else for(UINTN Row=0;Row<Height;++Row)
    WriteBackDataCacheRange((VOID *)(mFrameBase+Offset+Row*mFrameStride),Bytes);
  // The real ArmCacheMaintenanceLib supplies its required final DSB.
}

STATIC EFI_STATUS
BltFailure (EFI_STATUS Status,EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation,
            UINTN SourceX,UINTN SourceY,UINTN X,UINTN Y,UINTN Width,UINTN Height)
{
  if(mBltErrors<8){++mBltErrors;
    DEBUG((DEBUG_WARN,"SUNUEFI_GOP_BLT_ERROR index=%u op=%u src=%lu,%lu dst=%lu,%lu rect=%lux%lu status=%r\n",
      mBltErrors,(UINT32)Operation,(UINT64)SourceX,(UINT64)SourceY,(UINT64)X,(UINT64)Y,(UINT64)Width,(UINT64)Height,Status));}
  return Status;
}

EFI_STATUS
FrameBufferQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                 ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;

  if (This == NULL || This->Mode == NULL || This->Mode->Info == NULL ||
      SizeOfInfo == NULL || Info == NULL) { return EFI_INVALID_PARAMETER; }
  *Info = NULL;
  if (ModeNumber >= This->Mode->MaxMode) { return EFI_INVALID_PARAMETER; }
  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION), (VOID *)Info);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Pass Info Size
  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  // Pass Frame Buffer Infos
  (*Info)->Version              = This->Mode->Info->Version;
  (*Info)->HorizontalResolution = This->Mode->Info->HorizontalResolution;
  (*Info)->VerticalResolution   = This->Mode->Info->VerticalResolution;
  (*Info)->PixelFormat          = This->Mode->Info->PixelFormat;
  (*Info)->PixelsPerScanLine    = This->Mode->Info->PixelsPerScanLine;

  return EFI_SUCCESS;
}

EFI_STATUS
FrameBufferSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN UINT32                        ModeNumber)
{
  if (This == NULL || This->Mode == NULL || ModeNumber >= This->Mode->MaxMode) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
FrameBufferGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL      *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation OPTIONAL,
  IN UINTN                              SourceX,
  IN UINTN                              SourceY,
  IN UINTN                              DestinationX,
  IN UINTN                              DestinationY,
  IN UINTN                              Width,
  IN UINTN                              Height,
  IN UINTN                              Delta OPTIONAL)
{
  EFI_STATUS Status;
  EFI_TPL    Tpl;

  if(This!=&mFrameBuffer || !This->Mode || !This->Mode->Info ||
     !FrameBufferConfiguration || !mFrameBytes || !mRowScratch)
    return BltFailure(EFI_NOT_READY,BltOperation,SourceX,SourceY,DestinationX,DestinationY,Width,Height);
  if(This->Mode->FrameBufferBase!=mFrameBase || This->Mode->FrameBufferSize!=mFrameBytes ||
     This->Mode->Info->HorizontalResolution!=mFrameWidth || This->Mode->Info->VerticalResolution!=mFrameHeight ||
     This->Mode->Info->PixelsPerScanLine*mFramePixelBytes!=mFrameStride)
    return BltFailure(EFI_COMPROMISED_DATA,BltOperation,SourceX,SourceY,DestinationX,DestinationY,Width,Height);
  if(BltOperation>=EfiGraphicsOutputBltOperationMax ||
     (BltOperation!=EfiBltVideoToVideo && !BltBuffer) ||
     (BltOperation==EfiBltVideoToBltBuffer ? !FrameRectValid(SourceX,SourceY,Width,Height) :
      !FrameRectValid(DestinationX,DestinationY,Width,Height)) ||
     (BltOperation==EfiBltVideoToVideo && !FrameRectValid(SourceX,SourceY,Width,Height)))
    return BltFailure(EFI_INVALID_PARAMETER,BltOperation,SourceX,SourceY,DestinationX,DestinationY,Width,Height);

  // Raise TPL Level
  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  // Draw to Frame Buffer
  if(BltOperation==EfiBltVideoToVideo){
    // The selected library's downward copy starts at Y+Height, one row past
    // the rectangle. Use its real read/write primitives through one bounded
    // scratch row, bottom-up for downward overlap and top-down otherwise.
    Status=EFI_SUCCESS;
    for(UINTN I=0;I<Height;++I){UINTN Row=DestinationY>SourceY?Height-I-1:I;
      Status=FrameBufferBlt(FrameBufferConfiguration,mRowScratch,EfiBltVideoToBltBuffer,
        SourceX,SourceY+Row,0,0,Width,1,0);
      if(Status!=EFI_SUCCESS)break;
      Status=FrameBufferBlt(FrameBufferConfiguration,mRowScratch,EfiBltBufferToVideo,
        0,0,DestinationX,DestinationY+Row,Width,1,0);
      if(Status!=EFI_SUCCESS)break;
      PublishFrameRect(DestinationX,DestinationY+Row,Width,1);
    }
  }else{
    Status=FrameBufferBlt(FrameBufferConfiguration,BltBuffer,BltOperation,SourceX,SourceY,DestinationX,DestinationY,Width,Height,Delta);
    if(Status==EFI_SUCCESS && BltOperation!=EfiBltVideoToBltBuffer)
      PublishFrameRect(DestinationX,DestinationY,Width,Height);
  }

  // Restore TPL Level
  gBS->RestoreTPL (Tpl);

  return Status==EFI_SUCCESS?Status:BltFailure(Status,BltOperation,SourceX,SourceY,DestinationX,DestinationY,Width,Height);
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mFrameBuffer = {
  FrameBufferQueryMode,
  FrameBufferSetMode,
  FrameBufferGopBlt,
  NULL
};

EFI_STATUS
GetFrameBufferInfos (
  OUT EFI_PHYSICAL_ADDRESS *BaseAddress,
  OUT UINT32               *Width,
  OUT UINT32               *Height,
  OUT UINT32               *ColorDepth)
{
  EFI_MEMORY_REGION_DESCRIPTOR FrameBufferRegion = {0};
  EFI_STATUS Status;
  UINTN Bytes;

  // Locate "Display Reserved" Memory Region
  Status = LocateMemoryRegionByName ("Display_Reserved", &FrameBufferRegion);
  if (EFI_ERROR (Status)) { return Status; }

  // Verify Frame Buffer Region
  if (!FrameBufferRegion.Address) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate \"Display Reserved\" Memory Region!\n"));
    return EFI_NOT_FOUND;
  }

  // Get Frame Buffer Details
  *Width      = FixedPcdGet32 (PcdFrameBufferWidth);
  *Height     = FixedPcdGet32 (PcdFrameBufferHeight);
  *ColorDepth = FixedPcdGet32 (PcdFrameBufferColorDepth);

  // Verify Frame Buffer Resolution
  if (!*Width || !*Height || *Width < 640 || *Height < 475) {
    DEBUG ((EFI_D_ERROR, "Frame Buffer Resolution is too Small!\n"));
    DEBUG ((EFI_D_ERROR, "Minimal Supported Resolution = 640x475\n"));

    return EFI_UNSUPPORTED;
  }

  if (*ColorDepth != 32 || !CalculateFrameBytes(*Width,*Height,4,&Bytes) ||
      Bytes > FrameBufferRegion.Length ||
      FrameBufferRegion.Address>MAX_UINTN ||
      Bytes>MAX_UINTN-(UINTN)FrameBufferRegion.Address) {
    return EFI_UNSUPPORTED;
  }
  // Pass Frame Buffer Address
  *BaseAddress = FrameBufferRegion.Address;

  return EFI_SUCCESS;
}

EFI_STATUS
AllocateGopMemory ()
{
  EFI_STATUS Status;

  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE), (VOID *)&mFrameBuffer.Mode);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for GOP Mode! Status = %r\n", Status));
    return Status;
  }

  // Clear Memory
  ZeroMem (mFrameBuffer.Mode, sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));

  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION), (VOID *)&mFrameBuffer.Mode->Info);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for GOP Info! Status = %r\n", Status));
    return Status;
  }

  // Clear Memory
  ZeroMem (mFrameBuffer.Mode->Info, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));

  return EFI_SUCCESS;
}

EFI_STATUS
SetFrameBufferModeDetails (
  IN EFI_PHYSICAL_ADDRESS BaseAddress,
  IN UINT32               Width,
  IN UINT32               Height,
  IN UINT32               ColorDepth)
{
  UINTN Bytes;
  if(!mFrameBuffer.Mode || !mFrameBuffer.Mode->Info || !Width || !Height ||
     (ColorDepth!=32 && ColorDepth!=24 && ColorDepth!=16) ||
     !CalculateFrameBytes(Width,Height,ColorDepth/8,&Bytes) || BaseAddress>MAX_UINTN ||
     Bytes>MAX_UINTN-(UINTN)BaseAddress)return EFI_INVALID_PARAMETER;
  // Set Info Size
  mFrameBuffer.Mode->SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  // Set Mode Infos
  mFrameBuffer.Mode->MaxMode = 1;
  mFrameBuffer.Mode->Mode    = 0;

  // Set Frame Buffer Range
  mFrameBuffer.Mode->FrameBufferBase = BaseAddress;
  mFrameBuffer.Mode->FrameBufferSize = Bytes;

  // Set Mode Info Version
  mFrameBuffer.Mode->Info->Version = 0;

  // Set Frame Buffer Infos
  mFrameBuffer.Mode->Info->HorizontalResolution = Width;
  mFrameBuffer.Mode->Info->VerticalResolution   = Height;
  mFrameBuffer.Mode->Info->PixelsPerScanLine    = Width;

  // Set Pixel Format
  switch (ColorDepth) {
    case 32:
      mFrameBuffer.Mode->Info->PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
      break;

    case 24:
      mFrameBuffer.Mode->Info->PixelFormat                   = PixelBitMask;
      mFrameBuffer.Mode->Info->PixelInformation.RedMask      = 0x00FF0000;
      mFrameBuffer.Mode->Info->PixelInformation.GreenMask    = 0x0000FF00;
      mFrameBuffer.Mode->Info->PixelInformation.BlueMask     = 0x000000FF;
      mFrameBuffer.Mode->Info->PixelInformation.ReservedMask = 0x00000000;
      break;

    case 16:
      mFrameBuffer.Mode->Info->PixelFormat                   = PixelBitMask;
      mFrameBuffer.Mode->Info->PixelInformation.RedMask      = 0x0000F800;
      mFrameBuffer.Mode->Info->PixelInformation.GreenMask    = 0x000007E0;
      mFrameBuffer.Mode->Info->PixelInformation.BlueMask     = 0x0000001F;
      mFrameBuffer.Mode->Info->PixelInformation.ReservedMask = 0x00000000;
      break;

    default:
      DEBUG ((EFI_D_ERROR, "Color Depth %u is not Supported!\n", ColorDepth));
      return EFI_UNSUPPORTED;
  }

  mFrameBase=(UINTN)BaseAddress;mFrameBytes=mFrameBuffer.Mode->FrameBufferSize;
  mFrameWidth=Width;mFrameHeight=Height;mFramePixelBytes=ColorDepth/8;mFrameStride=(UINTN)Width*mFramePixelBytes;
  return EFI_SUCCESS;
}

EFI_STATUS
CreateFrameBufferConfig (IN EFI_PHYSICAL_ADDRESS BaseAddress)
{
  EFI_STATUS Status;

  // Set Configuration Size
  STATIC UINTN FrameBufferConfigurationSize = 0;

  // Get Frame Buffer Configuration Size
  FrameBufferBltConfigure ((VOID *)(UINTN)BaseAddress, mFrameBuffer.Mode->Info, FrameBufferConfiguration, &FrameBufferConfigurationSize);

  // Allocate Memory
  FrameBufferConfiguration = AllocatePool (FrameBufferConfigurationSize);
  if (FrameBufferConfiguration == NULL) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for the Frame Buffer Configuration!\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  // Create FrameBufferBltLib Configuration
  Status = FrameBufferBltConfigure ((VOID *)(UINTN)BaseAddress, mFrameBuffer.Mode->Info, FrameBufferConfiguration, &FrameBufferConfigurationSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Create the Frame Buffer Configuration! Status = %r\n", Status));
    return Status;
  }

  mRowScratch=AllocatePool((UINTN)mFrameWidth*sizeof(*mRowScratch));
  if(mRowScratch==NULL)return EFI_OUT_OF_RESOURCES;

  return EFI_SUCCESS;
}

VOID
FreeMemoryBuffers ()
{
  // Free Buffer
  if (mFrameBuffer.Mode->Info != NULL) {
    gBS->FreePool (mFrameBuffer.Mode->Info);
  }

  // Free Buffer
  if (mFrameBuffer.Mode != NULL) {
    gBS->FreePool (mFrameBuffer.Mode);
  }

  // Free Buffer
  if (FrameBufferConfiguration != NULL) {
    FreePool (FrameBufferConfiguration);
  }
  if(mRowScratch!=NULL){FreePool(mRowScratch);mRowScratch=NULL;}
}

EFI_STATUS
EFIAPI
RegisterFrameBuffer (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS BaseAddress;
  UINT32               Width;
  UINT32               Height;
  UINT32               ColorDepth;
  (VOID)SystemTable;

  // Get Frame Buffer Infos
  Status = GetFrameBufferInfos (&BaseAddress, &Width, &Height, &ColorDepth);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Allocate Memory
  Status = AllocateGopMemory ();
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Set Frame Buffer Mode Details
  Status = SetFrameBufferModeDetails (BaseAddress, Width, Height, ColorDepth);
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Create Frame Buffer Config
  Status = CreateFrameBufferConfig (BaseAddress);
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Register GOP Protocol
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gEfiDevicePathProtocolGuid, &DisplayDevicePath, &gEfiGraphicsOutputProtocolGuid, &mFrameBuffer, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register GOP Protocol!\n"));
    ASSERT_EFI_ERROR (Status);
  }

  DEBUG ((DEBUG_WARN, "SUNUEFI_GOP_READY width=%u height=%u stride=%u base=0x%lx bytes=%lu\n",
    Width, Height, Width, BaseAddress, (UINT64)mFrameBuffer.Mode->FrameBufferSize));
  return EFI_SUCCESS;

cleanup:
  // Free Memory Buffers
  FreeMemoryBuffers ();

  return Status;
}
