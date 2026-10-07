#!/usr/bin/env python3
"""Deterministic native late-APP EBS hooks; dry transform unless apply=True."""
from pathlib import Path
import hashlib,json,subprocess,shutil
from source_input_tail import split_source_tail
ROOT=Path(__file__).resolve().parents[1]
BASE='upstream/Mu-Silicium/Mu_Basecore/'
# Audited LF source bodies. Only independent footer comments/whitespace vary;
# function structure and the native EBS/getter behavior remain byte-identical.
PINS={
 BASE+'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c':'cd9d91719c7c5bdf0d01f4579034bc5ad31763bbc219f8a9c43a349869685471',
 BASE+'MdeModulePkg/Core/Dxe/Image/Image.c':'6790ba708a531f866db05ce3b1abe9edccebb6c79513bafadc5332e2d5254983',
 BASE+'MdeModulePkg/Core/Dxe/Mem/Page.c':'ec16ef78e7c817fbc7e238ae94ce90d8662d0e3872fdd44e92472505fe1c816a',
}
IMAGE_GETTER='''
// Native current StartImage frame; never trust the caller's handle alone.
EFI_LOADED_IMAGE_PROTOCOL *
CorePianoCurrentImageInfo (IN EFI_HANDLE ImageHandle)
{
  if (mCurrentImage == NULL || mCurrentImage->Signature != LOADED_IMAGE_PRIVATE_DATA_SIGNATURE || !mCurrentImage->Started ||
      mCurrentImage->Handle != ImageHandle ||
      mCurrentImage->ImageContext.ImageType != EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION) {
    return NULL;
  }
  return &mCurrentImage->Info;
}
'''
KEY_GETTER='''
// Read-only native key snapshot. No allocation/map termination/BS call.
UINTN
CorePianoCurrentMemoryMapKey (VOID)
{
  UINTN Key;
  CoreAcquireMemoryLock ();
  Key = mMemoryMapKey;
  CoreReleaseMemoryLock ();
  return Key;
}
'''
PROTOTYPES='''#include <Library/PianoProductExitLib.h>
EFI_LOADED_IMAGE_PROTOCOL *CorePianoCurrentImageInfo (EFI_HANDLE ImageHandle);
UINTN CorePianoCurrentMemoryMapKey (VOID);
'''
EXIT_HOOK='''  // Product native APP transition before any BeforeNotify/Timer0/map teardown.
  // Legacy targets bind Null. A stale key or foreign image does not stop USB.
  if (gMemoryMapTerminated) {
    return EFI_NOT_READY;
  }
  Status = PianoProductBeforeExitBootServices (
             ImageHandle, CorePianoCurrentImageInfo (ImageHandle), gEfiCurrentTpl,
             MapKey, CorePianoCurrentMemoryMapKey (), mExitBootServicesCalled);
  if (Status != EFI_SUCCESS) {
    return Status;
  }

'''


def transform_core(relative,raw):
    newline=b'\r\n'if b'\r\n'in raw else b'\n'
    text,tail=split_source_tail(raw.decode())
    if relative.endswith('DxeMain/DxeMain.c'):
        if text.count('#include "DxeMain.h"\n')!=1:raise ValueError('Native Core include boundary drift')
        text=text.replace('#include "DxeMain.h"\n','#include "DxeMain.h"\n'+PROTOTYPES,1)
        old='''  //
  // Notify other drivers of their last chance to use boot services
'''
        if text.count(old)!=1:raise ValueError('Native EBS boundary drift')
        text=text.replace(old,EXIT_HOOK+old,1)
    elif relative.endswith('Image/Image.c'):text+='\n\n'+IMAGE_GETTER.rstrip(' \t\n')
    elif relative.endswith('Mem/Page.c'):text+='\n\n'+KEY_GETTER.rstrip(' \t\n')
    else:raise ValueError(relative)
    return (text+tail).replace('\n',newline.decode()).encode()


def desired_core(root=ROOT):
    root=Path(root);result={};installed=[]
    for relative,pin in PINS.items():
        raw=(root/relative).read_bytes()
        body,tail=split_source_tail(raw.decode())
        has_hook=hashlib.sha256(body.encode()).hexdigest()!=pin
        installed.append(has_hook)
        if has_hook:
            # Exact installed form may be verified without silently accepting
            # edits to the native root function or the added hook/getters.
            newline=b'\r\n'if b'\r\n'in raw else b'\n';text=body
            if relative.endswith('DxeMain/DxeMain.c'):
                if text.count(PROTOTYPES)!=1 or text.count(EXIT_HOOK)!=1:raise ValueError('Native Core hook drift: '+relative)
                text=text.replace(PROTOTYPES,'',1).replace(EXIT_HOOK,'',1)
            else:
                suffix='\n\n'+(IMAGE_GETTER if relative.endswith('Image/Image.c')else KEY_GETTER).rstrip(' \t\n')
                if not text.endswith(suffix)or text.count(suffix)!=1:raise ValueError('Native getter drift: '+relative)
                text=text[:-len(suffix)]
            original=(text+tail).replace('\n',newline.decode()).encode()
            if hashlib.sha256(text.encode()).hexdigest()!=pin or raw!=transform_core(relative,original):
                raise ValueError('Native Core source drift: '+relative)
            result[relative]=raw
        else:result[relative]=transform_core(relative,raw)
    if len(set(installed))!=1:
        raise ValueError('Native Core hooks are partially installed')
    return result


