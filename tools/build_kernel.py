#!/usr/bin/env python3
"""Build a pinned independent kernel profile and record its actual inputs.

No adb/fastboot or device writes. Source branches are never switched by this
tool: it makes a detached build worktree at the profile's canonical commit.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(argv,*,cwd=None,env=None):
    subprocess.run([str(a) for a in argv],cwd=cwd,env=env,check=True)
def output(argv,cwd=None):return subprocess.check_output(argv,cwd=cwd,text=True).strip()

def image_info(path):
    data=Path(path).read_bytes()
    if len(data)<64 or data[56:60]!=b'ARM\x64':raise ValueError('Not an ARM64 Linux Image')
    info={'bytes':len(data),'sha256':digest(path),'arm64_magic':True,'efi_stub':False}
    if data[:2]==b'MZ':
        off=struct.unpack_from('<I',data,60)[0]
        if off<64 or off+24>len(data) or data[off:off+4]!=b'PE\0\0' or struct.unpack_from('<H',data,off+4)[0]!=0xaa64:
            raise ValueError('Invalid ARM64 EFI stub header')
        sections=struct.unpack_from('<H',data,off+6)[0]
        optional_bytes=struct.unpack_from('<H',data,off+20)[0]
        optional=off+24
        table=optional+optional_bytes
        if not 1<=sections<=96 or optional_bytes<112 or table+sections*40>len(data):
            raise ValueError('Truncated EFI stub optional/section headers')
        if struct.unpack_from('<H',data,optional)[0]!=0x20b:
            raise ValueError('EFI stub is not PE32+')
        directory_count=struct.unpack_from('<I',data,optional+108)[0]
        if directory_count>16 or 112+directory_count*8>optional_bytes:
            raise ValueError('Truncated EFI stub data directories')
        for index in range(sections):
            raw_bytes,raw_offset=struct.unpack_from('<II',data,table+index*40+16)
            if raw_bytes and (raw_offset<table+sections*40 or raw_offset+raw_bytes>len(data)):
                raise ValueError('EFI stub section exceeds Image')
        info['efi_stub']=True
    return info

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile',choices=('stable','next'),required=True)
    ap.add_argument('--mode',choices=('ram','userspace-debug'),default='ram')
    ap.add_argument('--repository',type=Path)
    ap.add_argument('--jobs',type=int,default=min(os.cpu_count() or 1,16))
    ap.add_argument('--configure-only',action='store_true')
    ap.add_argument('--modules',action='store_true')
    ap.add_argument('--dtb',type=Path,help='Explicit final board DTB; no implicit phone/MTP substitution')
    args=ap.parse_args();root=Path(__file__).resolve().parent.parent
    lock=json.loads((root/'linux/kernel-profiles.json').read_text());profile=lock['profiles'][args.profile]
    repo=(args.repository or root/lock['repository_path']).resolve()
    commit=output(['git','rev-parse',profile['commit']+'^{commit}'],repo)
    if commit!=profile['commit']:raise SystemExit('Pinned source commit mismatch')
    work=root/'build/kernel-worktrees'/f'{args.profile}-{commit[:12]}'
    legacy=root/'build/kernel-worktrees'/args.profile
    if legacy.exists() and output(['git','rev-parse','HEAD'],legacy)==commit:work=legacy
    work.parent.mkdir(parents=True,exist_ok=True)
    if work.exists():
        if output(['git','rev-parse','HEAD'],work)!=commit:raise SystemExit('Build worktree has another commit; preserve it and choose a new profile pin')
        if output(['git','status','--porcelain'],work):raise SystemExit('Build worktree is dirty; refusing to hide source changes')
    else:run(['git','worktree','add','--detach',work,commit],cwd=repo)
    out=root/'build/kernels'/args.profile/args.mode;out.mkdir(parents=True,exist_ok=True)
    artifacts=root/'artifacts/kernels'/args.profile/args.mode;artifacts.mkdir(parents=True,exist_ok=True)
    (artifacts/'manifest.json').unlink(missing_ok=True)
    env=os.environ.copy();bundled=root/'build/host-tools/usr'
    if (bundled/'bin/clang').exists():
        env['PATH']=str(bundled/'bin')+os.pathsep+env['PATH']
        env['LD_LIBRARY_PATH']=str(bundled/'lib')+(os.pathsep+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    for name in ('clang','ld.lld','llvm-ar','llvm-nm','llvm-objcopy','make','bison','flex','bc'):
        if not shutil.which(name,path=env['PATH']):raise SystemExit('Missing host build dependency: '+name)
    fragments=[root/'linux/configs/piano-ram.config']
    if args.mode=='userspace-debug':fragments.append(root/'linux/configs/piano-userspace-debug.config')
    for fragment in fragments:
        if not fragment.exists():raise SystemExit('Missing profile fragment: '+str(fragment))
    input_hashes={str(f.relative_to(root)):digest(f) for f in fragments}
    command=['make','-C',work,f'O={out}','ARCH=arm64','LLVM=1','LLVM_IAS=1']
    run(command+[profile['base_config']],env=env)
    run(['bash',work/'scripts/kconfig/merge_config.sh','-m','-O',out,out/'.config',*fragments],cwd=work,env=env)
    run(command+['olddefconfig'],env=env)
    config=(out/'.config').read_text()
    if 'CONFIG_CMDLINE_FORCE=y' in config:raise SystemExit('Forced downstream command line survived merge')
    if 'CONFIG_EFI=y' not in config:raise SystemExit('EFI support not enabled')
    if args.mode=='ram' and any(x in config.splitlines() for x in ('CONFIG_BLOCK=y','CONFIG_SCSI=y','CONFIG_SCSI_UFSHCD=y')):
        raise SystemExit('Persistent storage survived first RAM smoke config merge')
    state={'profile':args.profile,'mode':args.mode,'source_commit':commit,'branch':profile['branch'],
        'base_commit':profile['base_commit'],'canonical_patch_commits':output(['git','rev-list','--reverse',f'{profile["base_commit"]}..{commit}'],repo).splitlines(),
        'source_dirty':False,'hardware_verified':False,'config_sha256':digest(out/'.config'),
        'fragment_sha256':digest(fragments[0]),'fragments':input_hashes,
        'compiler':output([shutil.which('clang',path=env['PATH']),'--version']).splitlines()[0]}
    shutil.copyfile(out/'.config',artifacts/'config')
    if any(digest(root/name)!=value for name,value in input_hashes.items()):
        raise SystemExit('Profile input changed during configuration; refusing stale artifact manifest')
    if args.configure_only:
        state['status']='CONFIGURED_NOT_BUILT';(artifacts/'manifest.json').write_text(json.dumps(state,indent=2)+'\n');print(json.dumps(state,indent=2));return
    run(command+[f'-j{args.jobs}','Image']+(['modules'] if args.modules else []),env=env)
    if any(digest(root/name)!=value for name,value in input_hashes.items()):
        raise SystemExit('Profile input changed during the build; refusing stale artifact manifest')
    image=out/'arch/arm64/boot/Image';state['image']=image_info(image)
    if not state['image']['efi_stub']:raise SystemExit('Built Image has no EFI stub')
    shutil.copyfile(image,artifacts/'Image');shutil.copyfile(out/'System.map',artifacts/'System.map')
    state['kernel_release']=(out/'include/config/kernel.release').read_text().strip()
    if args.dtb:
        dtb=args.dtb.resolve();data=dtb.read_bytes()
        if len(data)<40 or data[:4]!=b'\xd0\x0d\xfe\xed' or struct.unpack_from('>I',data,4)[0]!=len(data):
            raise SystemExit('Explicit DTB is not a complete FDT')
        shutil.copyfile(dtb,artifacts/'piano.dtb');state['dtb']={'source':str(dtb),'sha256':digest(dtb),'validation':'header only; board topology still requires audit'}
    else:state['dtb']=None
    if args.modules:
        install=artifacts/'modules';run(command+[f'INSTALL_MOD_PATH={install}','modules_install'],env=env)
        state['modules']=[{'path':str(p.relative_to(artifacts)),'sha256':digest(p)} for p in sorted(install.rglob('*.ko*'))]
    state['status']='HOST_BUILT_NOT_HARDWARE_VERIFIED'
    (artifacts/'manifest.json').write_text(json.dumps(state,indent=2)+'\n');print(json.dumps(state,indent=2))

if __name__=='__main__':main()
