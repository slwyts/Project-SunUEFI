#!/usr/bin/env python3
"""Deterministic native late-APP EBS hooks; dry transform unless apply=True."""
from pathlib import Path
import hashlib,json,subprocess,shutil
ROOT=Path(__file__).resolve().parents[1]
BASE='upstream/Mu-Silicium/Mu_Basecore/'
PINS={
 BASE+'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c':'3679d1a6fbf77411b6aec6d93ca33f92745a6c0e61c7ddd6192c3e42671332f9',
 BASE+'MdeModulePkg/Core/Dxe/Image/Image.c':'3da6c13829490d35e19aedeb32d946664873ed5e116ce13211f0932922c72b64',
 BASE+'MdeModulePkg/Core/Dxe/Mem/Page.c':'fa8880138742041263701de921c0c266662e24b53af4e6da8488ac0960a19e87',
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
    text=raw.decode().replace('\r\n','\n')
    if relative.endswith('DxeMain/DxeMain.c'):
        text=text.replace('#include "DxeMain.h"\n','#include "DxeMain.h"\n'+PROTOTYPES,1)
        old='''  //
  // Notify other drivers of their last chance to use boot services
'''
        if text.count(old)!=1:raise ValueError('Native EBS boundary drift')
        text=text.replace(old,EXIT_HOOK+old,1)
    elif relative.endswith('Image/Image.c'):text+='\n'+IMAGE_GETTER
    elif relative.endswith('Mem/Page.c'):text+='\n'+KEY_GETTER
    else:raise ValueError(relative)
    return text.replace('\n',newline.decode()).encode()


def desired_core(root=ROOT):
    root=Path(root);result={}
    for relative,pin in PINS.items():
        raw=(root/relative).read_bytes()
        if hashlib.sha256(raw).hexdigest()!=pin:
            # Exact installed form may be verified without silently accepting
            # edits to the native root function or the added hook/getters.
            newline=b'\r\n'if b'\r\n'in raw else b'\n';text=raw.decode().replace('\r\n','\n')
            if relative.endswith('DxeMain/DxeMain.c'):
                if text.count(PROTOTYPES)!=1 or text.count(EXIT_HOOK)!=1:raise ValueError('Native Core hook drift: '+relative)
                text=text.replace(PROTOTYPES,'',1).replace(EXIT_HOOK,'',1)
            else:
                suffix='\n'+(IMAGE_GETTER if relative.endswith('Image/Image.c')else KEY_GETTER)
                if not text.endswith(suffix)or text.count(suffix)!=1:raise ValueError('Native getter drift: '+relative)
                text=text[:-len(suffix)]
            original=text.replace('\n',newline.decode()).encode()
            if hashlib.sha256(original).hexdigest()!=pin or raw!=transform_core(relative,original):
                raise ValueError('Native Core source drift: '+relative)
            result[relative]=raw
        else:result[relative]=transform_core(relative,raw)
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
    source=root/'bootprofiles/product-handoff/Mu_Basecore'
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
    # Relative ../uefi-app references use the existing shared header mirror.
    required=('PianoProductOwners.h','PianoCpuInput.h')
    for name in required:
        canonical=root/'bootprofiles/uefi-app'/name
        mirror=app/'uefi-app'/name
        if not mirror.exists()or mirror.read_bytes()!=canonical.read_bytes():
            raise ValueError('Late provider shared header stale: '+name)
    folder.mkdir()
    for name in ('PianoLateHandoff.c','PianoLateHandoff.h'):
        shutil.copyfile(root/'bootprofiles/product-handoff'/name,folder/name)
    return {'status':'ROOT_PROVIDER_COMPILED_NOT_PLATFORM_ARMED',
        'sources':['LateHandoff/PianoLateHandoff.c','LateHandoff/PianoLateHandoff.h'],
        'files':{str(path.relative_to(app)):hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in sorted(folder.iterdir())},'platform_bound':False}


def verify_provider(root,app,record):
    root,app=Path(root),Path(app)
    expected={'LateHandoff/'+name:hashlib.sha256((root/'bootprofiles/product-handoff'/name).read_bytes()).hexdigest()
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
