"""Actual native loader records only returned successful image handles."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
INC = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'

class NativeImageRegistryTests(unittest.TestCase):
    def test_actual_loader_registry_and_reentrant_lookup(self):
        with tempfile.TemporaryDirectory(prefix='native-images-') as td:
            d = Path(td)
            for name in ('NativeProbe.c', 'PianoNativeImages.h'):
                shutil.copyfile(ROOT / 'uefi/core' / name, d / name)
            (d / 'NativeProbeTable.h').write_text('''
typedef struct {CONST CHAR8 *Name;EFI_GUID Guid;CONST UINT8 *Depex;UINTN DepexBytes;} NATIVE_IMAGE;
static const UINT8 yes[]={6,8};
static const NATIVE_IMAGE mNativeImages[]={
 {"NpaDxe",{.Data1=1},yes,2},{"VcsDxe",{.Data1=2},yes,2},{"Failed",{.Data1=3},yes,2}};
''')
            (d / 'test.c').write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "NativeProbe.c"
EFI_BOOT_SERVICES bs;EFI_BOOT_SERVICES *gBS=&bs;EFI_HANDLE gImageHandle=(VOID *)9;
static unsigned loads,starts,unloads,frees,observations;
VOID *EFIAPI CopyMem(VOID *a,CONST VOID *b,UINTN n){return memcpy(a,b,n);}
BOOLEAN EFIAPI CompareGuid(CONST GUID *a,CONST GUID *b){return !memcmp(a,b,sizeof(*a));}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *a,CONST CHAR8 *b){return strcmp(a,b);}
VOID EFIAPI FreePool(VOID *p){free(p);frees++;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN l){(VOID)l;return TRUE;}
VOID EFIAPI DebugPrint(UINTN l,CONST CHAR8 *fmt,...){(VOID)l;(VOID)fmt;}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *g,EFI_SECTION_TYPE t,UINTN i,VOID **p,UINTN *n){
 assert(t==EFI_SECTION_PE32&&!i);*p=malloc(4);memcpy(*p,&g->Data1,4);*n=4;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI load(BOOLEAN policy,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *path,VOID *p,UINTN n,EFI_HANDLE *h){
 assert(!policy&&parent==gImageHandle&&!path&&n==4);UINT32 id;memcpy(&id,p,4);*h=(VOID *)(UINTN)(id+100);loads++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI start(EFI_HANDLE h,UINTN *n,CHAR16 **s){assert(!n&&!s);starts++;return h==(VOID *)103?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI unload(EFI_HANDLE h){assert(h==(VOID *)103);unloads++;return EFI_SUCCESS;}
static VOID observe(CONST CHAR8 *name,BOOLEAN before){
 (VOID)name;(VOID)before;EFI_GUID id={.Data1=1};EFI_HANDLE h=(VOID *)1;
 assert(PianoNativeGetLoadedImage(&id,&h)==EFI_NOT_READY&&!h);observations++;
}
int main(void){
 EFI_HANDLE h=(VOID *)1;EFI_GUID id={.Data1=1};
 assert(PianoNativeGetLoadedImage(NULL,&h)==EFI_INVALID_PARAMETER);
 assert(PianoNativeGetLoadedImage(&id,NULL)==EFI_INVALID_PARAMETER);
 assert(PianoNativeGetLoadedImage(&id,&h)==EFI_NOT_READY&&!h);
 bs.LoadImage=load;bs.StartImage=start;bs.UnloadImage=unload;PianoNativeSetObserver(observe);PianoProbeFoundation();
 assert(loads==3&&starts==3&&unloads==1&&frees==3&&observations==6);
 assert(PianoNativeGetLoadedImage(&id,&h)==EFI_SUCCESS&&h==(VOID *)101);
 id.Data1=2;assert(PianoNativeGetLoadedImage(&id,&h)==EFI_SUCCESS&&h==(VOID *)102);
 id.Data1=3;assert(PianoNativeGetLoadedImage(&id,&h)==EFI_NOT_FOUND&&!h);
 id.Data1=99;assert(PianoNativeGetLoadedImage(&id,&h)==EFI_NOT_FOUND&&!h);
 PianoProbeFoundation();assert(loads==3&&starts==3&&unloads==1);
 return 0;
}
''')
            exe = d / 'registry'
            result = subprocess.run(['cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I'+str(INC), '-I'+str(INC/'X64'), str(d/'test.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()
