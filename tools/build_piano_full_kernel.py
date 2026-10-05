#!/usr/bin/env python3
"""Build the exact public complete Piano candidate, isolated O and RAM-root policy.

No git checkout, rescue-pin change, guest execution, device or partition operation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from build_kernel import image_info
from prepare_linux_modules import modinfo

ROOT=Path(__file__).resolve().parents[1]
COMMIT='352508459733d3e6d349ea5581a8dd2fd8bb4180'
BASE_COMMIT='500df175a7f9e6bc1a9c328590ca5150f84f9ff0'
WORK=ROOT/'build/kernel-worktrees/piano-full-integration'
OUT=ROOT/'build/kernels/full-integration'
ARTIFACTS=ROOT/'artifacts/kernels/full-integration'
SOURCE_PINS={'arch/arm64/configs/piano_defconfig':'a54052e115f8dfa9044075f741d119a2765630df047b621e62faafc686f89312',
             'arch/arm64/configs/piano_rootfs.config':'db201a064a89e53c45b5d018f56497d9633a699b205bef89a6db26f5c96ac009'}
REQUIRED={
 'MODULES':'y','BLK_DEV_INITRD':'y','EFI':'y','EFI_STUB':'y','CMDLINE_FORCE':'y',
 'DRM_MSM':'m','ARM_SMMU':'m','QCOM_Q6V5_PAS':'m','DRM_SIMPLEDRM':'y','DRM_PANEL_NOVATEK_NT36532':'m',
 'VIDEO_QCOM_CAMSS':'m','VIDEO_OV32D40':'m','VIDEO_S5KJN1':'m','VIDEO_DW9768':'m','VIDEO_QCOM_IRIS':'m',
 'SND_SOC_SC8280XP':'m','SND_SOC_QDSP6':'m','SND_SOC_FS19XX':'m','QCOM_FASTRPC':'m',
 'BATTERY_PIANO_MCA':'m','CHARGER_SC8541':'m','QCOM_PMIC_GLINK':'m',
 'TOUCHSCREEN_NT36532E_SPI':'m','INPUT_EVDEV':'y','INPUT_UINPUT':'m','HID_NANOSIC_WN8030':'m',
 'ATH12K':'m','BT':'m','BT_HCIUART':'m','BT_HCIUART_QCA':'y',
 'EXT4_FS':'y','SCSI_UFSHCD':'m','SCSI_UFS_QCOM':'m','PHY_QCOM_QMP_UFS':'m','SCSI_UFS_CRYPTO':'n',
 'USB_CONFIGFS_NCM':'y','DEVTMPFS':'y','DEVTMPFS_MOUNT':'y','UNIX':'y','TMPFS':'y',
 'CGROUPS':'y','INOTIFY_USER':'y','FHANDLE':'y','SECCOMP':'y','SECCOMP_FILTER':'y','ZRAM':'y','BINFMT_MISC':'y',
}


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def text(argv,cwd=None,env=None):return subprocess.check_output([str(x)for x in argv],cwd=cwd,env=env,text=True).strip()
def run(argv,cwd=None,env=None):subprocess.run([str(x)for x in argv],cwd=cwd,env=env,check=True)


def config_values(raw):
    values={}
    for line in raw.splitlines():
        match=re.fullmatch(r'(CONFIG_[A-Z0-9_]+)=(.*)',line)
        disabled=re.fullmatch(r'# (CONFIG_[A-Z0-9_]+) is not set',line)
        if match:values[match[1]]=match[2]
        elif disabled:values[disabled[1]]='n'
    return values


def command_line(fragment,public,root_policy):
    override=config_values(fragment)
    if set(override)!={'CONFIG_CMDLINE'}:raise ValueError('Local full fragment may override only CONFIG_CMDLINE')
    original=json.loads(config_values(public)['CONFIG_CMDLINE']);provided=json.loads(override['CONFIG_CMDLINE'])
    tokens=original.split()
    if tokens.count('root=PARTLABEL=userdata')!=1:raise ValueError('Unexpected public root policy')
    expected=' '.join('piano.root=ram'if token=='root=PARTLABEL=userdata'else token for token in tokens)
    if provided!=expected:raise ValueError('Full candidate must preserve every public handoff argument except userdata root')
    if root_policy!='ram'and not re.fullmatch(r'(UUID=[0-9a-fA-F-]{8,64}|LABEL=PIANO[A-Za-z0-9_-]{0,48})',root_policy):
        raise ValueError('External root must be an explicit UUID or PIANO-owned label; Android partition selectors are refused')
    return provided.replace('piano.root=ram','piano.root='+root_policy)


def verify_source(work=WORK):
    if text(['git','rev-parse','HEAD'],work)!=COMMIT:raise ValueError('Full candidate source HEAD drifted')
    if text(['git','status','--porcelain=v1','--untracked-files=all'],work):raise ValueError('Full candidate source is dirty')
    for name,pin in SOURCE_PINS.items():
        if sha(work/name)!=pin:raise ValueError('Pinned public full config changed: '+name)


def validate_config(config,public,expected_command):
    values=config_values(config)
    if values.get('CONFIG_CMDLINE')!=json.dumps(expected_command):raise ValueError('Configured full candidate root command line changed')
    if 'userdata'in expected_command or 'root=PARTLABEL'in expected_command:raise ValueError('Android root target survived')
    for key,value in config_values(public).items():
        if key!='CONFIG_CMDLINE'and values.get(key)!=value:raise ValueError('Public full profile option lost: '+key)
    for key,value in REQUIRED.items():
        if values.get('CONFIG_'+key)!=value:raise ValueError('Full driver/userspace requirement lost: CONFIG_'+key)
    return {key:values['CONFIG_'+key]for key in REQUIRED}


def toolchain():
    env=os.environ.copy();bundled=ROOT/'build/host-tools/usr'
    env['PATH']=str(bundled/'bin')+os.pathsep+env.get('PATH','')
    env['LD_LIBRARY_PATH']=str(bundled/'lib')+(os.pathsep+env['LD_LIBRARY_PATH']if env.get('LD_LIBRARY_PATH')else '')
    names=('clang','ld.lld','llvm-ar','llvm-nm','llvm-objcopy','llvm-strip','make','bison','flex','bc','depmod')
    binaries={name:shutil.which(name,path=env['PATH'])for name in names}
    if any(path is None for path in binaries.values()):raise ValueError('Missing full build dependencies: '+str([name for name,path in binaries.items()if path is None]))
    return env,{'paths':binaries,'sha256':{name:sha(Path(path))for name,path in binaries.items()},'compiler':text([binaries['clang'],'--version'],env=env).splitlines()[0]}


def seal_modules(folder,release):
    modules=[];names=set();total=0
    for path in sorted(folder.rglob('*.ko')):
        info=modinfo(path);vermagic=info.get('vermagic','')
        if not vermagic or vermagic.split()[0]!=release:raise ValueError('Installed module release mismatch: '+str(path))
        name=info.get('name')
        if not name or name in names:raise ValueError('Missing or duplicate installed module identity')
        names.add(name);size=path.stat().st_size;total+=size
        modules.append({'path':path.relative_to(folder).as_posix(),'bytes':size,'sha256':sha(path),'name':name,'vermagic':vermagic,'depends':info.get('depends','')})
    if not modules or any(folder.rglob('*.ko.xz'))or any(folder.rglob('*.ko.zst'))or any(folder.rglob('*.ko.gz')):raise ValueError('Expected nonempty uncompressed stripped module set')
    library=folder/'lib/modules'/release
    metadata={path.relative_to(folder).as_posix():sha(path)for path in sorted(library.glob('modules.*'))if path.is_file()}
    if not(library/'modules.dep').is_file():raise ValueError('Installed module dependency index missing')
    return modules,{'count':len(modules),'bytes':total,'install_mod_strip':1,'all_vermagic_checked':True,'index_sha256':metadata}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--jobs',type=int,default=4);parser.add_argument('--root',default='ram');parser.add_argument('--configure-only',action='store_true')
    args=parser.parse_args()
    if not 1<=args.jobs<=8:raise SystemExit('Full candidate jobs must be 1..8')
    verify_source();public=WORK/'arch/arm64/configs/piano_rootfs.config';fragment=ROOT/'configs/linux/piano-full.config'
    root_command=command_line(fragment.read_text(),public.read_text(),args.root)
    env,tools=toolchain();OUT.mkdir(parents=True,exist_ok=True);ARTIFACTS.mkdir(parents=True,exist_ok=True)
    marker=ARTIFACTS/'manifest.json';marker.unlink(missing_ok=True)
    hashes={str(path.relative_to(ROOT)):sha(path)for path in (fragment,Path(__file__),ROOT/'tools/build_kernel.py',ROOT/'tools/prepare_linux_modules.py')}
    effective=OUT/'piano-full.effective.config';effective.write_text('CONFIG_CMDLINE='+json.dumps(root_command)+'\n')
    command=['make','-C',WORK,'O='+str(OUT),'ARCH=arm64','LLVM=1','LLVM_IAS=1']
    run(command+['piano_defconfig'],env=env)
    run(['bash',WORK/'scripts/kconfig/merge_config.sh','-m','-O',OUT,OUT/'.config',public,effective],cwd=WORK,env=env)
    run(command+['olddefconfig'],env=env)
    requirements=validate_config((OUT/'.config').read_text(),public.read_text(),root_command);config_hash=sha(OUT/'.config')
    def fresh():
        verify_source()
        if any(sha(ROOT/name)!=value for name,value in hashes.items())or sha(OUT/'.config')!=config_hash or effective.read_text()!='CONFIG_CMDLINE='+json.dumps(root_command)+'\n':raise ValueError('Full candidate source/config/tool inputs drifted')
        if any(sha(Path(tools['paths'][name]))!=value for name,value in tools['sha256'].items()):raise ValueError('Full candidate build tool changed during compilation')
    fresh();state={'profile':'full-integration','mode':'complete-public-hardware','build_id':str(uuid.uuid4()),'source_commit':COMMIT,'source_worktree':str(WORK),'source_clean':True,
      'source_branch':text(['git','branch','--show-current'],WORK),'base_commit':BASE_COMMIT,
      'public_patch_commits':text(['git','rev-list','--reverse',BASE_COMMIT+'..'+COMMIT],WORK).splitlines(),
      'public_config_sha256':SOURCE_PINS,'inputs':hashes,'config_sha256':config_hash,'toolchain':tools,'root_policy':args.root,'command_line':root_command,
      'full_profile_requirements':requirements,'hardware_verified':False,'device_operation_performed':False,'android_userdata_selected':False,
      'safe_pianoinit_external_bundle_required':True,'public_bt_le_enabled':config_values((OUT/'.config').read_text()).get('CONFIG_BT_LE')=='y',
      'dtb':None,'status':'CONFIGURED_NOT_BUILT'if args.configure_only else 'BUILDING_NOT_BOOTABLE'}
    shutil.copyfile(OUT/'.config',ARTIFACTS/'config');pending=OUT/'full-build-pending.json';pending.write_text(json.dumps(state,indent=2)+'\n')
    if args.configure_only:print(json.dumps({'status':state['status'],'config_sha256':config_hash,'requirements':len(requirements)}));return
    run(command+[f'-j{args.jobs}','Image','modules'],env=env);fresh()
    image=OUT/'arch/arm64/boot/Image';state['image']=image_info(image)
    if not state['image']['efi_stub']:raise ValueError('Complete public candidate has no EFI stub')
    state['kernel_release']=(OUT/'include/config/kernel.release').read_text().strip()
    install=ARTIFACTS/'modules'
    if install.exists():shutil.rmtree(install)
    run(command+[f'INSTALL_MOD_PATH={install}','INSTALL_MOD_STRIP=1','modules_install'],env=env);fresh()
    state['modules'],state['module_summary']=seal_modules(install,state['kernel_release'])
    for name,source in (('Image',image),('System.map',OUT/'System.map')):shutil.copyfile(source,ARTIFACTS/name)
    fresh();state['status']='HOST_BUILT_FULL_CANDIDATE_NOT_HARDWARE_VERIFIED';marker.write_text(json.dumps(state,indent=2)+'\n');pending.unlink()
    print(json.dumps({'status':state['status'],'build_id':state['build_id'],'image':state['image'],'kernel_release':state['kernel_release'],'module_summary':state['module_summary']},indent=2))


if __name__=='__main__':main()
