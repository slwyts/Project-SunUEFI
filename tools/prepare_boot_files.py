#!/usr/bin/env python3
"""Prepare a host-only Piano EFI file tree from checked kernel manifests.

No device access, partition changes, implicit DTB selection, userdata root or
kernel build. Unverified kernels require --allow-unverified and cannot become
the stable rescue default. --plan-only emits inactive entries without images.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import tempfile
from urllib.parse import urlsplit

ROOT=Path(__file__).resolve().parent.parent
TEMPLATE=ROOT/'uefi/components/piano-boot/simpleinit.static.uefi.json'
LOAD_LIMIT=0x8000000  # Fixed SimpleInit linux-boot/loader.c rejects >=128 MiB.

def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def require(condition,message):
    if not condition:raise ValueError(message)

def read_json(path):return json.loads(Path(path).read_text())

def canonical_commits(lock,name):
    pin=lock['profiles'][name];repository=ROOT/lock['repository_path']
    try:
        return subprocess.check_output(['git','-C',str(repository),'rev-list','--reverse',
            pin['base_commit']+'..'+pin['commit']],text=True,stderr=subprocess.PIPE).splitlines()
    except (OSError,subprocess.CalledProcessError) as error:
        raise ValueError(f'{name}: canonical commits unavailable in pinned local source repository') from error

def image_info(path,kernel=True):
    path=Path(path);size=path.stat().st_size
    require(64<=size<LOAD_LIMIT,f'{path}: image outside SimpleInit load bounds')
    with path.open('rb') as stream:
        head=stream.read(64)
        require(head[:2]==b'MZ',f'{path}: missing EFI PE/COFF header')
        if kernel:require(head[56:60]==b'ARM\x64',f'{path}: not an ARM64 Linux Image')
        pe=struct.unpack_from('<I',head,60)[0]
        require(pe+94<=size,f'{path}: PE header outside image')
        stream.seek(pe);header=stream.read(94)
    require(header[:4]==b'PE\0\0' and struct.unpack_from('<H',header,4)[0]==0xaa64,f'{path}: not AARCH64 PE')
    require(struct.unpack_from('<H',header,24)[0]==0x20b,f'{path}: not PE32+')
    require(struct.unpack_from('<H',header,92)[0]==10,f'{path}: not an EFI application')
    return {'bytes':size,'sha256':digest(path),'arm64_magic':kernel,'efi_stub':True}

def fdt_info(path):
    data=Path(path).read_bytes()
    require(40<=len(data)<=0x200000,f'{path}: DTB size outside ARM64 contract')
    magic,total,off_struct,off_strings,off_reserve,version,last_version,_,strings_size,struct_size=struct.unpack_from('>10I',data)
    require(magic==0xd00dfeed and total==len(data),f'{path}: incomplete/concatenated FDT')
    require(version>=17 and last_version<=17,f'{path}: unsupported FDT version')
    require(off_struct%4==0 and off_reserve%8==0,f'{path}: bad FDT alignment')
    for offset,size in ((off_struct,struct_size),(off_strings,strings_size)):
        require(offset>=40 and offset+size<=total,f'{path}: FDT block exceeds file')
    reserve=off_reserve
    while True:
        require(reserve>=40 and reserve+16<=total,f'{path}: unterminated FDT reserve map')
        address,size=struct.unpack_from('>QQ',data,reserve);reserve+=16
        if address==0 and size==0:break
    cursor=off_struct;end=off_struct+struct_size;strings=data[off_strings:off_strings+strings_size]
    for first,last in ((off_struct,off_struct+struct_size),(off_strings,off_strings+strings_size)):
        require(reserve<=first or off_reserve>=last,f'{path}: overlapping FDT reserve/block data')
    require(off_struct+struct_size<=off_strings or off_strings+strings_size<=off_struct,f'{path}: overlapping FDT structure/strings')
    stack=[];props={};ended=False;root_seen=False
    while cursor+4<=end:
        token=struct.unpack_from('>I',data,cursor)[0];cursor+=4
        if token==1:
            stop=data.find(b'\0',cursor,end);require(stop>=cursor,f'{path}: unterminated FDT node')
            name=data[cursor:stop].decode('ascii')
            if not stack:
                require(not root_seen and not name,f'{path}: invalid/multiple FDT root');root_seen=True
            stack.append(name);cursor=(stop+4)&~3
        elif token==2:
            require(bool(stack),f'{path}: unbalanced FDT nodes');stack.pop()
        elif token==3:
            require(stack and cursor+8<=end,f'{path}: property outside FDT node')
            size,name_offset=struct.unpack_from('>II',data,cursor);cursor+=8
            require(name_offset<len(strings) and cursor+size<=end,f'{path}: invalid FDT property bounds')
            stop=strings.find(b'\0',name_offset);require(stop>=name_offset,f'{path}: unterminated property name')
            name=strings[name_offset:stop].decode('ascii');node='/'+('/'.join(stack[1:]))
            props[(node,name)]=data[cursor:cursor+size];cursor=(cursor+size+3)&~3
        elif token==4:continue
        elif token==9:
            require(root_seen and not stack,f'{path}: FDT ends before root is complete');ended=True;break
        else:raise ValueError(f'{path}: invalid FDT token {token}')
    require(ended,f'{path}: missing FDT_END')
    def strings_at(node,key):
        value=props.get((node,key),b'')
        require(not value or value[-1]==0,f'{path}: invalid FDT string {node}/{key}')
        return [s.decode('utf-8') for s in value.rstrip(b'\0').split(b'\0') if s]
    return {'bytes':len(data),'sha256':digest(path),'complete_fdt':True,
        'model':strings_at('/','model'),'compatible':strings_at('/','compatible'),
        'chosen_bootargs':strings_at('/chosen','bootargs'),'board_topology_verified':False}

def initramfs_info(path):
    path=Path(path);require(0<path.stat().st_size<LOAD_LIMIT,f'{path}: initramfs outside SimpleInit load bounds')
    with path.open('rb') as stream:compressed=stream.read(2)==b'\x1f\x8b'
    if compressed:
        with gzip.open(path,'rb') as stream:data=stream.read(LOAD_LIMIT)
    else:data=path.read_bytes()
    require(len(data)<LOAD_LIMIT,f'{path}: decompressed initramfs is too large')
    cursor=0;members={};trailer=False
    while cursor+110<=len(data):
        header=data[cursor:cursor+110]
        require(header[:6] in (b'070701',b'070702'),f'{path}: expected newc CPIO')
        fields=[int(header[i:i+8],16) for i in range(6,110,8)]
        mode,size,name_size=fields[1],fields[6],fields[11];cursor+=110
        require(name_size>=1 and cursor+name_size<=len(data),f'{path}: invalid CPIO name')
        raw_name=data[cursor:cursor+name_size];require(raw_name[-1]==0,f'{path}: unterminated CPIO name')
        name=raw_name[:-1].decode('utf-8');cursor=(cursor+name_size+3)&~3
        require(cursor+size<=len(data),f'{path}: truncated CPIO member')
        contents=data[cursor:cursor+size];cursor=(cursor+size+3)&~3
        if name=='TRAILER!!!':trailer=True;break
        clean=str(PurePosixPath(name));require(not clean.startswith('/') and '..' not in PurePosixPath(clean).parts,f'{path}: invalid CPIO path')
        members[clean.removeprefix('./')]={'mode':mode,'data':contents}
    require(trailer,f'{path}: missing CPIO trailer')
    item=members.get('init');require(item is not None,f'{path}: no /init')
    if item['mode']&0o170000==0o120000:
        target=item['data'].decode();item=members.get(target.lstrip('/'))
        require(item is not None,f'{path}: /init symlink target absent')
    require(item['mode']&0o111,f'{path}: /init is not executable')
    if item['data'].startswith(b'\x7fELF'):
        require(len(item['data'])>=20 and struct.unpack_from('<H',item['data'],18)[0]==183,f'{path}: /init ELF is not AARCH64')
    else:require(item['data'].startswith(b'#!'),f'{path}: /init has no executable contract')
    return {'bytes':path.stat().st_size,'sha256':digest(path),'format':'gzip-newc' if compressed else 'newc',
        'uncompressed_bytes':len(data),'init_executable':True,'members':len(members)}

def validate_menu(menu):
    """Validate the subset actually consumed by fixed SimpleInit C readers."""
    require(menu['locates']['piano']['by_file']=='\\EFI\\Piano\\BootManifest.json','missing explicit marker locate')
    boot=menu['boot'];entries=boot['configs']
    require(boot['default'] in entries and type(boot['timeout']) is int,'invalid default/timeout')
    require(boot['default']!='piano-next','Next must remain separate from rescue default')
    for name,entry in entries.items():
        require(entry['mode'] in ('linux','efi','none','simple-init'),'unsupported boot mode')
        require(type(entry['show']) is bool and type(entry['enabled']) is bool,'menu flags must be booleans')
        extra=entry.get('extra',{})
        if entry['mode']=='linux':
            require(extra.get('use_uefi') is True,'Linux must use fixed source use_uefi key')
            require('use_efi' not in extra,'obsolete use_efi key')
            require(extra.get('skip_kernel_fdt_cmdline') is True,'inherited kernel cmdline must be disabled')
            require(len(extra['cmdline'].encode('ascii'))<511 and 'rdinit=/init' in extra['cmdline'],'invalid EFI LoadOptions cmdline')
            require(not any(s in extra['cmdline'] for s in ('root=','userdata','/beaconinit','/pianoinit')),'unexpected storage root/init fallback')
            profile='Stable' if name=='piano-stable' else 'Next'
            for key,file_name in (('kernel','Image.efi'),('dtb','piano.dtb'),('initrd','initramfs.cpio.gz')):
                expected=f'locate://piano/EFI/Piano/{profile}/{file_name}'
                require(extra.get(key)==expected,f'{name}: invalid {key} path')
                parsed=urlsplit(extra[key]);require(parsed.scheme=='locate' and parsed.netloc=='piano','invalid real locate URI')
        if name in ('android','recovery'):require(not entry['enabled'] and entry['mode']=='none','unimplemented Android handoff must stay inactive')
        if name=='shell':require(extra.get('options_widechar') is True and 'options_wchar' not in extra,'obsolete Shell option encoding key')
    return True

def source_contract():
    files={
        'json':'src/confd/json_conf.c','paths':'src/filesystem/layer/uefi.c',
        'linux':'src/linux-boot/conf.c','efi':'src/boot/efi.c',
        'load_bounds':'src/linux-boot/loader.c','static_prefix':'SimpleInit.dec'
    }
    checks={
        'json':'load_json_object(hand,obj,"")','paths':'strcasecmp(u->scheme,"locate")==0',
        'linux':'load_boolean(key,"use_uefi",cfg->use_uefi)',
        'efi':'"options_widechar",true','load_bounds':'size>=0x8000000',
        'static_prefix':'PcdConfDefaultStaticPrefix  | "\\\\simpleinit.static.uefi"'
    }
    result={}
    for key,relative in files.items():
        path=ROOT/'upstream/simple-init'/relative
        require(checks[key] in path.read_text(),f'SimpleInit contract changed: {relative}')
        result[key]={'path':relative,'sha256':digest(path)}
    return result

def validate_build(name,manifest_path,mode,allow_unverified,lock,image_name='Image'):
    path=Path(manifest_path).resolve();manifest=read_json(path);folder=path.parent;pin=lock['profiles'][name]
    require(manifest.get('profile')==name and manifest.get('mode')==mode,f'{name}: wrong artifact profile/mode')
    require(manifest.get('source_commit')==pin['commit'] and manifest.get('base_commit')==pin['base_commit'],f'{name}: stale/unpinned source manifest')
    require(manifest.get('canonical_patch_commits')==canonical_commits(lock,name),f'{name}: canonical patch commit list mismatch')
    require(manifest.get('source_dirty') is False,f'{name}: source dirty state is not known clean')
    require(manifest.get('status') in ('HOST_BUILT_NOT_HARDWARE_VERIFIED','HARDWARE_VERIFIED'),f'{name}: kernel is not built')
    verified=manifest.get('hardware_verified') is True
    require(verified or allow_unverified,f'{name}: unverified; use --allow-unverified for a candidate tree')
    config=folder/'config';fragment=ROOT/'linux/configs'/f'piano-{mode}.config'
    require(config.is_file() and digest(config)==manifest.get('config_sha256'),f'{name}: config hash mismatch')
    require(fragment.is_file() and digest(fragment)==manifest.get('fragment_sha256'),f'{name}: stale fragment hash')
    lines=set(config.read_text().splitlines())
    require({'CONFIG_EFI=y','CONFIG_BLK_DEV_INITRD=y','CONFIG_RD_GZIP=y'}<=lines,f'{name}: EFI/initramfs/gzip config absent')
    require('CONFIG_CMDLINE_FORCE=y' not in lines,f'{name}: forced downstream cmdline')
    for line in lines:
        if line.startswith('CONFIG_CMDLINE='):require(line=='CONFIG_CMDLINE=""',f'{name}: embedded downstream cmdline is nonempty')
    image=folder/image_name;actual=image_info(image)
    recorded=manifest.get('image') or {}
    require(recorded.get('efi_stub') is True and recorded.get('arm64_magic') is True,f'{name}: manifest missing EFI stub contract')
    require(recorded.get('sha256')==actual['sha256'] and recorded.get('bytes')==actual['bytes'],f'{name}: Image hash/size mismatch')
    dtb_record=manifest.get('dtb');require(isinstance(dtb_record,dict),f'{name}: explicit final DTB absent; no implicit replacement')
    dtb=folder/'piano.dtb';actual_dtb=fdt_info(dtb)
    require(dtb_record.get('sha256')==actual_dtb['sha256'] and dtb_record.get('source'),f'{name}: DTB provenance/hash mismatch')
    require(dtb_record.get('validation'),f'{name}: DTB validation status absent')
    return manifest,actual,actual_dtb,verified

def prepare(args,root=ROOT):
    lock=read_json(root/'linux/kernel-profiles.json');menu=read_json(TEMPLATE)
    result={'schema_version':1,'state':'HOST_ONLY_NOT_DEPLOYED','storage_location':'UNASSIGNED',
        'default_policy':'verified Stable rescue first; Next independent; no userdata fallback',
        'profiles':{},'files':[],'simpleinit_source_contract':source_contract(),
        'inactive_contracts':{'android':'handoff pending','recovery':'handoff pending'}}
    destination=args.output.resolve();require(not destination.exists(),f'refusing to replace existing tree: {destination}')
    destination.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.piano-boot-',dir=destination.parent) as temporary:
        tree=Path(temporary)/'tree';tree.mkdir();piano=tree/'EFI/Piano';piano.mkdir(parents=True)
        def stage(source,relative):
            target=tree/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
            result['files'].append({'path':'/'+relative,'bytes':target.stat().st_size,'sha256':digest(target),'source':str(Path(source).resolve())})
        for name in ('stable','next'):
            folder=piano/name.title();folder.mkdir();manifest_path=getattr(args,name+'_manifest')
            item={'status':'NOT_STAGED','source_commit':lock['profiles'][name]['commit'],'hardware_verified':False,
                'rescue_eligible':False,'dtb':None,'menu_entry':'piano-'+name}
            result['profiles'][name]=item
            if args.plan_only:continue
            if manifest_path is None:continue
            manifest,image,dtb,verified=validate_build(name,manifest_path,args.mode,args.allow_unverified,lock)
            initrd=getattr(args,name+'_initramfs') or args.initramfs
            require(initrd is not None,f'{name}: explicit initramfs required')
            ramdisk=initramfs_info(initrd)
            initrd_data=Path(initrd).read_bytes()
            prefix=f'EFI/Piano/{name.title()}'
            for source,file_name in ((Path(manifest_path).parent/'Image','Image.efi'),(Path(manifest_path).parent/'config','config'),
                                     (Path(manifest_path).parent/'piano.dtb','piano.dtb'),(manifest_path,'BuildManifest.json')):
                stage(source,prefix+'/'+file_name)
            # Normalize the staged name to gzip; record the original as well.
            staged=tree/prefix/'initramfs.cpio.gz'
            staged.write_bytes(initrd_data if ramdisk['format']=='gzip-newc' else gzip.compress(initrd_data,mtime=0))
            result['files'].append({'path':'/'+prefix+'/initramfs.cpio.gz','bytes':staged.stat().st_size,'sha256':digest(staged),'source':str(Path(initrd).resolve())})
            item.update(status='VERIFIED_HOST_STAGED' if verified else 'UNVERIFIED_HOST_STAGED',hardware_verified=verified,
                rescue_eligible=verified and name=='stable',mode=args.mode,source_manifest_sha256=digest(manifest_path),
                config_sha256=manifest['config_sha256'],fragment_sha256=manifest['fragment_sha256'],
                source_commit=manifest['source_commit'],base_commit=manifest['base_commit'],canonical_patch_commits=manifest['canonical_patch_commits'],
                compiler=manifest['compiler'],kernel_release=manifest['kernel_release'],image=image,
                dtb={**dtb,'source':manifest['dtb']['source'],'validation':manifest['dtb']['validation']},initramfs=ramdisk)
            entry=menu['boot']['configs']['piano-'+name];entry['enabled']=True
            entry['desc']='Piano '+name.title()+(' (verified rescue)' if item['rescue_eligible'] else ' (unverified candidate)' if not verified else ' (experimental)')
        if args.shell_efi and not args.plan_only:
            shell=image_info(args.shell_efi,kernel=False);stage(args.shell_efi,'EFI/Piano/Shell.efi')
            menu['boot']['configs']['shell'].update(enabled=True,desc='UEFI Shell');result['shell']=shell
        else:result['inactive_contracts']['shell']='external EFI file not staged'
        if result['profiles']['stable']['rescue_eligible']:
            menu['boot']['default']=menu['boot']['current']='piano-stable'
        validate_menu(menu)
        text=json.dumps(menu,indent=2)+'\n'
        (tree/'simpleinit.static.uefi.json').write_text(text)
        (piano/'simpleinit.static.uefi.json').write_text(text)
        for relative in ('simpleinit.static.uefi.json','EFI/Piano/simpleinit.static.uefi.json'):
            path=tree/relative;result['files'].append({'path':'/'+relative,'bytes':path.stat().st_size,'sha256':digest(path),'source':'generated from fixed SimpleInit schema'})
        result['default']=menu['boot']['default'];result['source_contract']={
            'configuration':'SimpleInit json_conf.c nested JSON; root static PCD prefix',
            'paths':'filesystem/layer/uefi.c locate://tag/absolute-path',
            'linux':'linux-boot/conf.c use_uefi + kernel/dtb/initrd; DTB config table + LoadFile2 initrd',
            'not_runtime_verified':'Staging checks never imply files already exist on UFS or EBS/DMA handoff works'}
        (piano/'BootManifest.json').write_text(json.dumps(result,indent=2)+'\n')
        shutil.move(str(tree),destination)
    return result

def check_tree(tree):
    tree=Path(tree).resolve();manifest=read_json(tree/'EFI/Piano/BootManifest.json');menu=read_json(tree/'simpleinit.static.uefi.json')
    require(manifest.get('state')=='HOST_ONLY_NOT_DEPLOYED','tree lacks honest host-only state');validate_menu(menu)
    lock=read_json(ROOT/'linux/kernel-profiles.json')
    for name,item in manifest['profiles'].items():
        require(item['source_commit']==lock['profiles'][name]['commit'],f'{name}: tree source pin stale')
        require(menu['boot']['configs']['piano-'+name]['enabled']==(item['status']!='NOT_STAGED'),f'{name}: menu availability differs from manifest')
        if name=='stable' and not item['rescue_eligible']:require(menu['boot']['default']!='piano-stable','unverified Stable is rescue default')
        if item['status']!='NOT_STAGED':
            prefix=tree/'EFI/Piano'/name.title();build_path=prefix/'BuildManifest.json'
            original,_,_,verified=validate_build(name,build_path,item['mode'],True,lock,image_name='Image.efi')
            require(digest(build_path)==item['source_manifest_sha256'],f'{name}: original build manifest changed')
            for field in ('source_commit','base_commit','canonical_patch_commits','config_sha256','fragment_sha256','compiler','kernel_release'):
                require(item[field]==original[field],f'{name}: staged provenance differs: {field}')
            require(item['hardware_verified']==verified and item['rescue_eligible']==(verified and name=='stable'),f'{name}: rescue/verified state contradicts build manifest')
            initramfs_info(prefix/'initramfs.cpio.gz')
    for item in manifest['files']:
        relative=PurePosixPath(item['path']);require(relative.is_absolute() and '..' not in relative.parts,'invalid manifest file path')
        path=tree/str(relative).lstrip('/');require(path.resolve().is_relative_to(tree),'manifest file escapes tree')
        require(path.is_file() and path.stat().st_size==item['bytes'] and digest(path)==item['sha256'],f'{path}: staged file hash/size mismatch')
    require(read_json(tree/'EFI/Piano/simpleinit.static.uefi.json')==menu,'static config copies differ')
    return manifest

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'artifacts/piano-boot')
    parser.add_argument('--mode',choices=('ram','userspace-debug'),default='ram')
    parser.add_argument('--stable-manifest',type=Path)
    parser.add_argument('--next-manifest',type=Path)
    parser.add_argument('--initramfs',type=Path,help='Explicit common RAM filesystem; no automatic userdata/rootfs selection')
    parser.add_argument('--stable-initramfs',type=Path)
    parser.add_argument('--next-initramfs',type=Path)
    parser.add_argument('--shell-efi',type=Path)
    parser.add_argument('--allow-unverified',action='store_true')
    parser.add_argument('--plan-only',action='store_true',help='Write inactive menu/BootManifest, with no claimed kernel or DTB files')
    parser.add_argument('--check-tree',type=Path)
    args=parser.parse_args()
    try:
        if args.check_tree:result=check_tree(args.check_tree)
        else:
            require(args.plan_only or args.stable_manifest or args.next_manifest,'specify a built manifest or --plan-only')
            result=prepare(args)
        print(json.dumps({'state':result['state'],'default':result['default'],'profiles':result['profiles']},indent=2))
    except (ValueError,OSError,KeyError,struct.error) as error:raise SystemExit(str(error))

if __name__=='__main__':main()
