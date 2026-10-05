#!/usr/bin/env python3
"""Pinned standard VariableRuntimeDxe runtime-NV guard; host source only.

Production binding uses -DPIANO_NV_BOOT_ONLY=1. Default diagnostic code stays
unchanged (macro0). This does not activate a disk NV store or claim NVReady.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess
ROOT=Path(__file__).resolve().parents[1]
BASE='upstream/Mu-Silicium/Mu_Basecore'
PIN='bb557081f80f4883ed832e34ab36bdca6ede1e10'
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
 if 'PIANO_NV_BOOT_ONLY'in text and any(text.count(new)!=1 for old,new in HOOKS):raise ValueError('partial, altered or duplicate runtime NV guard')
 for old,new in HOOKS:text=transform(text,old,new,FILE)
 wanted=text.replace('\n',line).encode()
 if apply:
  if raw!=wanted:path.write_bytes(wanted)
 elif raw!=wanted:raise ValueError('runtime NV guard absent/stale')
 return {'pin':PIN,'files':{FILE:hashlib.sha256(wanted).hexdigest()},'product_compile_flag':'PIANO_NV_BOOT_ONLY=1','default_diagnostic_flag':0,'runtime_nv_set':'EFI_UNSUPPORTED including zero-attribute deletion','physical_nv_ready':False}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('operation',choices=('apply','verify'));a=p.parse_args();print(json.dumps(prepare(apply=a.operation=='apply'),indent=2))
