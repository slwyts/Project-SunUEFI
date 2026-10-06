#!/usr/bin/env python3
"""Bind a pinned product SEC observer; no device operation or DDR expansion."""
from pathlib import Path
import hashlib
import shutil

ROOT=Path(__file__).resolve().parents[1]
SEC_SOURCE='77639c5a4ca526b199e9bcff2271625bac9952de8b3766d6a2ec03719c5da783'
SEC_TEXT='3c54d7b49d19f3a1f2165e4547466af142a97402bc8677c34700a295852a91be'
SEC_INF='a340343850e43c28ecca533a2623590ddef3513219848157353a02821458e9f3'
SEC_INF_TEXT='a4fb8f9de31be2b7848848cd7c7b1d6dc3bfda1d4fa5c75cba8eb471849efea1'
SEC_FILES={
    'AArch64/ArchSec.c':'900d8d655eeb7422b6ff8b055b7941d96d35fc6c520bf07e37613a474bb6d771',
    'AArch64/Exception.S':'6cbd46db3d3c5cab457d93271b4eba7c0bdff6be2a8b1d5c3d71f3b17584eed2',
    'AArch64/Helper.S':'275a611151a43d7776c965b613d1b0bb712d63ba81a8b6b616f26c4c42d13ac9',
    'AArch64/ModuleEntryPoint.S':'ac62466d82cb6f1cfe3a67d59de304a60e555fac142dc36c80dace94f53fc149',
    'Sec.c':SEC_SOURCE,'Sec.inf':SEC_INF,
    'Sec.h':'614b0416843e46abd57e7c9ba72b8ea3a9ded4802e3e07d474c6ea2583a88784',
}
EARLY_FILES=('PianoEarlyMemory.c','PianoEarlyMemory.h','PianoSecRead32.S',
             'PianoSmemRam.c','PianoSmemRam.h','PianoSmemDescriptor.c','PianoSmemDescriptor.h',
             'PianoColdBootObjects.c','PianoColdBootObjects.h','PianoColdBootObjectsContract.c')
OBJECT_DXE_FILES={'PianoProductBootObjects.c':'bootprofiles/uefi-app/PianoProductBootObjects.c',
                 'PianoProductBootObjects.h':'bootprofiles/uefi-app/PianoProductBootObjects.h',
                 'PianoColdBootObjects.h':'bootprofiles/early-memory/PianoColdBootObjects.h',
                 'PianoColdBootObjectsContract.c':'bootprofiles/early-memory/PianoColdBootObjectsContract.c'}
OBJECT_DXE_SOURCES=('PianoProductBootObjects.c','PianoColdBootObjectsContract.c')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sec_inf(text):
    if hashlib.sha256(text.encode()).hexdigest()!=SEC_INF_TEXT:
        raise ValueError('SEC INF drift')
    text=text.replace('BASE_NAME                      = Sec','BASE_NAME                      = PianoProductSec')
    text=text.replace('9AFFB503-E643-4141-8B90-17E8588B1D35','892BCA3B-55ED-4F2B-8750-534543504941')
    text=text.replace('  Sec.c','  Sec.c\n  PianoEarlyMemory.c\n  PianoSmemRam.c\n  PianoSmemDescriptor.c\n  PianoColdBootObjects.c\n  PianoColdBootObjectsContract.c',1)
    return text.replace('  AArch64/ArchSec.c','  PianoSecRead32.S\n  AArch64/ArchSec.c',1)


