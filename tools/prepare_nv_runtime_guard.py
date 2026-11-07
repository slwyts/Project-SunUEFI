#!/usr/bin/env python3
"""Prepare standard VariableRuntimeDxe notifications and runtime-NV guards.

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
HOOKS=[]
def hook(old,new):HOOKS.append((old,new))
hook('#include "Variable.h"\n','#include "Variable.h"\n#ifndef PIANO_NV_BOOT_ONLY\n#define PIANO_NV_BOOT_ONLY 0\n#endif\n#if PIANO_NV_BOOT_ONLY != 0 && PIANO_NV_BOOT_ONLY != 1\n#error PIANO_NV_BOOT_ONLY must be 0 or 1\n#endif\n')
hook('  //\n  // Special Handling for MOR Lock variable.\n',
     '#if PIANO_NV_BOOT_ONLY\n  // The UFS owner is retired before OS handoff. Refuse NV before MOR/auth\n  // callbacks or cache mutation, including attribute-zero deletion.\n  if (AtRuntime ()) {\n    if ((Attributes & EFI_VARIABLE_NON_VOLATILE) != 0) {\n      return EFI_UNSUPPORTED;\n    }\n    VARIABLE_POINTER_TRACK NvExisting;\n    EFI_STATUS NvLookup = FindVariable (VariableName, VendorGuid, &NvExisting, &mVariableModuleGlobal->VariableGlobal, TRUE);\n    if (!EFI_ERROR (NvLookup) && ((NvExisting.CurrPtr->Attributes & EFI_VARIABLE_NON_VOLATILE) != 0)) {\n      return EFI_UNSUPPORTED;\n    }\n  }\n#endif\n\n  //\n  // Special Handling for MOR Lock variable.\n')
hook('  if (!EFI_ERROR (Status)) {\n    if (((Variable.CurrPtr->Attributes & EFI_VARIABLE_RUNTIME_ACCESS) == 0) && AtRuntime ()) {',
     '  if (!EFI_ERROR (Status)) {\n#if PIANO_NV_BOOT_ONLY\n    // Attribute-zero deletes must also refuse an existing NV variable before\n    // auth/policy/language processing or any store mutation.\n    if (AtRuntime () && ((Variable.CurrPtr->Attributes & EFI_VARIABLE_NON_VOLATILE) != 0)) {\n      Status = EFI_UNSUPPORTED;\n      goto Done;\n    }\n#endif\n    if (((Variable.CurrPtr->Attributes & EFI_VARIABLE_RUNTIME_ACCESS) == 0) && AtRuntime ()) {')
def prepare(root=ROOT,apply=False,check_pin=True):
 if check_pin and subprocess.check_output(['git','-C',str(root/BASE),'rev-parse','HEAD'],text=True).strip()!=PIN:raise ValueError('wrong VariableRuntimeDxe source pin')
 path=root/FILE;raw=path.read_bytes();line='\r\n'if b'\r\n'in raw else'\n';text=raw.decode().replace('\r\n','\n')
 for old,new in HOOKS:
  # The file-backend freeze check is inserted before the same MOR anchor;
  # leave that complete already-prepared block intact on verification.
  if 'PIANO_NV_FILE_PROTOCOL' in text and old.startswith('  //\n  // Special Handling'):
   if text.count('#if PIANO_NV_BOOT_ONLY\n  // The UFS owner')!=1:raise ValueError('runtime NV guard absent')
   continue
  if old=='#include "Variable.h"\n' and 'extern PIANO_NV_FILE_PROTOCOL *mPianoNvFile;' in text:continue
  text=transform(text,old,new,FILE)
 wanted=text.replace('\n',line).encode()
 if apply:
  if raw!=wanted:path.write_bytes(wanted)
 elif raw!=wanted:raise ValueError('runtime NV guard absent/stale')
 files={FILE:hashlib.sha256(wanted).hexdigest()}
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
 anchor='  //\n  // Special Handling for MOR Lock variable.\n'
 replacement='''  // Refuse boot-time NV changes once the file backend is frozen for handoff.
  if (!AtRuntime () && mPianoNvFile != NULL && mPianoNvFile->Frozen) {
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
 replacement='''  if (!AtRuntime () && !EFI_ERROR (Status) && mPianoNvFile != NULL) {
    ++mPianoNvFile->Changes;
  }
  InterlockedDecrement (&mVariableModuleGlobal->VariableGlobal.ReentrantState);'''
 text=transform(text,anchor,replacement,FILE)
 value=text.replace('\n',line).encode()
 if apply:path.write_bytes(value)
 elif value!=raw:raise ValueError('NV file variable change hook not prepared')
 files[FILE]=hashlib.sha256(value).hexdigest()
 return {'pin':PIN,'files':files,'product_compile_flag':'PIANO_NV_BOOT_ONLY=1','default_diagnostic_flag':0,'runtime_nv_set':'EFI_UNSUPPORTED including zero-attribute deletion','physical_nv_ready':False}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('operation',choices=('apply','verify'));a=p.parse_args();print(json.dumps(prepare(apply=a.operation=='apply'),indent=2))
