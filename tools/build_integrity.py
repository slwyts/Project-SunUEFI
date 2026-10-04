#!/usr/bin/env python3
"""Host-only build freshness checks. Never invokes adb or fastboot."""
import argparse
import hashlib
import json
from pathlib import Path
import uuid

NAMES={'stage0':'piano','probe':'pianoProbe','linux':'pianoLinux','gui':'pianoGui'}


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
    record['outputs']={name:sha(folder/name) for name in ('piano-stage0.fd','BootShim.bin')}
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
