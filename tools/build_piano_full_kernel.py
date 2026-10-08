#!/usr/bin/env python3
"""Build the exact public complete Piano candidate, isolated O and RAM-root policy.

No git checkout, rescue-pin change, guest execution, device or partition operation.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
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
 'TMPFS_POSIX_ACL':'y','TMPFS_XATTR':'y','SECURITY':'y',
}
FLASH_OVERRIDES={
 'CONFIG_LEDS_CLASS_FLASH':'m','CONFIG_LEDS_QCOM_FLASH':'m',
 'CONFIG_V4L2_FLASH_LED_CLASS':'m','CONFIG_VIDEO_V4L2_SUBDEV_API':'y',
}
EARLY_CPUCP_OVERRIDES={'CONFIG_QCOM_CPUCP_MBOX':'y'}
ALLOWED_OVERRIDES={'CONFIG_UHID':'m',**FLASH_OVERRIDES,**EARLY_CPUCP_OVERRIDES}


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
    if root_policy!='ram'and not re.fullmatch(r'(UUID=[0-9a-fA-F-]{8,64}|PARTUUID=[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}|LABEL=PIANO[A-Za-z0-9_-]{0,48})',root_policy):
        raise ValueError('External root must be an explicit UUID or PIANO-owned label; Android partition selectors are refused')
    return provided.replace('piano.root=ram','piano.root='+root_policy)


def module_overrides(fragment,kind='bluetooth'):
    lines=[line.strip()for line in fragment.splitlines()if line.strip()and not line.lstrip().startswith('#')]
    expected={'CONFIG_UHID':'m'}if kind=='bluetooth'else FLASH_OVERRIDES if kind=='flash'else EARLY_CPUCP_OVERRIDES if kind=='early-cpucp'else None
    if expected is None or lines!=[key+'='+value for key,value in expected.items()]:raise ValueError('Unexpected '+kind+' configuration fragment')
    return dict(expected)


def validate_module_overrides(values):
    if any(key not in ALLOWED_OVERRIDES or value!=ALLOWED_OVERRIDES[key]for key,value in values.items()):raise ValueError('Only reviewed Bluetooth/flash/early-CPUCP configuration overrides are supported')
    flash=set(values)&set(FLASH_OVERRIDES)
    if flash and flash!=set(FLASH_OVERRIDES):raise ValueError('Flash requires all four reviewed options')
    return values


def effective_config(command,modules):
    validate_module_overrides(modules)
    return 'CONFIG_CMDLINE='+json.dumps(command)+'\n'+''.join(key+'='+value+'\n'for key,value in sorted(modules.items()))


def verify_source(work=WORK,commit=None):
    commit=COMMIT if commit is None else commit
    if not re.fullmatch(r'[0-9a-f]{40}',commit):raise ValueError('Exact canonical full kernel commit required')
    if text(['git','rev-parse','HEAD'],work)!=commit:raise ValueError('Full candidate source HEAD drifted')
    if commit!=COMMIT and subprocess.run(['git','merge-base','--is-ancestor',COMMIT,commit],cwd=work,capture_output=True).returncode:
        raise ValueError('Local full candidate must descend from the frozen public full baseline')
    if text(['git','status','--porcelain=v1','--untracked-files=all'],work):raise ValueError('Full candidate source is dirty')
    for name,pin in SOURCE_PINS.items():
        if sha(work/name)!=pin:raise ValueError('Pinned public full config changed: '+name)


def validate_config(config,public,expected_command,modules=None):
    modules=validate_module_overrides({}if modules is None else modules)
    values=config_values(config)
    if modules.get('CONFIG_QCOM_CPUCP_MBOX')=='y':
        for key in ('CONFIG_ARM_SCMI_PROTOCOL','CONFIG_ARM_SCMI_TRANSPORT_MAILBOX','CONFIG_ARM_SCMI_CPUFREQ'):
            if values.get(key)!='y':raise ValueError('Early CPUCP requires built-in SCMI/cpufreq: '+key)
    if values.get('CONFIG_CMDLINE')!=json.dumps(expected_command):raise ValueError('Configured full candidate root command line changed')
    if 'userdata'in expected_command or 'root=PARTLABEL'in expected_command:raise ValueError('Android root target survived')
    for key,value in config_values(public).items():
        if key!='CONFIG_CMDLINE'and values.get(key)!=modules.get(key,value):raise ValueError('Public full profile option lost: '+key)
    for key,value in modules.items():
        if values.get(key)!=value:raise ValueError('Local module requirement lost: '+key)
    for key,value in REQUIRED.items():
        if values.get('CONFIG_'+key)!=value:raise ValueError('Full driver/userspace requirement lost: CONFIG_'+key)
    return {**{key:values['CONFIG_'+key]for key in REQUIRED},**{key.removeprefix('CONFIG_'):value for key,value in modules.items()}}


def toolchain():
    env=os.environ.copy();bundled=ROOT/'build/host-tools/usr'
    env['PATH']=str(bundled/'bin')+os.pathsep+env.get('PATH','')
    env['LD_LIBRARY_PATH']=str(bundled/'lib')+(os.pathsep+env['LD_LIBRARY_PATH']if env.get('LD_LIBRARY_PATH')else '')
    names=('clang','ld.lld','llvm-ar','llvm-nm','llvm-objcopy','llvm-strip','make','bison','flex','bc','depmod')
    cached=bool(env.get('CCACHE_DIR'))
    if cached:names+=('ccache',)
    binaries={name:shutil.which(name,path=env['PATH'])for name in names}
    if any(path is None for path in binaries.values()):raise ValueError('Missing full build dependencies: '+str([name for name,path in binaries.items()if path is None]))
    cc=[binaries['ccache'],binaries['clang']]if cached else[binaries['clang']]
    return env,{'paths':binaries,'sha256':{name:sha(Path(path))for name,path in binaries.items()},'compiler':text([binaries['clang'],'--version'],env=env).splitlines()[0],
               'ccache_enabled':cached,'ccache_dir':env.get('CCACHE_DIR')if cached else None,'kernel_cc':cc}


def seal_modules(folder,release,required_builtin=()):
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
    # Built-in providers have no installed .ko or modules.dep entry. Require
    # their normal Kbuild metadata instead of treating them as missing modules.
    if required_builtin:
        index=library/'modules.builtin'
        if not index.is_file():raise ValueError('Installed built-in module index missing')
        builtin={Path(path).name.removesuffix('.ko').replace('-','_')for path in index.read_text().splitlines()}
        for name in required_builtin:
            if name not in builtin or name in names:raise ValueError('Required built-in provider differs from installed metadata: '+name)
    return modules,{'count':len(modules),'bytes':total,'install_mod_strip':1,'all_vermagic_checked':True,'index_sha256':metadata,'required_builtin':list(required_builtin)}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--jobs',type=int,default=4);parser.add_argument('--root',default='ram');parser.add_argument('--configure-only',action='store_true')
    parser.add_argument('--worktree',type=Path,default=WORK)
    parser.add_argument('--commit',default=COMMIT)
    parser.add_argument('--build-dir',type=Path,default=OUT)
    parser.add_argument('--artifacts',type=Path,default=ARTIFACTS)
    parser.add_argument('--rebuild',action='store_true',help='Replace generated candidate artifacts with an incremental build')
    args=parser.parse_args()
    if not 1<=args.jobs<=8:raise SystemExit('Full candidate jobs must be 1..8')
    work,out,artifacts=(path.resolve()for path in (args.worktree,args.build_dir,args.artifacts));commit=args.commit
    if not work.is_relative_to(ROOT/'build/kernel-worktrees')or not out.is_relative_to(ROOT/'build/kernels')or not artifacts.is_relative_to(ROOT/'artifacts/kernels'):
        raise ValueError('Explicit full kernel paths must stay in the workspace build/artifact directories')
    verify_source(work,commit);public=work/'arch/arm64/configs/piano_rootfs.config';fragment=ROOT/'linux/configs/piano-full.config'
    root_command=command_line(fragment.read_text(),public.read_text(),args.root)
    bluetooth=ROOT/'linux/configs/piano-bluetooth.config';flash=ROOT/'linux/configs/piano-flash.config'
    early_cpucp=ROOT/'linux/configs/piano-cpucp-early.config'
    modules={**module_overrides(bluetooth.read_text()),**module_overrides(flash.read_text(),'flash'),
             **module_overrides(early_cpucp.read_text(),'early-cpucp')}
    env,tools=toolchain();out.mkdir(parents=True,exist_ok=True);artifacts.mkdir(parents=True,exist_ok=True)
    build_locks=[]
    lock_root=ROOT/'build/locks';lock_root.mkdir(parents=True,exist_ok=True)
    for directory in sorted({out,artifacts}):
        name=hashlib.sha256(str(directory).encode()).hexdigest()+'.lock'
        handle=(lock_root/name).open('a');build_locks.append(handle)
        try:fcntl.flock(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:raise ValueError('Kernel build/output is in use: '+str(directory))
    marker=artifacts/'manifest.json'
    if marker.exists():
        if not args.rebuild:raise ValueError('Candidate exists; use --rebuild to replace generated artifacts')
        # Stale Image/modules may remain during the build, but no consumer can
        # mistake them for a completed new bundle without its final manifest.
        marker.unlink()
    hashes={str(path.relative_to(ROOT)):sha(path)for path in (fragment,bluetooth,flash,early_cpucp,Path(__file__),ROOT/'tools/build_kernel.py',ROOT/'tools/prepare_linux_modules.py')}
    effective_text=effective_config(root_command,modules)
    effective=out/'piano-full.effective.config';effective.write_text(effective_text)
    command=['make','-C',work,'O='+str(out),'ARCH=arm64','LLVM=1','LLVM_IAS=1']
    if tools['ccache_enabled']:
        # Kbuild recommends a stable timestamp for useful ccache reuse.
        env.setdefault('KBUILD_BUILD_TIMESTAMP',text(['git','show','-s','--format=%cI',commit],work))
        tools['kbuild_build_timestamp']=env['KBUILD_BUILD_TIMESTAMP']
        command.append('CC='+shlex.join(tools['kernel_cc']))
    run(command+['piano_defconfig'],env=env)
    run(['bash',work/'scripts/kconfig/merge_config.sh','-m','-O',out,out/'.config',public,effective],cwd=work,env=env)
    run(command+['olddefconfig'],env=env)
    requirements=validate_config((out/'.config').read_text(),public.read_text(),root_command,modules);config_hash=sha(out/'.config')
    def fresh():
        verify_source(work,commit)
        if any(sha(ROOT/name)!=value for name,value in hashes.items())or sha(out/'.config')!=config_hash or effective.read_text()!=effective_text:raise ValueError('Full candidate source/config/tool inputs drifted')
        if any(sha(Path(tools['paths'][name]))!=value for name,value in tools['sha256'].items()):raise ValueError('Full candidate build tool changed during compilation')
    fresh();state={'profile':'full-integration','mode':'complete-public-hardware','build_id':str(uuid.uuid4()),'source_commit':commit,'source_worktree':str(work),'source_clean':True,
      'source_branch':text(['git','branch','--show-current'],work),'base_commit':BASE_COMMIT,
      'public_full_baseline':COMMIT,'local_patch_commits':text(['git','rev-list','--reverse',COMMIT+'..'+commit],work).splitlines(),
      'public_patch_commits':text(['git','rev-list','--reverse',BASE_COMMIT+'..'+COMMIT],work).splitlines(),
      'public_config_sha256':SOURCE_PINS,'inputs':hashes,'config_sha256':config_hash,'toolchain':tools,'root_policy':args.root,'command_line':root_command,
      'full_profile_requirements':requirements,'local_module_overrides':modules,'hardware_verified':False,'device_operation_performed':False,'android_userdata_selected':False,
      'safe_pianoinit_external_bundle_required':True,'public_bt_le_enabled':config_values((out/'.config').read_text()).get('CONFIG_BT_LE')=='y',
      'dtb':None,'status':'CONFIGURED_NOT_BUILT'if args.configure_only else 'BUILDING_NOT_BOOTABLE'}
    shutil.copyfile(out/'.config',artifacts/'config');pending=out/'full-build-pending.json';pending.write_text(json.dumps(state,indent=2)+'\n')
    if args.configure_only:print(json.dumps({'status':state['status'],'config_sha256':config_hash,'requirements':len(requirements)}));return
    run(command+[f'-j{args.jobs}','Image','modules'],env=env);fresh()
    image=out/'arch/arm64/boot/Image';state['image']=image_info(image)
    if not state['image']['efi_stub']:raise ValueError('Complete public candidate has no EFI stub')
    state['kernel_release']=(out/'include/config/kernel.release').read_text().strip()
    install=artifacts/'modules'
    if install.exists():shutil.rmtree(install)
    run(command+[f'INSTALL_MOD_PATH={install}','INSTALL_MOD_STRIP=1','modules_install'],env=env);fresh()
    state['modules'],state['module_summary']=seal_modules(install,state['kernel_release'],required_builtin=('qcom_cpucp_mbox',))
    for name,source in (('Image',image),('System.map',out/'System.map')):shutil.copyfile(source,artifacts/name)
    fresh();state['status']='HOST_BUILT_FULL_CANDIDATE_NOT_HARDWARE_VERIFIED'
    temporary=marker.with_suffix('.json.tmp');temporary.write_text(json.dumps(state,indent=2)+'\n');os.replace(temporary,marker);pending.unlink()
    print(json.dumps({'status':state['status'],'build_id':state['build_id'],'image':state['image'],'kernel_release':state['kernel_release'],'module_summary':state['module_summary']},indent=2))


if __name__=='__main__':main()
