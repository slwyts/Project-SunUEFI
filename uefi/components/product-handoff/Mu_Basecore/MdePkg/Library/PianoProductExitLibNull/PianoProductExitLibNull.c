// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/PianoProductExitLib.h>
EFI_STATUS EFIAPI PianoProductBeforeExitBootServices(EFI_HANDLE Image,
 CONST EFI_LOADED_IMAGE_PROTOCOL *Info,EFI_TPL Tpl,UINTN Key,UINTN Current,BOOLEAN Before){
 (VOID)Image;(VOID)Info;(VOID)Tpl;(VOID)Key;(VOID)Current;(VOID)Before;
 return EFI_SUCCESS;
}