def sec_source(text):
    """Exact pinned SEC source with the sole cold boundary and report HOB."""
    if hashlib.sha256(text.encode()).hexdigest()!=SEC_TEXT:
        raise ValueError('SEC source drift: re-audit first MemoryPeim ordering')
    text=text.replace('#include "Sec.h"','#include "Sec.h"\n#include "PianoEarlyMemory.h"\n#include "PianoColdBootObjects.h"',1)
    anchor='  // Locate "DXE Heap" Memory Region'
    if text.count(anchor)!=1:
        raise ValueError('SEC first memory boundary ambiguous')
    text=text.replace(anchor,'  // Product SEC observation before the first PHIT/MMU. Failures keep\n'
        '  // the native low map; observation never authorizes high DDR.\n'
        '  PianoColdBootObjectsObserve ();\n'
        '  PianoEarlyMemoryObserveCold ();\n\n'+anchor,1)
    anchor='  PrePeiSetHobList (HobList);'
    if text.count(anchor)!=1:
        raise ValueError('SEC PHIT publication boundary ambiguous')
    text=text.replace(anchor,anchor+'\n\n  // Immutable diagnostic HOB, not RAM/resource/allocation authority.\n'
        '  PianoEarlyMemoryPublishHob ();\n'
        '  PianoColdBootObjectsPublishHob ();',1)
    if text.count('Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);')!=1:
        raise ValueError('SEC must call the original MemoryPeim exactly once')
    return text

def bootshim_digest(root):
    actual=Path(root)/'bootprofiles/handoff/BootShim.S'
    candidate=Path(root)/'bootprofiles/early-memory/PianoBootObjectsShim.S'
    if actual.read_bytes()!=candidate.read_bytes():
        raise ValueError('BootObjects BootShim differs from reviewed extension source')
    return sha(actual)

def bind_object_sources(text):
    if text.count('[Sources]\n')!=1:raise ValueError('ProductCore INF source boundary ambiguous')
    source=text.split('[Sources]\n',1)[1].split('[',1)[0].splitlines()
    names=[line.strip()for line in source if line.strip()]
    if any(name in names for name in OBJECT_DXE_SOURCES):raise ValueError('Cold object consumer already staged')
    return text.replace('[Sources]\n','[Sources]\n'+''.join('  '+name+'\n'for name in OBJECT_DXE_SOURCES),1)


def prepare(root=ROOT,target=None):
    root=Path(root);target=Path(target) if target else root/'platforms/pianoProductPkg'
    source=root/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec'
    actual={str(path.relative_to(source)):sha(path)for path in source.rglob('*')if path.is_file()}
    if actual!=SEC_FILES:
        raise ValueError('SEC source/assembly/INF drift: audit entry, vector and dependencies')
    transformed=sec_source((source/'Sec.c').read_text());shim_sha=bootshim_digest(root)
    destination=target/'Sec'
    if destination.exists():
        raise ValueError('Early SEC already staged; rebuild product preparation')
    dsc=target/'pianoProduct.dsc';fdf=target/'pianoProduct.fdf'
    dsc_text=dsc.read_text();fdf_text=fdf.read_text()
    original='  INF SiliciumPkg/Sec/Sec.inf'
    if fdf_text.count(original)!=1:
        raise ValueError('Product FDF must contain exactly one SEC')
    shutil.copytree(source,destination)
    (destination/'Sec.c').write_text(transformed)
    for name in EARLY_FILES:
        shutil.copyfile(root/'bootprofiles/early-memory'/name,destination/name)
    (destination/'Sec.inf').write_text(sec_inf((destination/'Sec.inf').read_text()))
    dsc.write_text(dsc_text+'\n[Components]\n  pianoProductPkg/Sec/Sec.inf {\n'
        '    <LibraryClasses>\n'
        '      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf\n'
        '  }\n')
    fdf.write_text(fdf_text.replace(original,'  INF pianoProductPkg/Sec/Sec.inf',1))
    # Header is shared with the consumer; observer implementation lives only in
    # SEC and cannot accidentally link into a DXE app as a second initializer.
    app=target/'Applications/ProductCore'
    if app.exists():
        shutil.copyfile(root/'bootprofiles/early-memory/PianoEarlyMemory.h',app/'PianoEarlyMemory.h')
        for name,canonical in OBJECT_DXE_FILES.items():shutil.copyfile(root/canonical,app/name)
        inf=app/'ProductCore.inf';inf.write_text(bind_object_sources(inf.read_text()))
    staged={str(path.relative_to(target)):sha(path)for path in sorted(destination.rglob('*'))if path.is_file()}
    if app.exists():staged.update({str((app/name).relative_to(target)):sha(app/name)for name in OBJECT_DXE_FILES})
    return {'status':'COLD_SEC_OBSERVER_BOUND_NOT_HARDWARE_VERIFIED',
        'sec_source_sha256':SEC_SOURCE,'sec_inf_sha256':SEC_INF,
        'first_hook':'InitializeMemory before LocateMemoryRegionByName / HobConstructor',
        'report_hook':'after PrePeiSetHobList before sole MemoryPeim',
        'hob_guid':'495ec035-44a5-4f17-8750-5049414e4f31',
        'objects_hob_guid':'3025a79b-7ed3-4f6b-9050-434f4c443031',
        'bootshim_sha256':shim_sha,'objects_observation_bound':True,
        'high_ddr_published':False,'memory_ownership_granted':False,
        'fallback':'unchanged corrected native low memory table',
        'files':staged}


