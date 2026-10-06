#!/usr/bin/env python3
"""Host-only build freshness checks. Never invokes adb or fastboot."""
import argparse
import hashlib
import json
from pathlib import Path
import uuid

NAMES={'stage0':'piano','probe':'pianoProbe','linux':'pianoLinux','gui':'pianoGui','product':'pianoProduct'}

def output_names(profile):
    return ('PianoUEFI-product.fd','BootShim.bin') if profile=='product' else ('piano-stage0.fd','BootShim.bin')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(root,profile):
    ws=root/'upstream/Mu-Silicium'
    platform=ws/'Platforms/Xiaomi'/f'{NAMES[profile]}Pkg'
    if not platform.is_dir():
        raise ValueError('Prepared platform is missing')
    files=set(p for p in platform.rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    binaries=ws/'Binaries/piano'
    if binaries.exists():
        files.update(p for p in binaries.rglob('*') if p.is_file())
    shim=ws/'BootShim' if profile=='stage0' else root/'bootprofiles/handoff'
    for p in (shim/'BootShim.S',shim/'Makefile',ws/'Resources/DTBs/piano.dtb',
              root/'tools/build_stage0.sh',root/'tools/build_integrity.py'):
        if p.is_file():files.add(p)
    if profile=='product':
        from prepare_product_pump import prepare as pump
        from prepare_product_ui import prepare as ui
        from prepare_nv_runtime_guard import prepare as nv_guard
        pump_record=pump(root,apply=False);ui_record=ui(root,apply=False)
        files.update(root/path for path in (*pump_record['files'],*ui_record['files']))
        files.update(root/path for path in nv_guard(root,apply=False)['files'])
        from prepare_product import SOURCE_NAMES, os_boot_files, verify_os_boot, observation_files, verify_observation_families,verify_display_mapping
        canonical=root/'bootprofiles/uefi-app'
        files.update(canonical/name for name in SOURCE_NAMES)
        files.update(path for path in canonical.iterdir() if path.is_file() and path.suffix in ('.h','.inc'))
        files.update(os_boot_files(root).values())
        prepared=json.loads((root/'build/product/prepared-manifest.json').read_text());os_boot=prepared.get('os_boot',{})
        for rows in observation_files(root).values():files.update(rows.values())
        for app in (platform/'Applications/ProductCore',root/'platforms/pianoProductPkg/Applications/ProductCore'):
            verify_os_boot(root,app,os_boot)
            verify_observation_families(root,app,prepared.get('dxe_observation',{}))
        from prepare_product_early_memory import verify as verify_early_memory, EARLY_FILES
        for target in (platform,root/'platforms/pianoProductPkg'):
            verify_early_memory(root,target,prepared.get('early_memory',{}))
            verify_display_mapping(root,target,prepared.get('display_mapping',{}))
        from piano_display_mapping import source_files as display_sources
        files.update(display_sources(root))
        files.update(root/path for path in ('tools/compose_piano_dtb.py','tools/analyze_capture.py'))
        files.update(root/'bootprofiles/early-memory'/name for name in EARLY_FILES)
        from prepare_product_handoff import prepare as handoff, verify_provider
        handoff_record=handoff(root,apply=False)
        if prepared.get('native_late_handoff',{}).get('files')!=handoff_record['files']:
            raise ValueError('Native late handoff preparation differs from canonical sources')
        for name,digest in handoff_record['files'].items():
            path=root/name
            if not path.is_file()or sha(path)!=digest:raise ValueError('Native late handoff compiled hook stale: '+name)
            files.add(path)
        for target in (platform,root/'platforms/pianoProductPkg'):
            verify_provider(root,target/'Applications/ProductCore',prepared.get('late_provider',{}))
        for folder in ('bootprofiles/product-pump','bootprofiles/product-support','bootprofiles/product-handoff'):
            files.update(path for path in (root/folder).rglob('*') if path.is_file())
        for relative in ('config/piano-product.json','build/product/prepared-manifest.json',
                         'tools/prepare_product.py','tools/build_product.sh','tools/package_product.py',
                         'tools/prepare_product_pump.py','tools/prepare_product_ui.py','tools/prepare_nv_runtime_guard.py','tools/prepare_product_early_memory.py','tools/prepare_product_handoff.py','tools/simpleinit_build_identity.py',
                         'tools/build_simpleinit.sh','tools/prepare_simpleinit.py','tools/product_payload_digest.py',
                         'artifacts/simpleinit/product/SimpleInit.efi','artifacts/simpleinit/product/app-payload.bin',
                         'artifacts/simpleinit/product/build-ok.json'):
            p=root/relative
            if not p.is_file():raise ValueError('Missing product build input: '+relative)
            files.add(p)
    hashes={str(p.relative_to(root)):sha(p) for p in sorted(files)}
    return {'sha256':hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest(),
            'file_count':len(hashes)}


def marker(root,profile):
    return root/'artifacts'/profile/'build-ok.json'


def pending(root,profile):
    return root/'build/integrity'/f'{profile}-pending.json'


def start(root,profile):
    marker(root,profile).unlink(missing_ok=True)
    p=pending(root,profile);p.parent.mkdir(parents=True,exist_ok=True)
    record={'profile':profile,'build_id':str(uuid.uuid4()),'inputs':inputs(root,profile)}
    p.write_text(json.dumps(record,indent=2)+'\n')
    return record


def finish(root,profile):
    p=pending(root,profile)
    record=json.loads(p.read_text())
    if record['profile']!=profile or record['inputs']!=inputs(root,profile):
        raise ValueError('Build inputs changed during compilation; rebuild before packaging')
    folder=root/'artifacts'/profile
    record['outputs']={name:sha(folder/name) for name in output_names(profile)}
    record['status']='successful build, matching inputs and outputs'
    marker(root,profile).write_text(json.dumps(record,indent=2)+'\n')
    p.unlink()
    return record


def validate(root,profile,manifest=None):
    p=marker(root,profile)
    if not p.is_file():
        raise ValueError('No completed build marker; old artifacts cannot be packaged or booted')
    record=json.loads(p.read_text())
    if record['profile']!=profile or record['inputs']!=inputs(root,profile):
        raise ValueError('Prepared sources differ from the completed build; rebuild first')
    for name,digest in record['outputs'].items():
        if sha(root/'artifacts'/profile/name)!=digest:
            raise ValueError('Built output hash mismatch: '+name)
    if manifest is not None and manifest.get('build_id')!=record['build_id']:
        raise ValueError('Boot package belongs to an older build; repackage the completed build')
    return record


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation',choices=('start','finish','validate'))
    parser.add_argument('--profile',choices=tuple(NAMES),required=True)
    args=parser.parse_args();root=Path(__file__).resolve().parent.parent
    try:
        record={'start':start,'finish':finish,'validate':validate}[args.operation](root,args.profile)
    except (ValueError,OSError,KeyError) as error:
        raise SystemExit(str(error))
    print(f'{args.operation}: {args.profile} build {record["build_id"]}')


if __name__=='__main__':main()
