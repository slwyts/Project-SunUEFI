#!/usr/bin/env python3
"""Bind a pinned product SEC observer; no device operation or DDR expansion."""
from pathlib import Path
import hashlib
import shutil
from source_input_tail import split_source_tail

ROOT=Path(__file__).resolve().parents[1]
# Audited LF bodies retain the original instructions, declarations and INF
# bindings. Complete independent footer comments/whitespace are not behavior.
SEC_SOURCE='80802e9c635e3002b6d21b5b2f0505de9f9726fee8a1050d4c6c65cd87f18f26'
SEC_INF='a85d09b0f9c3aba87302ea3897fe9d72d8e47ad3c92b68d9b23b708358833d53'
SEC_FILES={
    'AArch64/ArchSec.c':'ff0804506364966b00e3b29cca228a9a8316f113a6b89a7d6f5af032617eb89f',
    'AArch64/Exception.S':'83f07cd70c26e646d99106f086c6164acb70e13aa40e5e4a5123c8ba5ea7680e',
    'AArch64/Helper.S':'183f0c295e2f42c003f5b2538510f43db20ab854f2b1f098f68f56cc2a2a52f7',
    'AArch64/ModuleEntryPoint.S':'2d41a74edb3c9005aa301a051208f203e3220584944483a377cd3a2075a2029b',
    'Sec.c':SEC_SOURCE,'Sec.inf':SEC_INF,
    'Sec.h':'ec96d634fa0ca82a984ff576ab9ed4c5ce3c9ab813fffb43874624983a0ed90b',
}
EARLY_FILES=('PianoEarlyMemory.c','PianoEarlyMemory.h','PianoSecRead32.S',
             'PianoSmemRam.c','PianoSmemRam.h','PianoSmemDescriptor.c','PianoSmemDescriptor.h',
             'PianoColdBootObjects.c','PianoColdBootObjects.h','PianoColdBootObjectsContract.c','PianoColdSecRead256.S')
OBJECT_DXE_FILES={'PianoProductBootObjects.c':'uefi/core/PianoProductBootObjects.c',
                 'PianoProductBootObjects.h':'uefi/core/PianoProductBootObjects.h',
                 'PianoColdBootObjects.h':'uefi/handoff/early-memory/PianoColdBootObjects.h',
                 'PianoColdBootObjectsContract.c':'uefi/handoff/early-memory/PianoColdBootObjectsContract.c'}
OBJECT_DXE_SOURCES=('PianoProductBootObjects.c','PianoColdBootObjectsContract.c')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sec_inf(text):
    text,tail=split_source_tail(text,inf=True)
    if hashlib.sha256(text.encode()).hexdigest()!=SEC_INF:
        raise ValueError('SEC INF drift')
    text=text.replace('BASE_NAME                      = Sec','BASE_NAME                      = PianoProductSec')
    text=text.replace('9AFFB503-E643-4141-8B90-17E8588B1D35','892BCA3B-55ED-4F2B-8750-534543504941')
    text=text.replace('  Sec.c','  Sec.c\n  PianoEarlyMemory.c\n  PianoSmemRam.c\n  PianoSmemDescriptor.c\n  PianoColdBootObjects.c\n  PianoColdBootObjectsContract.c',1)
    return text.replace('  AArch64/ArchSec.c','  PianoSecRead32.S\n  PianoColdSecRead256.S\n  AArch64/ArchSec.c',1)+tail


def sec_source(text):
    """Pinned SEC behavior with the sole cold boundary and report HOB."""
    text,tail=split_source_tail(text)
    if hashlib.sha256(text.encode()).hexdigest()!=SEC_SOURCE:
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
    return text+tail


def source_inputs(source):
    paths={str(path.relative_to(source)):path for path in source.rglob('*')if path.is_file()}
    if set(paths)!=set(SEC_FILES):
        raise ValueError('SEC source/assembly/INF inventory drift')
    raw={name:path.read_bytes()for name,path in paths.items()}
    for name,pin in SEC_FILES.items():
        body,_=split_source_tail(raw[name].decode(),inf=name.endswith('.inf'))
        if hashlib.sha256(body.encode()).hexdigest()!=pin:
            raise ValueError('SEC source/assembly/INF behavior drift: '+name)
    return raw,{name:hashlib.sha256(data).hexdigest()for name,data in raw.items()}

