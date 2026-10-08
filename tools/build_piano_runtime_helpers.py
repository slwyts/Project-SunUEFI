#!/usr/bin/env python3
"""One reproducible Piano runtime build/stage entry; no device or global install.

Small generated main wrappers add a --help path before any device code. Touch
diagnostics are applied to a copied, pinned public source. A new output directory
is mandatory; the public checkout and device remain untouched.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

from prepare_linux_modules import modinfo

ROOT=Path(__file__).resolve().parents[1]
PUBLIC_COMMIT='a75f8c5d5fa099d65c171ac839c2e3bb6c63ec45'
UAPI_COMMIT='352508459733d3e6d349ea5581a8dd2fd8bb4180'
UAPI_DIGEST='c4c7b7358f4440da8832eea607ff812fb5481181401e869a34cca70bbe103d4c'
MACROS_COMMIT='993a17dcb672357998a463a73f120064d6c74f4f'
LOOP_COMMIT='0f9ee86760b7f2bea174b7e3e7a1d38845da0ab4'
PUBLIC_SOURCES={
 'piano-touch-view':('initramfs/touch-view/piano-touch-view.c','eb1bac5a7bfac3200248ebe39e8d9564bed3d7848b11655ded5d065dc0ef6e12','usr/bin/piano-touch-view'),
 'piano-camerad':('camera/piano-camerad.c','c2ab6391fa41d21d1c3e4359dc0adf671261417dbda180724faf8b2758932d47','usr/lib/piano/piano-camerad'),
 'piano-pd-locator':('initramfs/pd-locator/piano-pd-locator.c','8de9d2840a896b4bd6c90bd4b124479d2f142e9e85420f2022894bb32fc8dde8','usr/sbin/piano-pd-locator')}
BSP_SOURCES={
 'piano-camera-ctl':('linux/userspace/piano-camera-ctl.c',None,'usr/bin/piano-camera-ctl')}
TOOL_PINS={'clang':'939a882527432ec23b094c289e7f170bf2d6ec282e74dde75e31b08602fa3eae',
 'ld.lld':'57b6c64db534793f05918a6e935c900e9938bd64d0ac95933285930b568387ea',
 'llvm-strip':'629062ddc62f936d7f07418099d202850e18b222217a85f419552fa25d3eb4ec'}
QEMU_PIN='73cc2584f119f9e74c85c0a8549dbb6fd104ae3897a06a7500797f9e5b4d0943'
ALSA_TOOLCHAIN_PIN='09676a0fc68ee9cd5ba6684646b7e82c96e2ed5ac7ba53d639c4fe53b0612346'
TOPOLOGY_PIN='6b10e42b5d0b4242004c750613462c2ccd7d37cd180ca842abbb431bda6057bb'
DEFAULT_RELEASE='7.2.6-piano-gnome-00061-g352508459733'
TOUCH_DIAGNOSTICS_PATCH=ROOT/'tools/patches/piano-touch-view-observability.patch'
TOUCH_CAPTURE_PATCH=ROOT/'tools/patches/piano-touch-view-raw-capture.patch'
CAMERAD_CCM_PATCH=ROOT/'tools/patches/piano-camerad-writable-ccm.patch'
CAMERAD_AE_PATCH=ROOT/'tools/patches/piano-camerad-stable-ae.patch'
CAMERAD_FRAME_PATCH=ROOT/'tools/patches/piano-camerad-frame-integrity.patch'
CAMERAD_MANUAL_PATCH=ROOT/'tools/patches/piano-camerad-manual-controls.patch'
CAMERAD_TIMING_PATCH=ROOT/'tools/patches/piano-camerad-front-exposure-time.patch'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def capture(argv,cwd=None,env=None):return subprocess.check_output([str(x)for x in argv],cwd=cwd,env=env,text=True).strip()
def run(argv,env,log):
    with log.open('a')as stream:
        stream.write(json.dumps([str(x)for x in argv])+'\n');stream.flush()
        subprocess.run([str(x)for x in argv],env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)


def repository(path,pin):
    if capture(['git','rev-parse','HEAD'],path)!=pin or capture(['git','status','--porcelain'],path):raise ValueError('Pinned runtime repository changed: '+str(path))


def tree_files(folder):return {p.relative_to(folder).as_posix():sha(p)for p in sorted(folder.rglob('*'))if p.is_file()}
def tree_digest(rows):return hashlib.sha256(json.dumps(rows,sort_keys=True).encode()).hexdigest()


def entry_source(name):
    if name not in PUBLIC_SOURCES and name not in BSP_SOURCES:raise ValueError('Unknown runtime helper')
    signature='int PianoOriginalMain(int,char **);'if name in ('piano-touch-view','piano-camera-ctl')else'int PianoOriginalMain(void);'
    normal='return PianoOriginalMain(argc,argv);'if name in ('piano-touch-view','piano-camera-ctl')else'if(argc!=1){fprintf(stderr,"Use --help or no arguments.\\n");return 2;} return PianoOriginalMain();'
    description=' Optional --diagnostics N emits touch JSON; --capture FILE saves this reader\'s complete raw records with --capture-seconds N (1..10, default5, max16MiB). Both default off.'if name=='piano-touch-view'else''
    if name=='piano-camera-ctl':description=' caps|get rear|front; set rear|front ae|awb|af auto|manual; set rear|front exposure|analog-gain|digital-gain|red-balance|blue-balance|focus INTEGER; set rear|front exposure-time-ns NANOSECONDS quantizes to the verified active sensor mode.'
    return '#include <stdio.h>\n#include <string.h>\n'+signature+'\nint main(int argc,char **argv){if(argc==2 && !strcmp(argv[1],"--help")){puts("'+name+': Linux Piano runtime helper; --help performs no device access.'+description+'");return 0;}'+normal+'}\n'


def derive_touch_source(public, output):
    """Apply optional diagnostics/capture to the same verified public copy."""
    relative,pin,_=PUBLIC_SOURCES['piano-touch-view'];original=Path(public)/relative
    if sha(original)!=pin:raise ValueError('Public touch source changed')
    folder=Path(output)/'piano-touch-source';target=folder/relative
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,target)
    patches=[]
    for patch in (TOUCH_DIAGNOSTICS_PATCH,TOUCH_CAPTURE_PATCH):
        for options in (['--check'],[]):
            subprocess.run(['git','apply','--no-index','--unidiff-zero',*options,str(patch)],cwd=folder,check=True)
        patches.append({'file':str(patch.relative_to(ROOT)),'sha256':sha(patch)})
    return target,{'public_source_sha256':pin,'patch_sha256':sha(TOUCH_DIAGNOSTICS_PATCH),
                  'patches':patches,'effective_source_sha256':sha(target)}


def derive_camerad_source(public, output):
    """Apply CCM, AE/AF, frame integrity, manual controls and mode timing."""
    relative,pin,_=PUBLIC_SOURCES['piano-camerad'];original=Path(public)/relative
    if sha(original)!=pin:raise ValueError('Public camera source changed')
    folder=Path(output)/'piano-camera-source';target=folder/relative
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,target)
    patches=[]
    for patch in (CAMERAD_CCM_PATCH,CAMERAD_AE_PATCH,CAMERAD_FRAME_PATCH,CAMERAD_MANUAL_PATCH,CAMERAD_TIMING_PATCH):
        # This generated directory is inside the parent Git checkout. Git can
        # silently skip a git-format patch whose paths lie outside that prefix.
        # Apply the standard patch to this standalone source copy instead.
        subprocess.run(['patch','--batch','--forward','--fuzz=0',
                        '--no-backup-if-mismatch','-p1','-i',str(patch)],
                       cwd=folder,check=True)
        patches.append({'file':str(patch.relative_to(ROOT)),'sha256':sha(patch)})
    return target,{'public_source_sha256':pin,'patch':'tools/patches/piano-camerad-writable-ccm.patch',
                  'patch_sha256':sha(CAMERAD_CCM_PATCH),'patches':patches,
                  'effective_source_sha256':sha(target)}


def kernel_compiler_source(build,source,commit):
    build,source=Path(build).resolve(),Path(source).resolve()
    repository(source,commit)
    # Parse the known Kbuild three-line output, never execute Make to discover
    # its include. No variables, extra directives, aliases or external paths.
    makefile=build/'Makefile'
    lines=makefile.read_text().splitlines()
    if makefile.is_symlink()or len(lines)!=3 or not lines[2].startswith('include ')or not lines[2].endswith('/Makefile'):
        raise ValueError('Unexpected generated kernel Makefile format')
    raw=lines[2][len('include '):-len('/Makefile')]
    if not re.fullmatch(r'/[A-Za-z0-9_./+-]+',raw):raise ValueError('Kernel source path contains Make syntax')
    compiler=Path(raw).resolve()
    if (str(compiler)!=raw or not compiler.is_relative_to(ROOT/'build/kernel-worktrees')or
            not build.is_relative_to(ROOT/'build/kernels')):
        raise ValueError('Compiler checkout/build directory escapes the build-owned paths')
    expected=["# Automatically generated by "+raw+"/Makefile: don't edit",
              'export KBUILD_OUTPUT = '+str(build),'include '+raw+'/Makefile']
    if lines!=expected:raise ValueError('Generated kernel Makefile has unexpected directives')
    repository(compiler,commit)
    if capture(['git','rev-parse','HEAD^{tree}'],compiler)!=capture(['git','rev-parse','HEAD^{tree}'],source):
        raise ValueError('Compiler and prepared kernel source trees differ')
    return compiler


def kernel_identity(build,source,commit,release):
    if not re.fullmatch(r'[a-zA-Z0-9_.+-]{1,128}',release)or not re.fullmatch(r'[0-9a-f]{40}',commit):raise ValueError('Exact kernel release/commit required')
    repository(source,commit)
    compiler=kernel_compiler_source(build,source,commit)
    actual=(build/'include/config/kernel.release').read_text().strip()
    if actual!=release or ('#define UTS_RELEASE "'+release+'"')not in(build/'include/generated/utsrelease.h').read_text():raise ValueError('Kernel build release differs from requested ABI')
    rows={str(p):sha(p)for p in (build/'.config',build/'Module.symvers',build/'include/config/kernel.release',build/'include/generated/utsrelease.h')}
    identity={'commit':commit,'release':release,'source':str(source),'build':str(build),'inputs':rows}
    if compiler!=source:
        identity.update(compiler_source=str(compiler),source_tree=capture(['git','rev-parse','HEAD^{tree}'],source))
        rows[str(build/'Makefile')]=sha(build/'Makefile')
    return identity


def verify_elf(path):
    raw=path.read_bytes()
    if len(raw)<64 or raw[:6]!=b'\x7fELF\x02\x01'or struct.unpack_from('<H',raw,18)[0]!=183:raise ValueError('Runtime is not ELF64 AArch64')
    offset=struct.unpack_from('<Q',raw,32)[0];step,count=struct.unpack_from('<HH',raw,54)
    if step!=56 or offset+step*count>len(raw):raise ValueError('Runtime ELF program table invalid')
    if any(struct.unpack_from('<I',raw,offset+i*step)[0]==3 for i in range(count)):raise ValueError('Runtime needs dynamic interpreter')
    return {'bytes':len(raw),'sha256':sha(path),'machine':183,'static_no_pt_interp':True}


def stage(output,destination):
    if not destination.resolve().is_relative_to(ROOT/'build/distros'):raise ValueError('Stage only into a derived workspace distro')
    manifest=json.loads((output/'manifest.json').read_text());files=manifest['runtime_files'];release=manifest['kernel']['release']
    if not re.fullmatch(r'[a-zA-Z0-9_.+-]{1,128}',release):raise ValueError('Unsafe runtime kernel release')
    expected={row[2]for row in (PUBLIC_SOURCES|BSP_SOURCES).values()}|{'usr/lib/firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin','usr/lib/modules/'+release+'/updates/v4l2loopback.ko'}
    if set(files)!=expected:raise ValueError('Runtime bundle source/install paths differ from the exact contract')
    if not(destination/'usr/lib/modules'/release/'kernel').is_dir():raise ValueError('Stage the matching full kernel module tree first')
    # Prevalidate the entire generation before changing any destination file.
    for name,row in files.items():
        p=output/row['file']
        target=destination/name
        if not p.resolve().is_relative_to(output.resolve())or not target.resolve().is_relative_to(destination.resolve())or target.is_symlink():raise ValueError('Runtime bundle destination/source alias escapes staging root')
        if not p.is_file()or p.is_symlink()or p.stat().st_size!=row['bytes']or sha(p)!=row['sha256']:raise ValueError('Runtime bundle changed: '+name)
    for name,row in files.items():
        target=destination/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(output/row['file'],target);target.chmod(row['mode'])
    subprocess.run(['depmod','-b',str(destination),'-m','/usr/lib/modules',manifest['kernel']['release']],check=True)
    record={'status':'REAL_RUNTIME_STAGED_NOT_DEVICE_TESTED','bundle_manifest_sha256':sha(output/'manifest.json'),'kernel':manifest['kernel'],'files':files,'device_tested':False}
    p=destination/'usr/share/piano-provenance/runtime-helpers.json';p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(record,indent=2)+'\n')
    return record


def build(args):
    output=args.output.resolve()
    if not output.is_relative_to(ROOT/'build')or output.exists():raise ValueError('Choose a new workspace build output; preserve prior bundles')
    public=args.public_source.resolve();repository(public,PUBLIC_COMMIT)
    source=args.kernel_source.resolve();identity=kernel_identity(args.kernel_build.resolve(),source,args.kernel_commit,args.kernel_release)
    # UAPI remains the exact public 3525 baseline for the C helper interfaces;
    # the external module binds the independently requested kernel commit/ABI.
    uapi=args.uapi.resolve();uapi_rows=tree_files(uapi/'include')
    if tree_digest(uapi_rows)!=UAPI_DIGEST:raise ValueError('3525 UAPI header generation changed')
    repository(args.macros.resolve(),MACROS_COMMIT);repository(args.v4l2_source.resolve(),LOOP_COMMIT)
    tools=ROOT/'build/host-tools/usr/bin';env=os.environ.copy();env['PATH']=str(tools)+os.pathsep+env['PATH'];env['LD_LIBRARY_PATH']=str(ROOT/'build/host-tools/usr/lib')
    inputs={str(Path(__file__).resolve()):sha(Path(__file__).resolve())}
    inputs[str(TOUCH_DIAGNOSTICS_PATCH)]=sha(TOUCH_DIAGNOSTICS_PATCH)
    inputs[str(TOUCH_CAPTURE_PATCH)]=sha(TOUCH_CAPTURE_PATCH)
    for patch in (CAMERAD_CCM_PATCH,CAMERAD_AE_PATCH,CAMERAD_FRAME_PATCH,CAMERAD_MANUAL_PATCH,CAMERAD_TIMING_PATCH):
        inputs[str(patch)]=sha(patch)
    for name,pin in TOOL_PINS.items():
        p=tools/name
        if sha(p)!=pin:raise ValueError('Reviewed LLVM tool changed: '+name)
        inputs[str(p)]=pin
    qemu=ROOT/'build/distro-tools/usr/bin/qemu-aarch64'
    if sha(qemu)!=QEMU_PIN:raise ValueError('Reviewed QEMU changed')
    inputs[str(qemu)]=QEMU_PIN
    alsa_record=ROOT/'build/piano-runtime/topology/evidence/host-toolchain.json'
    if sha(alsa_record)!=ALSA_TOOLCHAIN_PIN:raise ValueError('Reviewed ALSA tooling record changed')
    alsa=json.loads(alsa_record.read_text());alsa_root=args.alsa_tools.resolve()
    for path,pin in ((Path(alsa['loader']),alsa['loader_sha256']),(Path(alsa['m4_path']),alsa['m4_sha256'])):
        if sha(path)!=pin:raise ValueError('Reviewed ALSA loader/m4 changed')
        inputs[str(path)]=pin
    for name,row in alsa['files'].items():
        p=alsa_root/name
        if 'symlink'in row:
            if not p.is_symlink()or p.readlink().as_posix()!=row['symlink']:raise ValueError('ALSA tool symlink changed')
        elif sha(p)!=row['sha256']:raise ValueError('ALSA tool ELF changed')
        if not p.is_symlink():inputs[str(p)]=sha(p)
    sysroot=args.sysroot.resolve()
    for folder in ('usr/include','usr/lib/gcc/aarch64-linux-gnu/14','usr/lib/aarch64-linux-gnu'):
        for path in (sysroot/folder).rglob('*'):
            if path.is_file()and path.suffix in ('.h','.a','.o'):inputs[str(path)]=sha(path)
    if not(sysroot/'usr/lib/aarch64-linux-gnu/libc.a').is_file():raise ValueError('Verified official ARM64 static sysroot unavailable')
    for name,(path,pin,_)in PUBLIC_SOURCES.items():
        p=public/path
        if sha(p)!=pin:raise ValueError('Public helper source changed: '+name)
        inputs[str(p)]=pin
    for name,(path,pin,_)in BSP_SOURCES.items():
        p=ROOT/path
        # Project sources are editable; record the actual bytes instead of
        # requiring developers to update a second, hard-coded source hash.
        inputs[str(p)]=sha(p)
    for folder in (public/'topology',args.macros.resolve()/'audioreach'):
        inputs.update({str(folder/name):digest for name,digest in tree_files(folder).items()})
    inputs[str(public/'scripts/build-topology.sh')]=sha(public/'scripts/build-topology.sh')
    inputs.update(identity['inputs']);output.mkdir(parents=True);log=output/'build.log';files={};commands=[]
    flags=[tools/'clang','--target=aarch64-linux-gnu','--sysroot='+str(sysroot),'--gcc-toolchain='+str(sysroot/'usr'),'-O2','-Wall','-Wextra','-Werror']
    touch_source,touch_provenance=derive_touch_source(public,output)
    camera_source,camera_provenance=derive_camerad_source(public,output)
    for name,(path,pin,destination)in (PUBLIC_SOURCES|BSP_SOURCES).items():
        entry=output/(name+'-entry.c');entry.write_text(entry_source(name));obj=output/(name+'.o');binary=output/name
        effective_source=touch_source if name=='piano-touch-view'else camera_source if name=='piano-camerad'else(ROOT/path if name in BSP_SOURCES else public/path)
        if name in BSP_SOURCES:pin=inputs[str(ROOT/path)]
        run([*flags,'-isystem',uapi/'include','-Dmain=PianoOriginalMain','-c',effective_source,'-o',obj],env,log)
        run([*flags,'-fuse-ld=lld','-static',entry,obj,'-lm','-o',binary],env,log)
        row=verify_elf(binary);help_result=subprocess.run([str(qemu),str(binary),'--help'],capture_output=True,text=True,timeout=10,check=True)
        row.update({'file':name,'mode':0o755,'source_sha256':pin,'entry_sha256':sha(entry),'compile_exit_code':0,'help_exit_code':help_result.returncode,'help_no_device_access':True});files[destination]=row
        if name=='piano-touch-view':row.update(touch_provenance)
        if name=='piano-camerad':row.update(camera_provenance)
        if name in BSP_SOURCES:row.update(source_kind='project-bsp',source_path=path)
    wrapper=output/'host-bin';wrapper.mkdir();exe=wrapper/'alsatplg';launch=[alsa['loader'],'--library-path',str(alsa_root/'usr/lib/x86_64-linux-gnu'),str(alsa_root/'usr/bin/alsatplg')]
    import shlex
    exe.write_text('#!/bin/sh\nexec '+shlex.join(launch)+' "$@"\n');exe.chmod(0o755);top_env=env.copy();top_env['PATH']=str(wrapper)+os.pathsep+top_env['PATH']
    run(['bash',public/'scripts/build-topology.sh',args.macros.resolve(),output/'firmware'],top_env,log)
    tplg=output/'firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin'
    if sha(tplg)!=TOPOLOGY_PIN:raise ValueError('Public topology differs from validated repeat build')
    run([*launch,'-d',tplg,'-o',output/'topology-decoded.conf'],env,log)
    files['usr/lib/firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin']={'file':tplg.relative_to(output).as_posix(),'bytes':tplg.stat().st_size,'sha256':sha(tplg),'mode':0o644,'decode_exit_code':0,'device_tested':False}
    module_source=output/'v4l2loopback';module_source.mkdir()
    tracked=capture(['git','ls-files','-z'],args.v4l2_source.resolve()).split('\0')
    for name in filter(None,tracked):
        original=args.v4l2_source.resolve()/name;target=module_source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,target);inputs[str(original)]=sha(original)
    run(['make','-C',args.kernel_build.resolve(),'M='+str(module_source),'ARCH=arm64','LLVM=1','LLVM_IAS=1','-j4','modules'],env,log)
    module=module_source/'v4l2loopback.ko';run([tools/'llvm-strip','--strip-debug',module],env,log);info=modinfo(module)
    if info.get('name')!='v4l2loopback'or info.get('vermagic','').split()[0]!=args.kernel_release:raise ValueError('External module does not match requested exact kernel')
    files['usr/lib/modules/'+args.kernel_release+'/updates/v4l2loopback.ko']={'file':module.relative_to(output).as_posix(),'bytes':module.stat().st_size,'sha256':sha(module),'mode':0o644,'vermagic':info['vermagic'],'source_commit':LOOP_COMMIT,'strip_debug':True,'loaded':False}
    for name,pin in inputs.items():
        if sha(Path(name))!=pin:raise ValueError('Runtime build input changed: '+name)
    kernel_identity(args.kernel_build.resolve(),source,args.kernel_commit,args.kernel_release)
    manifest={'status':'RUNTIME_COMPILED_NOT_DEVICE_TESTED','public_commit':PUBLIC_COMMIT,'uapi_commit':UAPI_COMMIT,'uapi_sha256':UAPI_DIGEST,'macros_commit':MACROS_COMMIT,'v4l2_commit':LOOP_COMMIT,'kernel':identity,'inputs':inputs,'runtime_files':files,'hardware_verified':False,'device_operation':False,'known_topology_limit':'ALSA decoded config does not recompile vendor data; original public config matches known binary SHA and decodes successfully'}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if args.stage_rootfs:stage(output,args.stage_rootfs.resolve())
    return manifest


def parser():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--public-source',type=Path,default=ROOT/'upstream/debian-piano-current');p.add_argument('--uapi',type=Path,default=ROOT/'build/piano-runtime/uapi')
    p.add_argument('--sysroot',type=Path,default=ROOT/'build/distros/piano-runtime-build');p.add_argument('--macros',type=Path,default=ROOT/'build/piano-runtime/topology/macros');p.add_argument('--alsa-tools',type=Path,default=ROOT/'build/piano-runtime/topology/host-tools')
    p.add_argument('--v4l2-source',type=Path,default=ROOT/'build/piano-runtime/v4l2loopback');p.add_argument('--kernel-source',type=Path,default=ROOT/'build/kernel-worktrees/piano-full-integration');p.add_argument('--kernel-commit',default=UAPI_COMMIT)
    p.add_argument('--kernel-build',type=Path,default=ROOT/'build/kernels/full-integration');p.add_argument('--kernel-release',default=DEFAULT_RELEASE);p.add_argument('--stage-rootfs',type=Path)
    return p


if __name__=='__main__':
    result=build(parser().parse_args());print(json.dumps({'status':result['status'],'kernel_release':result['kernel']['release'],'runtime_files':len(result['runtime_files'])},indent=2))
