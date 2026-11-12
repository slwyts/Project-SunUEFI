// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
// Handles come from this product's actual successful native LoadImage/StartImage
// calls. Consumers still verify LoadedImage and pinned code before using them.
EFI_STATUS PianoNativeGetLoadedImage(CONST EFI_GUID *FileGuid,EFI_HANDLE *Handle);