def bootshim_digest(root):
    actual=Path(root)/'uefi/handoff/bootshim/BootShim.S'
    candidate=Path(root)/'uefi/handoff/early-memory/PianoBootObjectsShim.S'
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
    root=Path(root);target=Path(target) if target else root/'uefi/platforms/pianoProductPkg'
    source=root/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec'
    inputs,actual=source_inputs(source)
    transformed=sec_source(inputs['Sec.c'].decode());shim_sha=bootshim_digest(root)
    destination=target/'Sec'
    if destination.exists():
        raise ValueError('Early SEC already staged; rebuild product preparation')
    dsc=target/'pianoProduct.dsc';fdf=target/'pianoProduct.fdf'
    dsc_text=dsc.read_text();fdf_text=fdf.read_text()
    original='  INF SiliciumPkg/Sec/Sec.inf'
    if fdf_text.count(original)!=1:
        raise ValueError('Product FDF must contain exactly one SEC')
    destination.mkdir(parents=True)
    for name,data in inputs.items():
        path=destination/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
    (destination/'Sec.c').write_text(transformed)
    for name in EARLY_FILES:
        shutil.copyfile(root/'uefi/handoff/early-memory'/name,destination/name)
    (destination/'Sec.inf').write_text(sec_inf(inputs['Sec.inf'].decode()))
    dsc.write_text(dsc_text+'\n[Components]\n  pianoProductPkg/Sec/Sec.inf {\n'
        '    <LibraryClasses>\n'
        '      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf\n'
        '  }\n')
    fdf.write_text(fdf_text.replace(original,'  INF pianoProductPkg/Sec/Sec.inf',1))
    # Header is shared with the consumer; observer implementation lives only in
    # SEC and cannot accidentally link into a DXE app as a second initializer.
    app=target/'Applications/ProductCore'
    if app.exists():
        shutil.copyfile(root/'uefi/handoff/early-memory/PianoEarlyMemory.h',app/'PianoEarlyMemory.h')
        for name,canonical in OBJECT_DXE_FILES.items():shutil.copyfile(root/canonical,app/name)
        inf=app/'ProductCore.inf';inf.write_text(bind_object_sources(inf.read_text()))
    staged={str(path.relative_to(target)):sha(path)for path in sorted(destination.rglob('*'))if path.is_file()}
    if app.exists():staged.update({str((app/name).relative_to(target)):sha(app/name)for name in OBJECT_DXE_FILES})
    if source_inputs(source)[1]!=actual:
        raise ValueError('Original SEC sources changed during preparation')
    return {'status':'COLD_SEC_OBSERVER_BOUND_NOT_HARDWARE_VERIFIED',
        'sec_source_sha256':actual['Sec.c'],'sec_inf_sha256':actual['Sec.inf'],'source_files':actual,
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
    inputs,actual=source_inputs(source)
    if record.get('source_files')!=actual or record.get('sec_source_sha256')!=actual['Sec.c'] or record.get('sec_inf_sha256')!=actual['Sec.inf']:
        raise ValueError('Original SEC source drifted after preparation')
    expected={'Sec/'+name:digest for name,digest in actual.items()}
    expected['Sec/Sec.c']=hashlib.sha256(sec_source(inputs['Sec.c'].decode()).encode()).hexdigest()
    expected['Sec/Sec.inf']=hashlib.sha256(sec_inf(inputs['Sec.inf'].decode()).encode()).hexdigest()
    expected.update({'Sec/'+name:sha(root/'uefi/handoff/early-memory'/name)for name in EARLY_FILES})
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
        if not header.is_file()or sha(header)!=sha(root/'uefi/handoff/early-memory/PianoEarlyMemory.h'):
            raise ValueError('ProductCore early HOB header differs from the actual SEC producer')
        classes=(app/'ProductCore.inf').read_text().split('[LibraryClasses]\n',1)[1].split('[Guids]',1)[0].splitlines()
        if 'HobLib'not in {line.strip()for line in classes}:raise ValueError('ProductCore lacks the actual HOB consumer library')
        sources=[line.strip()for line in (app/'ProductCore.inf').read_text().split('[Sources]\n',1)[1].split('[',1)[0].splitlines()]
        if any(sources.count(name)!=1 for name in OBJECT_DXE_SOURCES):raise ValueError('ProductCore cold consumer source missing or duplicated')
        if 'PianoColdBootObjects.c'in sources or 'PianoEarlyMemory.c'in sources:raise ValueError('SEC collector cannot link into DXE')
    return True
