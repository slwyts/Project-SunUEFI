#!/usr/bin/env python3
"""Connect VariableRuntimeDxe to the boot-time variable store.

The product restores ordinary variables through SetVariable and saves them
through its boot-service file backend. This does not activate standard FVB/FTW
or make persistent runtime writes available.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess
ROOT=Path(__file__).resolve().parents[1]
BASE='upstream/Mu-Silicium/Mu_Basecore'
PIN=json.loads((ROOT/'sources.lock.json').read_text())['Mu-Silicium/Mu_Basecore']['commit']
FILE=BASE+'/MdeModulePkg/Universal/Variable/RuntimeDxe/Variable.c'
from prepare_product_pump import transform
def prepare(root=ROOT,apply=False,check_pin=True):
 if check_pin and subprocess.check_output(['git','-C',str(root/BASE),'rev-parse','HEAD'],text=True).strip()!=PIN:raise ValueError('wrong VariableRuntimeDxe source pin')
 files={}
 # A boot-only file backend restores through standard SetVariable. Notify it
 # of successful changes; never replace the standard cache or runtime table.
 header='MdePkg/Include/Protocol/PianoNvFile.h'
 destination=root/BASE/header
 source=root/'uefi/components/nv/Include/Protocol/PianoNvFile.h'
 if apply:
  destination.parent.mkdir(parents=True,exist_ok=True);destination.write_bytes(source.read_bytes())
 elif not destination.is_file()or destination.read_bytes()!=source.read_bytes():raise ValueError('NV file protocol header differs')
 files[BASE+'/'+header]=hashlib.sha256(source.read_bytes()).hexdigest()
 dxe=root/BASE/'MdeModulePkg/Universal/Variable/RuntimeDxe/VariableDxe.c'
 raw=dxe.read_bytes();line='\r\n'if b'\r\n'in raw else'\n';text=raw.decode().replace('\r\n','\n')
 include='#include "Variable.h"\n'
 block='''#include "Variable.h"
#include <Protocol/PianoNvFile.h>
PIANO_NV_FILE_PROTOCOL *mPianoNvFile;
STATIC EFI_GUID mPianoNvFileGuid=PIANO_NV_FILE_PROTOCOL_GUID;
STATIC VOID EFIAPI PianoNvFileNotify(EFI_EVENT Event,VOID *Context){
  (VOID)Event;(VOID)Context;
  gBS->LocateProtocol(&mPianoNvFileGuid,NULL,(VOID **)&mPianoNvFile);
}
'''
 text=transform(text,include,block,str(dxe))
 anchor='  Status = VariableCommonInitialize ();'
 replacement='''  EFI_EVENT NvFileEvent;
  VOID *NvFileRegistration;
  NvFileEvent=EfiCreateProtocolNotifyEvent(&mPianoNvFileGuid,TPL_CALLBACK,PianoNvFileNotify,NULL,&NvFileRegistration);
  (VOID)NvFileEvent;
  Status = VariableCommonInitialize ();'''
 text=transform(text,anchor,replacement,str(dxe))
 value=text.replace('\n',line).encode()
 if apply:dxe.write_bytes(value)
 elif value!=raw:raise ValueError('NV file variable notification not prepared')
 files[str(dxe.relative_to(root))]=hashlib.sha256(value).hexdigest()
 path=root/FILE;raw=path.read_bytes();line='\r\n'if b'\r\n'in raw else'\n';text=raw.decode().replace('\r\n','\n')
 anchor='#include "Variable.h"\n'
 replacement='#include "Variable.h"\n#include <Protocol/PianoNvFile.h>\nextern PIANO_NV_FILE_PROTOCOL *mPianoNvFile;\n'
 text=transform(text,anchor,replacement,FILE)
 # Return before inspecting attributes, pointers or the variable store.
 begin=text.index('\nVariableServiceSetVariable (')
 end=text.index('\n}\n',begin)
 entry=text[begin:end]
 anchor='  AuthFormat = mVariableModuleGlobal->VariableGlobal.AuthFormat;\n'
 marker='  // The product has no persistent variable writer after ExitBootServices.\n'
 replacement=marker+'''\
  if (AtRuntime ()) {
    return EFI_UNSUPPORTED;
  }

'''+anchor
 if marker in entry and (entry.count(marker)!=1 or entry.count(replacement)!=1):
  raise ValueError('Inconsistent SetVariable runtime guard')
 text=text[:begin]+transform(entry,anchor,replacement,FILE)+text[end:]
 anchor='  //\n  // Special Handling for MOR Lock variable.\n'
 replacement='''  // Refuse boot-time NV changes once the file backend is frozen for handoff.
  if (mPianoNvFile != NULL && mPianoNvFile->Frozen) {
    VARIABLE_POINTER_TRACK Existing;
    if ((Attributes & EFI_VARIABLE_NON_VOLATILE) != 0 ||
        (!EFI_ERROR (FindVariable (VariableName,VendorGuid,&Existing,&mVariableModuleGlobal->VariableGlobal,TRUE)) &&
         (Existing.CurrPtr->Attributes & EFI_VARIABLE_NON_VOLATILE) != 0)) {
      return EFI_UNSUPPORTED;
    }
  }
  //
  // Special Handling for MOR Lock variable.
'''
 text=transform(text,anchor,replacement,FILE)
 anchor='  InterlockedDecrement (&mVariableModuleGlobal->VariableGlobal.ReentrantState);'
 replacement='''  if (!EFI_ERROR (Status) && mPianoNvFile != NULL) {
    ++mPianoNvFile->Changes;
  }
  InterlockedDecrement (&mVariableModuleGlobal->VariableGlobal.ReentrantState);'''
 text=transform(text,anchor,replacement,FILE)
 value=text.replace('\n',line).encode()
 if apply:path.write_bytes(value)
 elif value!=raw:raise ValueError('NV file variable change hook not prepared')
 files[FILE]=hashlib.sha256(value).hexdigest()
 return {'pin':PIN,'files':files,'runtime_set_variable':'EFI_UNSUPPORTED','physical_nv_ready':False}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('operation',choices=('apply','verify'));a=p.parse_args();print(json.dumps(prepare(apply=a.operation=='apply'),indent=2))
