#!/usr/bin/env python3
"""Install a sealed full kernel's real modules into an explicit derived GNU root.

No device, module loading or host installation. Exact hashes and kernel ABI are
checked before staging. Old releases are preserved unless explicitly named for
removal inside this derived copy; original artifacts stay intact.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from build_piano_full_kernel import seal_modules

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024**2), b''):h.update(chunk)
    return h.hexdigest()


def safe_target(root, relative):
    path = root / relative
    for current in (path, *path.parents):
        if current == root:break
        if current.is_symlink():raise ValueError('Guest module destination traverses a symlink')
    return path


def inspect(kernel):
    kernel = Path(kernel).resolve()
    if not kernel.is_relative_to(ROOT/'artifacts/kernels'):
        raise ValueError('Expected sealed workspace kernel artifact')
    path = kernel/'manifest.json'; raw = path.read_bytes(); m=json.loads(raw)
    if m.get('status')!='HOST_BUILT_FULL_CANDIDATE_NOT_HARDWARE_VERIFIED':
        raise ValueError('Full Image/modules build is not complete')
    release=m['kernel_release']
    if not re.fullmatch(r'[A-Za-z0-9_.+-]{1,128}',release):raise ValueError('Invalid kernel release')
    config_sha=m.get('config_sha256')
    if config_sha is None:
        configs=[digest for name,digest in m.get('inputs',{}).items() if name.endswith('/.config')]
        if len(configs)!=1:raise ValueError('Exact kernel config provenance is missing')
        config_sha=configs[0]
    if sha(kernel/'Image')!=m['image']['sha256'] or sha(kernel/'config')!=config_sha:
        raise ValueError('Sealed kernel Image/config changed')
    expected_summary=m['module_summary']
    rows,summary=seal_modules(kernel/'modules',release,
                               required_builtin=expected_summary.get('required_builtin',()))
    # Older sealed bundles predate the optional built-in-provider receipt.
    # Their existing file/ABI/index checks remain identical.
    if 'required_builtin' not in expected_summary:
        summary.pop('required_builtin',None)
    if rows!=m['modules'] or summary!=m['module_summary']:
        raise ValueError('Sealed module ABI, file hash or dependency index changed')
    if path.read_bytes()!=raw:raise ValueError('Kernel manifest changed during verification')
    return m,hashlib.sha256(raw).hexdigest()


def stage(root,kernel,remove=()):
    root,kernel=Path(root).resolve(),Path(kernel).resolve()
    if not root.is_relative_to(ROOT/'build/distros') or not root.is_dir():
        raise ValueError('Target must be an existing derived workspace root')
    m,manifest_sha=inspect(kernel);release=m['kernel_release']
    modules=safe_target(root,Path('usr/lib/modules'));target=modules/release
    if target.exists() or target.is_symlink():raise ValueError('Kernel release already exists; preserve staged bytes')
    removed=[]
    for old in remove:
        if not re.fullmatch(r'[A-Za-z0-9_.+-]{1,128}',old) or old==release:
            raise ValueError('Invalid explicit old release')
        path=safe_target(root,Path('usr/lib/modules')/old)
        if not path.is_dir():raise ValueError('Named old release is missing')
        removed.append(path)
    destination=safe_target(root,Path('usr/share/piano-provenance/kernel-modules.json'))
    per_release=safe_target(root,Path('usr/share/piano-provenance/kernel-modules')/(release+'.json'))
    modules.mkdir(parents=True,exist_ok=True)
    shutil.copytree(kernel/'modules/lib/modules'/release,target,symlinks=True)
    # Development links point to host source/build trees, never runtime data.
    for name in ('build','source'):
        link=target/name
        if link.is_symlink():link.unlink()
        elif link.exists():raise ValueError('Unexpected real development directory in module artifact')
    for row in m['modules']:
        relative=Path(row['path']).relative_to('lib/modules/'+release)
        if sha(target/relative)!=row['sha256']:raise ValueError('Staged module hash differs: '+str(relative))
    for name,digest in m['module_summary']['index_sha256'].items():
        relative=Path(name).relative_to('lib/modules/'+release)
        if sha(target/relative)!=digest:raise ValueError('Staged module index differs')
    if sha(kernel/'manifest.json')!=manifest_sha:raise ValueError('Kernel input changed during staging')
    for path in removed:shutil.rmtree(path)
    result={'status':'EXACT_FULL_KERNEL_MODULES_STAGED_NOT_BOOT_VERIFIED','kernel_commit':m['source_commit'],
            'kernel_release':release,'kernel_image_sha256':m['image']['sha256'],'kernel_manifest_sha256':manifest_sha,
            'modules':m['module_summary'],'removed_releases':[path.name for path in removed],
            'tool_sha256':sha(__file__),'device_operation_performed':False,'piano_boot_verified':False}
    per_release.parent.mkdir(parents=True,exist_ok=True);per_release.write_text(json.dumps(result,indent=2)+'\n')
    records={}
    for path in sorted(per_release.parent.glob('*.json')):
        value=json.loads(path.read_text());records[value['kernel_release']]=value
    destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_text(json.dumps({'schema':1,'status':'SEALED_KERNEL_RELEASES_STAGED_NOT_BOOT_VERIFIED',
        'installed_releases':records,'piano_boot_verified':False},indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs',type=Path,required=True);parser.add_argument('--kernel',type=Path,required=True)
    parser.add_argument('--remove-release',action='append',default=[])
    parser.add_argument('--mapped-userns',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args()
    if not args.mapped_userns:
        os.execvp('unshare',['unshare','--user','--map-root-user','--map-auto','--fork',sys.executable,
            str(Path(__file__).resolve()),*sys.argv[1:],'--mapped-userns'])
    if os.getuid() or args.rootfs.stat().st_uid:
        raise ValueError('Guest ownership must be mapped to private namespace root')
    print(json.dumps(stage(args.rootfs,args.kernel,args.remove_release),indent=2))