def _insert(text,anchor,line,label):
    if text.count(line)==1:
        if text.replace(line,'',1).count(anchor)!=1:raise ValueError('Duplicate anchor: '+label)
        return text
    if line in text or text.count(anchor)!=1:raise ValueError('Binding drift: '+label)
    return text.replace(anchor,anchor+line,1)


def prepare(root=ROOT,apply=False):
    root=Path(root);outputs=desired_core(root)
    for relative,line in (
        (BASE+'MdeModulePkg/Core/Dxe/DxeMain.inf','  PianoProductExitLib\n'),
        (BASE+'MdePkg/MdePkg.dec','  PianoProductExitLib|Include/Library/PianoProductExitLib.h\n'),
        (BASE+'MdePkg/MdeLibs.dsc.inc','  PianoProductExitLib|MdePkg/Library/PianoProductExitLibNull/PianoProductExitLibNull.inf\n'),
        ('upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc',
         '  PianoProductExitLib|MdePkg/Library/PianoProductExitLibNull/PianoProductExitLibNull.inf\n')):
        path=root/relative;raw=path.read_bytes();newline=b'\r\n'if b'\r\n'in raw else b'\n'
        text=raw.decode().replace('\r\n','\n');anchor='[LibraryClasses]\n'
        if relative.endswith('SiliciumPkg.dsc.inc'):
            anchor='!include MdePkg/MdeLibs.dsc.inc\n\n[LibraryClasses]\n'
        outputs[relative]=_insert(text,anchor,line,relative).replace('\n',newline.decode()).encode()
    source=root/'uefi/components/product-handoff/Mu_Basecore'
    for path in source.rglob('*'):
        if path.is_file():outputs[BASE+path.relative_to(source).as_posix()]=path.read_bytes()
    # Construct/validate every output before any mutation. apply=False is a
    # reviewable desired-state report, not a claim hooks are currently installed.
    if apply:
        for relative,raw in outputs.items():
            path=root/relative;path.parent.mkdir(parents=True,exist_ok=True)
            if not path.exists()or path.read_bytes()!=raw:path.write_bytes(raw)
    return {'status':'NATIVE_LATE_APP_HOOK_DESIRED_NOT_APPLIED'if not apply else 'NATIVE_LATE_APP_HOOK_APPLIED_NOT_DEVICE_VERIFIED',
        'default_library':'PianoProductExitLibNull','product_library_override_required':True,
        'source_pins':PINS,'files':{name:hashlib.sha256(raw).hexdigest()for name,raw in sorted(outputs.items())},
        'product_provider_bound':False,'device_boot_performed':False}


def stage_provider(root,app):
    """Stage exact Root provider source; does not initialize/Arm any image."""
    root=Path(root);app=Path(app);folder=app/'LateHandoff'
    if folder.exists():raise ValueError('Late provider already staged')
    # The generated module contains the canonical core header mirror.
    required=('PianoProductOwners.h','PianoCpuInput.h')
    for name in required:
        canonical=root/'uefi/core'/name
        mirror=app/'core'/name
        if not mirror.exists()or mirror.read_bytes()!=canonical.read_bytes():
            raise ValueError('Late provider shared header stale: '+name)
    folder.mkdir()
    for name in ('PianoLateHandoff.c','PianoLateHandoff.h'):
        shutil.copyfile(root/'uefi/components/product-handoff'/name,folder/name)
    return {'status':'ROOT_PROVIDER_COMPILED_NOT_PLATFORM_ARMED',
        'sources':['LateHandoff/PianoLateHandoff.c','LateHandoff/PianoLateHandoff.h'],
        'files':{str(path.relative_to(app)):hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in sorted(folder.iterdir())},'platform_bound':False}


def verify_provider(root,app,record):
    root,app=Path(root),Path(app)
    expected={'LateHandoff/'+name:hashlib.sha256((root/'uefi/components/product-handoff'/name).read_bytes()).hexdigest()
              for name in ('PianoLateHandoff.c','PianoLateHandoff.h')}
    if record.get('files')!=expected:raise ValueError('Late provider canonical manifest differs')
    for name,digest in expected.items():
        path=app/name
        if path.is_symlink()or not path.is_file()or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise ValueError('Compiled late provider source differs: '+name)
    inf=(app/'ProductCore.inf').read_text()
    sources=inf.split('[Sources]\n',1)[1].split('[Packages]',1)[0]
    if not all('  '+name+'\n'in sources for name in expected):raise ValueError('ProductCore omits actual late provider source')
    return True


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('operation',choices=('apply','verify'))
    args=parser.parse_args();record=prepare(apply=args.operation=='apply')
    if args.operation=='verify':
        for name,digest in record['files'].items():
            path=ROOT/name
            if not path.is_file()or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
                raise ValueError('Native late handoff hook is not installed: '+name)
    print(json.dumps(record,indent=2))