def verify(root,target,record):
    root,target=Path(root),Path(target)
    source=root/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec'
    actual={str(path.relative_to(source)):sha(path)for path in source.rglob('*')if path.is_file()}
    if actual!=SEC_FILES:raise ValueError('Original SEC source drifted after preparation')
    expected={'Sec/'+name:digest for name,digest in SEC_FILES.items()}
    expected['Sec/Sec.c']=hashlib.sha256(sec_source((source/'Sec.c').read_text()).encode()).hexdigest()
    expected['Sec/Sec.inf']=hashlib.sha256(sec_inf((source/'Sec.inf').read_text()).encode()).hexdigest()
    expected.update({'Sec/'+name:sha(root/'bootprofiles/early-memory'/name)for name in EARLY_FILES})
    app=target/'Applications/ProductCore'
    if app.exists():expected.update({str((app/name).relative_to(target)):sha(root/canonical)for name,canonical in OBJECT_DXE_FILES.items()})
    if record.get('files')!=expected or record.get('high_ddr_published')is not False or record.get('memory_ownership_granted')is not False:
        raise ValueError('Cold SEC preparation record differs from canonical bounded observer')
    if record.get('bootshim_sha256')!=bootshim_digest(root)or record.get('objects_observation_bound')is not True or record.get('objects_hob_guid')!='3025a79b-7ed3-4f6b-9050-434f4c443031':
        raise ValueError('Cold BootObjects handoff/producer record differs')
    for name,digest in expected.items():
        path=target/name
        if path.is_symlink()or not path.is_file()or sha(path)!=digest:raise ValueError('Cold SEC compiled copy stale: '+name)
    fdf=(target/'pianoProduct.fdf').read_text()
    if fdf.count('  INF pianoProductPkg/Sec/Sec.inf')!=1 or '  INF SiliciumPkg/Sec/Sec.inf'in fdf:
        raise ValueError('Product FDF must bind the sole actual cold SEC observer')
    dsc=(target/'pianoProduct.dsc').read_text()
    if dsc.count('  pianoProductPkg/Sec/Sec.inf {')!=1:raise ValueError('Product DSC omits or duplicates cold SEC')
    if app.exists():
        header=app/'PianoEarlyMemory.h'
        if not header.is_file()or sha(header)!=sha(root/'bootprofiles/early-memory/PianoEarlyMemory.h'):
            raise ValueError('ProductCore early HOB header differs from the actual SEC producer')
        classes=(app/'ProductCore.inf').read_text().split('[LibraryClasses]\n',1)[1].split('[Guids]',1)[0].splitlines()
        if 'HobLib'not in {line.strip()for line in classes}:raise ValueError('ProductCore lacks the actual HOB consumer library')
        sources=[line.strip()for line in (app/'ProductCore.inf').read_text().split('[Sources]\n',1)[1].split('[',1)[0].splitlines()]
        if any(sources.count(name)!=1 for name in OBJECT_DXE_SOURCES):raise ValueError('ProductCore cold consumer source missing or duplicated')
        if 'PianoColdBootObjects.c'in sources or 'PianoEarlyMemory.c'in sources:raise ValueError('SEC collector cannot link into DXE')
    return True
