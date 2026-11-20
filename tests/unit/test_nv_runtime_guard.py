import platform
from pathlib import Path
import sys,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
import prepare_nv_runtime_guard as guard
MARKER='  // The product has no persistent variable writer after ExitBootServices.'
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
RTC=ROOT/'uefi/components/product-support/Library/UnsupportedRealTimeClockLib/UnsupportedRealTimeClockLib.c'
class RuntimeNvGuardTests(unittest.TestCase):
 def pinned_sources(self,root):
  # Pinned VariableRuntimeDxe sources, as the build's workspace checkout starts
  path=root/guard.FILE;path.parent.mkdir(parents=True);relative=guard.FILE[len(guard.BASE)+1:]
  path.write_bytes(subprocess.check_output(['git','-C',str(ROOT/guard.BASE),'show','HEAD:'+relative]))
  dxe=path.with_name('VariableDxe.c');dxe.write_bytes(subprocess.check_output(['git','-C',str(ROOT/guard.BASE),'show','HEAD:'+str(Path(relative).with_name('VariableDxe.c'))]))
  header=root/'uefi/components/nv/Include/Protocol/PianoNvFile.h';header.parent.mkdir(parents=True);header.write_bytes((ROOT/'uefi/components/nv/Include/Protocol/PianoNvFile.h').read_bytes())
  return path
 def compile_and_run(self, text, *options):
  with tempfile.TemporaryDirectory(prefix='nv-guard-') as directory:
   source=Path(directory)/'guard.c'
   exe=source.with_suffix('')
   source.write_text(text)
   subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror',
                   '-Wno-unused-parameter','-I'+str(INC),'-I'+str(INC/HOST_PROCESSOR_BIND),
                   '-I'+str(INC.parents[1]/'MdeModulePkg/Include'),
                   *options,str(source),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
 def test_exact_standard_entry_before_store_or_arguments(self):
  with tempfile.TemporaryDirectory()as d:
   path=self.pinned_sources(Path(d));guard.prepare(Path(d),apply=True,check_pin=False);text=path.read_text()
  function=text.index('\nVariableServiceSetVariable (')
  begin=text.index(MARKER,function)
  end=text.index('  AuthFormat = ',function)
  self.assertLess(begin,end)
  self.compile_and_run('''#include <assert.h>
#undef NULL
#include <Uefi.h>
static BOOLEAN runtime;
static UINTN writes;
static BOOLEAN AtRuntime(void){return runtime;}
static EFI_STATUS Check(UINT32 attributes,UINTN bytes){
'''+text[begin:end]+'''
  ++writes;return EFI_SUCCESS;
}
int main(void){
  runtime=TRUE;
  assert(Check(EFI_VARIABLE_NON_VOLATILE,4)==EFI_UNSUPPORTED);
  assert(Check(EFI_VARIABLE_RUNTIME_ACCESS,4)==EFI_UNSUPPORTED);
  assert(Check(0,0)==EFI_UNSUPPORTED&&!writes);
  runtime=FALSE;
  assert(Check(EFI_VARIABLE_NON_VOLATILE,4)==EFI_SUCCESS);
  assert(Check(0,0)==EFI_SUCCESS&&writes==2);
  return 0;
}
''')
 def test_file_snapshot_failures_freeze_only_when_safe(self):
  nv=ROOT/'uefi/components/nv'
  self.compile_and_run('''#include <assert.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#define MDEPKG_NDEBUG
#include "'''+str(nv/'PianoNvFile.c')+'''"
EFI_GUID gEfiFileInfoGuid;
static EFI_STATUS OpenStatus;
static UINTN Opens;
VOID *EFIAPI AllocateZeroPool(UINTN Bytes){return calloc(1,Bytes);}
VOID *EFIAPI AllocatePool(UINTN Bytes){return malloc(Bytes);}
VOID EFIAPI FreePool(VOID *Buffer){free(Buffer);}
VOID *EFIAPI CopyMem(VOID *Dst,CONST VOID *Src,UINTN Bytes){return memcpy(Dst,Src,Bytes);}
VOID *EFIAPI ZeroMem(VOID *Dst,UINTN Bytes){return memset(Dst,0,Bytes);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Bytes){return memcmp(A,B,Bytes);}
UINTN EFIAPI StrSize(CONST CHAR16 *Name){assert(FALSE);return 0;}
UINT32 PianoGptCrc32(CONST VOID *Data,UINTN Bytes){return 0;}
static EFI_STATUS EFIAPI Next(UINTN *Bytes,CHAR16 *Name,EFI_GUID *Guid){
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Open(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs,EFI_FILE_PROTOCOL **Root){
  ++Opens;
  return OpenStatus;
}
EFI_RUNTIME_SERVICES Rt={.GetNextVariableName=Next};
EFI_RUNTIME_SERVICES *gRT=&Rt;
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs={.OpenVolume=Open};
static VOID Reset(EFI_STATUS Status){
  mFs=&Fs;
  mStarted=TRUE;
  mSaveError=EFI_SUCCESS;
  mProtocol.Frozen=FALSE;
  mProtocol.Changes=1;
  mSavedChanges=0;
  mSequence=1;
  mRestoredBootNext=42;
  OpenStatus=Status;
  Opens=0;
}
int main(void){
  Reset(EFI_VOLUME_FULL);
  assert(PianoNvFileCanConsumeBootNext(42));
  assert(PianoNvFileSave(FALSE)==EFI_VOLUME_FULL && Opens==1);
  assert(!PianoNvFileCanConsumeBootNext(42));
  assert(PianoNvFileSave(TRUE)==EFI_SUCCESS && mProtocol.Frozen && Opens==1);

  Reset(EFI_WRITE_PROTECTED);
  assert(PianoNvFileSave(TRUE)==EFI_SUCCESS && mProtocol.Frozen && Opens==1);

  Reset(EFI_SUCCESS);
  mSequence=MAX_UINT64;
  assert(PianoNvFileSave(FALSE)==EFI_OUT_OF_RESOURCES && Opens==0);
  assert(PianoNvFileSave(TRUE)==EFI_SUCCESS && mProtocol.Frozen && Opens==0);

  Reset(EFI_DEVICE_ERROR);
  assert(PianoNvFileSave(FALSE)==EFI_DEVICE_ERROR && Opens==1);
  assert(PianoNvFileSave(TRUE)==EFI_DEVICE_ERROR && !mProtocol.Frozen && Opens==1);
  return 0;
}
''','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
    '-Wno-misleading-indentation','-I'+str(nv/'Include'),
    '-I'+str(ROOT/'uefi/core/storage'))
 def test_actual_runtime_properties_keep_variable_reads(self):
  self.compile_and_run('''#include <assert.h>
#undef NULL
#include "'''+str(RTC)+'''"
EFI_GUID gEfiRtPropertiesTableGuid=EFI_RT_PROPERTIES_TABLE_GUID;
static EFI_RT_PROPERTIES_TABLE *inherited,*published;
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(EFI_GUID *Guid,VOID **Table){
  assert(Guid==&gEfiRtPropertiesTableGuid);
  *Table=inherited;
  return inherited?EFI_SUCCESS:EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Publish(EFI_GUID *Guid,VOID *Table){
  assert(Guid==&gEfiRtPropertiesTableGuid);
  published=Table;
  return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES Services={.InstallConfigurationTable=Publish};
EFI_BOOT_SERVICES *gBS=&Services;
int main(void){
  const UINT32 readers=EFI_RT_SUPPORTED_GET_VARIABLE|EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME|
                      EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO;
  assert(LibRtcInitialize(NULL,NULL)==EFI_SUCCESS);
  assert(published->Length==sizeof(*published));
  assert((published->RuntimeServicesSupported&readers)==readers);
  assert(!(published->RuntimeServicesSupported&EFI_RT_SUPPORTED_SET_VARIABLE));
  EFI_RT_PROPERTIES_TABLE input={EFI_RT_PROPERTIES_TABLE_VERSION,sizeof(input),
    EFI_RT_SUPPORTED_GET_VARIABLE|EFI_RT_SUPPORTED_SET_VARIABLE|EFI_RT_SUPPORTED_SET_TIME|
    EFI_RT_SUPPORTED_RESET_SYSTEM};
  inherited=&input;
  assert(LibRtcInitialize(NULL,NULL)==EFI_SUCCESS);
  assert(published->RuntimeServicesSupported==(EFI_RT_SUPPORTED_GET_VARIABLE|
                                                EFI_RT_SUPPORTED_RESET_SYSTEM));
  --input.Length;
  assert(LibRtcInitialize(NULL,NULL)==EFI_COMPROMISED_DATA);
  return 0;
}
''')
 def test_pinned_and_idempotent(self):
  with tempfile.TemporaryDirectory()as d:
   root=Path(d);path=self.pinned_sources(root)
   first=guard.prepare(root,apply=True,check_pin=False);self.assertEqual(first,guard.prepare(root,apply=True,check_pin=False))
   text=path.read_text().replace('return EFI_UNSUPPORTED;\n  }\n\n  AuthFormat',
                                 'return EFI_SUCCESS;\n  }\n\n  AuthFormat',1);path.write_text(text)
   with self.assertRaises(ValueError):guard.prepare(root,apply=True,check_pin=False)
if __name__=='__main__':unittest.main()
