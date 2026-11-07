#!/usr/bin/env python3
"""One reproducible Piano runtime build/stage entry; no device or global install.

Small generated main wrappers add a --help path to external helpers. Camera
sources provide their own main entry and are built through their Makefile. Touch
diagnostics are applied to a copied, pinned public source. A new output directory
is mandatory; the public checkout and device remain untouched.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess

from prepare_linux_modules import modinfo

ROOT=Path(__file__).resolve().parents[1]
PUBLIC_COMMIT='a75f8c5d5fa099d65c171ac839c2e3bb6c63ec45'
UAPI_COMMIT='352508459733d3e6d349ea5581a8dd2fd8bb4180'
UAPI_DIGEST='c4c7b7358f4440da8832eea607ff812fb5481181401e869a34cca70bbe103d4c'
SOURCE_PINS=json.loads((ROOT/'sources.lock.json').read_text())
MACROS_COMMIT=SOURCE_PINS['audioreach-topology']['commit']
LOOP_COMMIT=SOURCE_PINS['v4l2loopback']['commit']
PUBLIC_SOURCES={
 'piano-touch-view':('initramfs/touch-view/piano-touch-view.c','eb1bac5a7bfac3200248ebe39e8d9564bed3d7848b11655ded5d065dc0ef6e12','usr/bin/piano-touch-view'),
 'piano-pd-locator':('initramfs/pd-locator/piano-pd-locator.c','8de9d2840a896b4bd6c90bd4b124479d2f142e9e85420f2022894bb32fc8dde8','usr/sbin/piano-pd-locator')}
CAMERA_RUNTIME_PATHS={'usr/lib/piano/piano-camerad','usr/bin/piano-camera-ctl',
 'usr/share/licenses/piano-camera/LICENSE-MIT',
 'usr/share/licenses/piano-quad/LICENSE-MIT',
 'usr/share/licenses/demosaic/LICENSE-MIT'}
PEN_SOURCE_FILES=('Makefile','piano-pen-frame.c','piano-pen-frame.h',
 'piano-pen-core.cpp','piano-pen-core.h','piano-pen-decoder.cpp',
 'piano-pen-decoder.hpp','piano-pen-offline.cpp','piano-pen-owner.c',
 'piano-pen-owner.h','Apache-2.0.txt','OPEN_CORE_ORIGIN.txt')
PEN_DOCUMENTS=('Apache-2.0.txt','OPEN_CORE_ORIGIN.txt')
PEN_RUNTIME_PATHS={'usr/bin/piano-pen-offline','usr/bin/piano-pen-canvas','usr/lib/piano/pen-bluetooth','usr/lib/piano/touch-input','usr/share/piano/pen/p81c-radio.json'}|{
 'usr/share/doc/piano-pen-offline/'+name for name in PEN_DOCUMENTS}
TOOL_PINS={'clang':'939a882527432ec23b094c289e7f170bf2d6ec282e74dde75e31b08602fa3eae',
 'ld.lld':'57b6c64db534793f05918a6e935c900e9938bd64d0ac95933285930b568387ea',
 'llvm-strip':'629062ddc62f936d7f07418099d202850e18b222217a85f419552fa25d3eb4ec'}
QEMU_PIN='73cc2584f119f9e74c85c0a8549dbb6fd104ae3897a06a7500797f9e5b4d0943'
ALSA_TOOLCHAIN_PIN='09676a0fc68ee9cd5ba6684646b7e82c96e2ed5ac7ba53d639c4fe53b0612346'
TOPOLOGY_PIN=json.loads((ROOT/'config/release.json').read_text())['audio_topology']['sha256']
DEFAULT_RELEASE='7.2.6-piano-gnome-00061-g352508459733'
TOUCH_DIAGNOSTICS_PATCH=ROOT/'tools/patches/piano-touch-view-observability.patch'
TOUCH_CAPTURE_PATCH=ROOT/'tools/patches/piano-touch-view-raw-capture.patch'
TOUCH_PEN_PATCH=ROOT/'tools/patches/piano-touch-view-pen-owner.patch'
TOUCH_EPOCH_PATCH=ROOT/'tools/patches/piano-touch-view-epoch-reset.patch'
TOUCH_LINE_NOISE_PATCH=ROOT/'tools/patches/piano-touch-view-line-noise.patch'
TOUCH_PALM_PATCH=ROOT/'tools/patches/piano-touch-view-palm-rejection.patch'
LOOPBACK_PATCH_SERIES=ROOT/'patches/v4l2loopback/0.15.4/series.json'


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


def build_topology(public, macros, destination, env=None):
    policy=json.loads((ROOT/'config/release.json').read_text())['audio_topology']
    source=ROOT/policy['source']
    config=destination/'piano.conf'
    target=destination/'qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin'
    target.parent.mkdir(parents=True,exist_ok=True)
    with config.open('w') as output:
        subprocess.run(['m4','-I',public/'topology','-I',macros,source],
                       stdout=output,env=env,check=True)
    subprocess.run(['alsatplg','-c',config,'-o',target],env=env,check=True)
    if sha(target)!=policy['sha256']:
        raise ValueError('Product topology differs from its recorded source build')
    return target


def source_patches(series):
    """Read the shared package patch list used by runtime and Nix builds."""
    patches=[]
    for entry in json.loads(series.read_text())['patches']:
        path=series.parent/entry['file']
        if sha(path)!=entry['sha256']:
            raise ValueError('Package patch checksum differs: '+str(path))
        patches.append(path)
    return patches


def entry_source(name):
    if name not in PUBLIC_SOURCES:raise ValueError('Unknown runtime helper')
    signature='int PianoOriginalMain(int,char **);'if name=='piano-touch-view'else'int PianoOriginalMain(void);'
    normal='return PianoOriginalMain(argc,argv);'if name=='piano-touch-view'else'if(argc!=1){fprintf(stderr,"Use --help or no arguments.\\n");return 2;} return PianoOriginalMain();'
    description=' Optional --diagnostics N emits touch JSON; --capture FILE saves this reader\'s complete raw records with --capture-seconds N (1..10, default5, max16MiB). Both default off.'if name=='piano-touch-view'else''
    if name=='piano-touch-view':description+=' --pen-ini ACTUAL.ini --pen-pressure-max-age-ms N enables the calibrated pen owner; add --pen-input in input mode for tablet-tool events, or --pen-json for diagnostics. No pen options means no pen/HID setup.'
    return '#include <stdio.h>\n#include <string.h>\n'+signature+'\nint main(int argc,char **argv){if(argc==2 && !strcmp(argv[1],"--help")){puts("'+name+': Linux Piano runtime helper; --help performs no device access.'+description+'");return 0;}'+normal+'}\n'


def derive_touch_source(public, output):
    """Apply diagnostics/capture and the pen bridge to one public source copy."""
    relative,pin,_=PUBLIC_SOURCES['piano-touch-view'];original=Path(public)/relative
    if sha(original)!=pin:raise ValueError('Public touch source changed')
    folder=Path(output)/'piano-touch-source';target=folder/relative
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,target)
    patches=[]
    for patch in (TOUCH_DIAGNOSTICS_PATCH,TOUCH_CAPTURE_PATCH,TOUCH_PEN_PATCH,TOUCH_EPOCH_PATCH,TOUCH_LINE_NOISE_PATCH,TOUCH_PALM_PATCH):
        if patch in (TOUCH_PEN_PATCH,TOUCH_PALM_PATCH):
            subprocess.run(['patch','--batch','--forward','--fuzz=0',
                            '--no-backup-if-mismatch','-p1','-i',str(patch)],cwd=folder,check=True)
        else:
            for options in (['--check'],[]):
                subprocess.run(['git','apply','--no-index','--unidiff-zero',*options,str(patch)],cwd=folder,check=True)
        patches.append({'file':str(patch.relative_to(ROOT)),'sha256':sha(patch)})
    return target,{'public_source_sha256':pin,'patch_sha256':sha(TOUCH_DIAGNOSTICS_PATCH),
                  'patches':patches,'effective_source_sha256':sha(target)}


def camera_inputs():
    """Record the one maintained camera source tree, not a generated patch stack."""
    source = ROOT / 'linux/userspace/camera'
    rows = {}
    for folder, directories, files in os.walk(source):
        directories[:] = sorted(name for name in directories if name not in ('vendor', 'target'))
        for name in sorted(files):
            path = Path(folder) / name
            rows[str(path)] = sha(path)
    series = ROOT / 'patches/demosaic/0.3.0/series.json'
    for path in [series, *source_patches(series)]:
        rows[str(path)] = sha(path)
    rows[str(Path(__file__).resolve())] = sha(Path(__file__).resolve())
    return rows


def prepare_camera_quad(source, work, env):
    """Materialize the fixed, patched dependency in this build directory."""
    import urllib.request

    series = ROOT / 'patches/demosaic/0.3.0/series.json'
    item = json.loads(series.read_text())['source']
    name = item['pname'] + '-' + item['version']
    archive = ROOT / 'build/source-cache/camera' / (name + '.crate')
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        temporary = archive.with_suffix('.download')
        url = 'https://static.crates.io/crates/' + item['pname'] + '/' + name + '.crate'
        with urllib.request.urlopen(url, timeout=30) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        if sha(temporary) != item['sha256']:
            raise ValueError('Camera dependency download checksum differs: ' + name)
        temporary.replace(archive)
    if sha(archive) != item['sha256']:
        raise ValueError('Camera dependency archive checksum differs: ' + str(archive))
    run(['tar', '-xf', archive, '-C', work], env, work / 'build.log')
    dependency = work / name
    for patch in source_patches(series):
        run(['patch', '--batch', '--forward', '--fuzz=0', '-p1',
             '--directory', dependency, '--input', patch], env, work / 'build.log')
    vendor = source / 'quad/vendor'
    vendor.mkdir()
    (vendor / item['pname']).symlink_to(dependency.resolve(), target_is_directory=True)
    return dependency


def build_camera(output, cc_flags, cxx_flags, uapi, sysroot=None, env=None,
                 emulator=None, linker_flags=()):
    """Use the same camera Makefile as Nix; install no global files."""
    output = Path(output).resolve()
    work = output / 'camera'
    source, built = work / 'source', work / 'bin'
    build_env = (os.environ if env is None else env).copy()
    inputs = camera_inputs()
    shutil.copytree(ROOT / 'linux/userspace/camera', source,
                    ignore=shutil.ignore_patterns('vendor', 'target'))
    dependency = prepare_camera_quad(source, work, build_env)
    if sysroot:
        build_env['PKG_CONFIG_SYSROOT_DIR'] = str(sysroot)
        build_env['PKG_CONFIG_LIBDIR'] = os.pathsep.join(str(Path(sysroot) / path) for path in (
            'usr/lib/aarch64-linux-gnu/pkgconfig', 'usr/lib/pkgconfig', 'usr/share/pkgconfig'))
    # Camera uses the project's normal warning policy. Other existing helper
    # builds retain their own flags; do not make old baseline warnings fatal.
    flags = [str(flag) for flag in cc_flags[1:] if flag != '-Werror']
    command = ['make', '-C', source, 'OUT=' + str(built),
               'CC=' + shlex.join(map(str, cc_flags)), 'CXX=' + shlex.join(map(str, cxx_flags)),
               'CFLAGS=' + shlex.join(flags),
               'CPPFLAGS=' + shlex.join(['-isystem', str(Path(uapi) / 'include')]),
               'LDFLAGS=' + shlex.join(['-static', *linker_flags]),
               'PKG_CONFIG=pkg-config --static',
               # Debian's libyuv-dev supplies no .pc file. The compiler
               # already selects the target sysroot and its standard paths.
               'YUV_CFLAGS=', 'YUV_LIBS=-lyuv']
    command.append('CARGO_TARGET=aarch64-unknown-linux-gnu')
    run(command, build_env, work / 'build.log')
    files = {}
    for name, destination in (('piano-camerad', 'usr/lib/piano/piano-camerad'),
                              ('piano-camera-ctl', 'usr/bin/piano-camera-ctl')):
        binary = built / name
        row = verify_elf(binary)
        check = ([str(emulator)] if emulator else []) + [str(binary), '--help']
        subprocess.run(check, env=build_env, check=True, timeout=10)
        row.update(file=str(binary.relative_to(output)), mode=0o755,
                   source_kind='project-camera', source_path='linux/userspace/camera',
                   compile_exit_code=0, help_exit_code=0, help_no_device_access=True)
        files[destination] = row
    license = source / 'LICENSE-MIT'
    files['usr/share/licenses/piano-camera/LICENSE-MIT'] = {
        'file': str(license.relative_to(output)), 'mode': 0o644,
        'bytes': license.stat().st_size, 'sha256': sha(license)}
    for license, destination in (
        (source / 'quad/LICENSE-MIT', 'usr/share/licenses/piano-quad/LICENSE-MIT'),
        (dependency / 'LICENSE-MIT', 'usr/share/licenses/demosaic/LICENSE-MIT'),
    ):
        files[destination] = {
            'file': str(license.relative_to(output)), 'mode': 0o644,
            'bytes': license.stat().st_size, 'sha256': sha(license)}
    for path, digest in inputs.items():
        if sha(Path(path)) != digest:
            raise ValueError('Camera build input changed: ' + path)
    return files, {'inputs': inputs, 'build_command': list(map(str, command))}


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


def build_pen_core(output, cc_flags, sysroot=None, env=None, emulator=None):
    """Build the same offline C/C++ sources in both runtime pipelines; no ini input."""
    output=Path(output).resolve();env=os.environ.copy() if env is None else env
    source=ROOT/'linux/userspace/pen';copied=output/'pen-source';built=output/'pen-core'
    copied.mkdir();built.mkdir()
    inputs={str(Path(__file__).resolve()):sha(Path(__file__).resolve())}
    for name in PEN_SOURCE_FILES:
        original=source/name;inputs[str(original)]=sha(original)
        shutil.copyfile(original,copied/name)
    compiler=Path(cc_flags[0]).resolve();flags=list(map(str,cc_flags[1:]))
    if 'clang' in compiler.name:
        cxx=[str(compiler),'--driver-mode=g++',*flags]
    else:
        paired=compiler.with_name(re.sub(r'gcc(?=(?:-\d+)?$)','g++',compiler.name)) if re.search(r'gcc(?:-\d+)?$',compiler.name) else None
        if paired and paired.is_file():
            cxx=[str(paired),*flags]
        else:
            clang=shutil.which('clang',path=env.get('PATH'))
            if not clang:raise ValueError('Pen core needs the matching G++ compiler or Clang with ARM64 C++ development files')
            cxx=[clang,'--driver-mode=g++','--target=aarch64-linux-gnu']
            if sysroot:cxx+=['--sysroot='+str(sysroot),'--gcc-toolchain='+str(Path(sysroot)/'usr')]
            cxx += [f for f in flags if not f.startswith(('--sysroot=','--gcc-toolchain=','--target='))]
    archiver=shutil.which('llvm-ar',path=env.get('PATH')) or shutil.which('ar',path=env.get('PATH'))
    make=shutil.which('make',path=env.get('PATH'))
    if not archiver or not make:raise ValueError('Pen core needs make and llvm-ar or ar')
    toolchain={}
    for name,command in (('cc',[str(compiler)]),('cxx',cxx[:2] if '--driver-mode=g++' in cxx else cxx[:1]),('ar',[archiver]),('make',[make])):
        path=Path(command[0]).resolve();inputs[str(path)]=sha(path)
        toolchain[name]={'path':str(path),'sha256':sha(path),
                         'version':capture([*command,'--version'],env=env).splitlines()[0]}
    # Record the selected target static C++ library and its real headers. Clang
    # uses the supplied GNU sysroot, rather than the host's C++ runtime.
    for library in ('libstdc++.a','libgcc.a','libgcc_eh.a'):
        selected=Path(capture([*cxx,'-print-file-name='+library],env=env))
        if not selected.is_file():raise ValueError('ARM64 static C++ sysroot missing '+library)
        selected=selected.resolve();inputs[str(selected)]=sha(selected)
        toolchain[library]={'path':str(selected),'sha256':sha(selected)}
    if sysroot:
        for folder in (Path(sysroot)/'usr/include/c++',Path(sysroot)/'usr/include/aarch64-linux-gnu/c++'):
            inputs.update({str(folder/name):digest for name,digest in tree_files(folder).items()})
    command=[make,'-C',copied,'OUT='+str(built),
             'CC='+shlex.join([str(compiler),*flags]),'CXX='+shlex.join(cxx),
             'AR='+archiver,'CPPFLAGS=','CFLAGS=','CXXFLAGS=',
             'LDFLAGS=-static','LDLIBS=-lm']
    log=output/'pen-build.log';run(command,env,log)
    binary=built/'piano-pen-offline';item=verify_elf(binary)
    help_result=subprocess.run(([str(emulator)] if emulator else [])+[str(binary),'--help'],
                               env=env,capture_output=True,text=True,timeout=10,check=True)
    item.update(file=str(binary.relative_to(output)),mode=0o755,
                source_kind='project-pen-offline',source_path='linux/userspace/pen',
                source_files={str(Path(path).relative_to(ROOT)):digest for path,digest in inputs.items()
                              if Path(path).is_relative_to(source)},
                compile_exit_code=0,help_exit_code=help_result.returncode,help_no_device_access=True)
    files={'usr/bin/piano-pen-offline':item}
    script=source/'piano-pen-bluetooth.py';inputs[str(script)]=sha(script)
    copied_script=copied/script.name;shutil.copyfile(script,copied_script)
    files['usr/lib/piano/pen-bluetooth']={
        'file':str(copied_script.relative_to(output)),'bytes':copied_script.stat().st_size,
        'sha256':sha(copied_script),'mode':0o755,'source_kind':'project-pen-control',
        'source_path':'linux/userspace/pen/piano-pen-bluetooth.py'}
    profile=ROOT/'linux/bsp/common/usr/share/piano/pen/p81c-radio.json';inputs[str(profile)]=sha(profile)
    copied_profile=copied/profile.name;shutil.copyfile(profile,copied_profile)
    files['usr/share/piano/pen/p81c-radio.json']={
        'file':str(copied_profile.relative_to(output)),'bytes':copied_profile.stat().st_size,
        'sha256':sha(copied_profile),'mode':0o644,'source_kind':'board-pen-radio-profile',
        'source_path':'linux/bsp/common/usr/share/piano/pen/p81c-radio.json'}
    canvas=source/'piano-pen-canvas.py';inputs[str(canvas)]=sha(canvas)
    copied_canvas=copied/canvas.name;shutil.copyfile(canvas,copied_canvas)
    files['usr/bin/piano-pen-canvas']={
        'file':str(copied_canvas.relative_to(output)),'bytes':copied_canvas.stat().st_size,
        'sha256':sha(copied_canvas),'mode':0o755,'source_kind':'project-pen-drawing',
        'source_path':'linux/userspace/pen/piano-pen-canvas.py'}
    frontend=source/'piano-touch-input';inputs[str(frontend)]=sha(frontend)
    copied_frontend=copied/frontend.name;shutil.copyfile(frontend,copied_frontend)
    files['usr/lib/piano/touch-input']={
        'file':str(copied_frontend.relative_to(output)),'bytes':copied_frontend.stat().st_size,
        'sha256':sha(copied_frontend),'mode':0o755,'source_kind':'project-touch-input',
        'source_path':'linux/userspace/pen/piano-touch-input'}
    for name in PEN_DOCUMENTS:
        path=copied/name
        files['usr/share/doc/piano-pen-offline/'+name]={
            'file':str(path.relative_to(output)),'bytes':path.stat().st_size,'sha256':sha(path),'mode':0o644}
    for path,digest in inputs.items():
        if sha(Path(path))!=digest:raise ValueError('Pen build input changed: '+path)
    archive=built/'libpiano-pen-core.a'
    record={'inputs':inputs,'toolchain':toolchain,'build_command':list(map(str,command)),
            'cxx_command':cxx,
            'archive':{'file':str(archive.relative_to(output)),'bytes':archive.stat().st_size,
                       'sha256':sha(archive),'installed':False},
            'ini_required_at_build':False,'device_operation':False,'drawing_input_enabled':False}
    (output/'pen-manifest.json').write_text(json.dumps(record,indent=2)+'\n')
    return files,record


def build_touch_binary(output, cc_flags, uapi, source, pen_record, env=None, linker_flags=()):
    """Keep the public touch/entry C ABI; link the shared C++ pen archive last."""
    output=Path(output);env=os.environ.copy() if env is None else env
    entry=output/'piano-touch-view-entry.c';entry.write_text(entry_source('piano-touch-view'))
    obj=output/'piano-touch-view.o';entry_obj=output/'piano-touch-view-entry.o'
    binary=output/'piano-touch-view';log=output/'build.log'
    archive=output/pen_record['archive']['file']
    run([*cc_flags,'-isystem',Path(uapi)/'include','-I',output/'pen-source',
         '-Dmain=PianoOriginalMain','-c',source,'-o',obj],env,log)
    # A C++ link driver must not compile the C wrapper: its external entry
    # would otherwise be mangled and fail to match PianoOriginalMain.
    run([*cc_flags,'-c',entry,'-o',entry_obj],env,log)
    run([*pen_record['cxx_command'],*linker_flags,'-static',entry_obj,obj,archive,
         '-lm','-o',binary],env,log)
    row=verify_elf(binary)
    row.update(entry_sha256=sha(entry),entry_compile_language='c',final_link_language='c++',
               pen_archive_sha256=pen_record['archive']['sha256'],
               pen_source_files={str(Path(path).relative_to(ROOT)):digest
                                 for path,digest in pen_record['inputs'].items()
                                 if Path(path).is_relative_to(ROOT/'linux/userspace/pen')})
    return row


def stage(output,destination):
    if not destination.resolve().is_relative_to(ROOT/'build/distros'):raise ValueError('Stage only into a derived workspace distro')
    manifest=json.loads((output/'manifest.json').read_text());files=manifest['runtime_files'];release=manifest['kernel']['release']
    if not re.fullmatch(r'[a-zA-Z0-9_.+-]{1,128}',release):raise ValueError('Unsafe runtime kernel release')
    expected={row[2]for row in PUBLIC_SOURCES.values()}|PEN_RUNTIME_PATHS|CAMERA_RUNTIME_PATHS|{'usr/lib/firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin','usr/lib/modules/'+release+'/updates/v4l2loopback.ko'}
    if manifest.get('fingerprint'):
        from build_piano_fingerprint import RUNTIME_PATHS
        expected |= RUNTIME_PATHS
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
    record={'status':'REAL_RUNTIME_STAGED_NOT_DEVICE_TESTED','bundle_manifest_sha256':sha(output/'manifest.json'),'kernel':manifest['kernel'],'files':files,'pen_core':manifest['pen_core'],'device_tested':False}
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
    inputs[str(TOUCH_PEN_PATCH)]=sha(TOUCH_PEN_PATCH)
    inputs[str(TOUCH_EPOCH_PATCH)]=sha(TOUCH_EPOCH_PATCH)
    for patch in [LOOPBACK_PATCH_SERIES,*source_patches(LOOPBACK_PATCH_SERIES)]:
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
    for folder in (public/'topology',args.macros.resolve()/'audioreach'):
        inputs.update({str(folder/name):digest for name,digest in tree_files(folder).items()})
    inputs[str(public/'scripts/build-topology.sh')]=sha(public/'scripts/build-topology.sh')
    inputs.update(identity['inputs']);output.mkdir(parents=True);log=output/'build.log';files={};commands=[]
    flags=[tools/'clang','--target=aarch64-linux-gnu','--sysroot='+str(sysroot),'--gcc-toolchain='+str(sysroot/'usr'),'-O2','-Wall','-Wextra','-Werror']
    touch_source,touch_provenance=derive_touch_source(public,output)
    pen_files,pen_record=build_pen_core(output,flags,sysroot,env,qemu)
    files.update(pen_files);inputs.update(pen_record['inputs'])
    camera_files,camera_record=build_camera(output,flags,pen_record['cxx_command'],uapi,sysroot,env,qemu,('-fuse-ld=lld',))
    files.update(camera_files);inputs.update(camera_record['inputs'])
    for name,(path,pin,destination)in PUBLIC_SOURCES.items():
        entry=output/(name+'-entry.c');entry.write_text(entry_source(name));obj=output/(name+'.o');binary=output/name
        effective_source=touch_source if name=='piano-touch-view'else public/path
        if name=='piano-touch-view':
            row=build_touch_binary(output,flags,uapi,effective_source,pen_record,env,('-fuse-ld=lld',))
        else:
            run([*flags,'-isystem',uapi/'include','-Dmain=PianoOriginalMain','-c',effective_source,'-o',obj],env,log)
            run([*flags,'-fuse-ld=lld','-static',entry,obj,'-lm','-o',binary],env,log)
            row=verify_elf(binary)
        help_result=subprocess.run([str(qemu),str(binary),'--help'],capture_output=True,text=True,timeout=10,check=True)
        row.update({'file':name,'mode':0o755,'source_sha256':pin,'entry_sha256':sha(entry),'compile_exit_code':0,'help_exit_code':help_result.returncode,'help_no_device_access':True});files[destination]=row
        if name=='piano-touch-view':row.update(touch_provenance)
    wrapper=output/'host-bin';wrapper.mkdir();exe=wrapper/'alsatplg';launch=[alsa['loader'],'--library-path',str(alsa_root/'usr/lib/x86_64-linux-gnu'),str(alsa_root/'usr/bin/alsatplg')]
    import shlex
    exe.write_text('#!/bin/sh\nexec '+shlex.join(launch)+' "$@"\n');exe.chmod(0o755);top_env=env.copy();top_env['PATH']=str(wrapper)+os.pathsep+top_env['PATH']
    build_topology(public,args.macros.resolve(),output/'firmware',top_env)
    tplg=output/'firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin'
    if sha(tplg)!=TOPOLOGY_PIN:raise ValueError('Public topology differs from validated repeat build')
    run([*launch,'-d',tplg,'-o',output/'topology-decoded.conf'],env,log)
    files['usr/lib/firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin']={'file':tplg.relative_to(output).as_posix(),'bytes':tplg.stat().st_size,'sha256':sha(tplg),'mode':0o644,'decode_exit_code':0,'device_tested':False}
    module_source=output/'v4l2loopback';module_source.mkdir()
    tracked=capture(['git','ls-files','-z'],args.v4l2_source.resolve()).split('\0')
    for name in filter(None,tracked):
        original=args.v4l2_source.resolve()/name;target=module_source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,target);inputs[str(original)]=sha(original)
    for patch in source_patches(LOOPBACK_PATCH_SERIES):
        subprocess.run(['patch','--batch','--forward','--fuzz=0',
                        '--no-backup-if-mismatch','-p1','-i',str(patch)],
                       cwd=module_source,check=True)
    run(['make','-C',args.kernel_build.resolve(),'M='+str(module_source),'ARCH=arm64','LLVM=1','LLVM_IAS=1','LOCALVERSION=','-j4','modules'],env,log)
    module=module_source/'v4l2loopback.ko';run([tools/'llvm-strip','--strip-debug',module],env,log);info=modinfo(module)
    if info.get('name')!='v4l2loopback'or info.get('vermagic','').split()[0]!=args.kernel_release:raise ValueError('External module does not match requested exact kernel')
    files['usr/lib/modules/'+args.kernel_release+'/updates/v4l2loopback.ko']={'file':module.relative_to(output).as_posix(),'bytes':module.stat().st_size,'sha256':sha(module),'mode':0o644,'vermagic':info['vermagic'],'source_commit':LOOP_COMMIT,'strip_debug':True,'loaded':False}
    for name,pin in inputs.items():
        if sha(Path(name))!=pin:raise ValueError('Runtime build input changed: '+name)
    kernel_identity(args.kernel_build.resolve(),source,args.kernel_commit,args.kernel_release)
    manifest={'status':'RUNTIME_COMPILED_NOT_DEVICE_TESTED','public_commit':PUBLIC_COMMIT,'uapi_commit':UAPI_COMMIT,'uapi_sha256':UAPI_DIGEST,'macros_commit':MACROS_COMMIT,'v4l2_commit':LOOP_COMMIT,'kernel':identity,'inputs':inputs,'runtime_files':files,'pen_core':pen_record,'camera':camera_record,'hardware_verified':False,'device_operation':False,'known_topology_limit':'ALSA decoded config does not recompile vendor data; original public config matches known binary SHA and decodes successfully'}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if args.stage_rootfs:stage(output,args.stage_rootfs.resolve())
    return manifest


def parser():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--public-source',type=Path,default=ROOT/'upstream/debian-piano-current');p.add_argument('--uapi',type=Path,default=ROOT/'build/piano-runtime/uapi')
    p.add_argument('--sysroot',type=Path,default=ROOT/'build/distros/piano-runtime-build',
                   help='Prepared ARM64 GNU development sysroot with static libyuv/libjpeg');p.add_argument('--macros',type=Path,default=ROOT/'build/piano-runtime/topology/macros');p.add_argument('--alsa-tools',type=Path,default=ROOT/'build/piano-runtime/topology/host-tools')
    p.add_argument('--v4l2-source',type=Path,default=ROOT/'build/piano-runtime/v4l2loopback');p.add_argument('--kernel-source',type=Path,default=ROOT/'build/kernel-worktrees/piano-full-integration');p.add_argument('--kernel-commit',default=UAPI_COMMIT)
    p.add_argument('--kernel-build',type=Path,default=ROOT/'build/kernels/full-integration');p.add_argument('--kernel-release',default=DEFAULT_RELEASE);p.add_argument('--stage-rootfs',type=Path)
    return p


if __name__=='__main__':
    result=build(parser().parse_args());print(json.dumps({'status':result['status'],'kernel_release':result['kernel']['release'],'runtime_files':len(result['runtime_files'])},indent=2))
