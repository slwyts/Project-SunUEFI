// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline file content inspection. No whole-file allocation or execution.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoFastbootBoot.c"
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static EFI_STATUS file_read(VOID *Context,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  if(Offset>INT64_MAX || fseeko(Context,(off_t)Offset,SEEK_SET))return EFI_DEVICE_ERROR;
  return fread(Buffer,1,Bytes,Context)==Bytes?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
static VOID range(CONST CHAR8 *Name,PIANO_BOOT_RANGE R){printf("%s offset=0x%llx bytes=0x%llx\n",Name,(unsigned long long)R.Offset,(unsigned long long)R.Bytes);}
int main(int argc,char **argv) {
  if(argc!=2){fprintf(stderr,"usage: %s FILE\n",argv[0]);return 2;}
  FILE *File=fopen(argv[1],"rb");if(!File){perror(argv[1]);return 2;}
  if(fseeko(File,0,SEEK_END)){fclose(File);return 2;}off_t Bytes=ftello(File);if(Bytes<0){fclose(File);return 2;}
  PIANO_BOOT_SOURCE S={File,file_read,(UINT64)Bytes};PIANO_BOOT_IMAGE I;EFI_STATUS Status=PianoFastbootBootParse(&S,&I);fclose(File);
  printf("status=0x%llx kind=%u source_bytes=%llu\n",(unsigned long long)Status,I.Kind,(unsigned long long)S.Bytes);
  if(Status!=EFI_SUCCESS)return 1;
  printf("version=%u header_bytes=%u declared_header=%u page=%u v4_cli_quirk=%u kernel_pe=%u kernel_pe_status=0x%llx\n",I.Version,I.HeaderBytes,I.DeclaredHeaderBytes,I.PageBytes,I.KnownV4CliHeaderQuirk,I.KernelIsArm64Pe,(unsigned long long)I.KernelPeStatus);
  range("kernel",I.Kernel);range("ramdisk",I.Ramdisk);range("second",I.Second);range("recovery_dtbo",I.RecoveryDtbo);range("dtb",I.Dtb);range("signature",I.Signature);range("trailing",I.Trailing);
  if(I.Kind==PianoBootArm64Pe || I.KernelIsArm64Pe)printf("machine=%04x subsystem=%u sections=%u image_bytes=0x%x entry_rva=0x%x entry_file_offset=0x%llx\n",I.Pe.Machine,I.Pe.Subsystem,I.Pe.Sections,I.Pe.ImageBytes,I.Pe.EntryRva,(unsigned long long)I.Pe.EntryFileOffset);
  return 0;
}
