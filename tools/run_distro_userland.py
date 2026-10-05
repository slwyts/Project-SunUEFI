#!/usr/bin/env python3
"""Execute real ARM64 distro programs with workspace QEMU/PRoot in bwrap.

Readonly guest by default. This uses the host kernel, not a Piano kernel boot.
No host-global binfmt registration or installation. --writable only changes the
explicit workspace guest directory; host /usr and runtime helpers stay readonly.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

from prepare_distro_host_tools import inspect

ROOT=Path(__file__).resolve().parents[1]
PROBE='''set -eu
cat /etc/os-release
dpkg --print-architecture
getconf LONG_BIT
getconf GNU_LIBC_VERSION
bash --version | head -n 1
printf piano-arm64 | sha256sum
/bin/bash -c 'printf "%s\\n" child-exec-ok'
'''
PRIVATE_BINFMT_SETUP=r'''
import os,sys,subprocess
# This process already has a private user/mount namespace and subordinate IDs.
# Mask /tmp before creating anything; the host binfmt mount is never used.
subprocess.run(['/usr/bin/mount','-t','tmpfs','tmpfs','/tmp'],check=True)
where='/tmp/piano-private-binfmt'
os.mkdir(where)
subprocess.run(['/usr/bin/mount','-t','binfmt_misc','binfmt_misc',where],check=True)
magic=b'\x7fELF\x02\x01\x01'+b'\0'*9+b'\x02\0\xb7\0'
mask=b'\xff'*7+b'\0'*9+b'\xfe\xff\xff\xff'
escaped=lambda data:''.join('\\x%02x'%byte for byte in data)
rule=':piano-aarch64:M::'+escaped(magic)+':'+escaped(mask)+':'+sys.argv[1]+':F'
with open(where+'/register','w')as stream:stream.write(rule)
with open(where+'/piano-aarch64')as stream:print('PRIVATE_BINFMT '+stream.read().replace('\n',';'),flush=True)
for path in ('/proc/self/uid_map','/proc/self/gid_map'):
 with open(path)as stream:print(path+' '+stream.read().replace('\n',';'),flush=True)
os.execvp(sys.argv[2],sys.argv[2:])
'''


def build_command(rootfs, tools, command, writable=False,backend='proot'):
    rootfs,tools=Path(rootfs).resolve(),Path(tools).resolve()
    if not rootfs.is_relative_to(ROOT) or not tools.is_relative_to(ROOT) or not rootfs.is_dir():
        raise ValueError('Guest and helpers must be workspace directories')
    if writable and rootfs.is_relative_to(ROOT/'artifacts/distros'):
        raise ValueError('Imported distro artifact base is immutable; use a derived build directory')
    bash=(rootfs/'usr/bin/bash').read_bytes()
    if len(bash)<64 or bash[:5]!=b'\x7fELF\x02' or struct.unpack_from('<H',bash,18)[0]!=183:
        raise ValueError('Expected actual ELF64 AArch64 Bash in guest')
    metadata=json.loads((tools/'manifest.json').read_text())
    inspect(tools,metadata)
    if backend not in ('proot','namespace'):raise ValueError('Unknown distro execution backend')
    # Mount the guest as the real namespace root. PRoot 5.1 does not translate
    # every modern QEMU syscall (notably statx); a host /usr root would make
    # existence checks disagree with open(). Native helpers live only under
    # existing, unused guest /opt and /mnt mountpoints.
    args=['bwrap',*(['--unshare-user']if backend=='proot'else []),'--unshare-pid','--unshare-uts','--unshare-ipc',
        '--die-with-parent','--bind' if writable else '--ro-bind',str(rootfs),'/',
        '--ro-bind',str(tools),'/opt',*(['--ro-bind','/usr/lib','/mnt']if backend=='proot'else []),
        '--dev','/dev','--proc','/proc','--tmpfs','/tmp','--chmod','1777','/tmp']
    if backend=='namespace'and writable:
        # Ordinary package ownership and account operations inside the private
        # subordinate-ID userns. No host-global SYS_ADMIN/MODULE/TIME capability.
        for capability in ('CAP_CHOWN','CAP_DAC_OVERRIDE','CAP_FOWNER','CAP_FSETID','CAP_SETUID','CAP_SETGID','CAP_SETFCAP','CAP_SYS_CHROOT'):
            args+=['--cap-add',capability]
    if writable:
        args+=['--ro-bind','/etc/resolv.conf','/etc/resolv.conf']
    args+=['--chdir','/','--',
        *(['/mnt/ld-linux-x86-64.so.2','--library-path','/opt/usr/lib/x86_64-linux-gnu:/mnt',
           '/opt/usr/bin/proot','-0','-q','/opt/usr/bin/qemu-aarch64','-r','/','-w','/']if backend=='proot'else []),
        '/usr/bin/env','-i','HOME=/root','PATH=/usr/sbin:/usr/bin:/sbin:/bin',
        'LC_ALL=C','DEBIAN_FRONTEND=noninteractive',*command]
    if backend=='namespace':
        args=['unshare','--user','--map-root-user','--map-auto','--mount','--fork',
              '/usr/bin/python3','-c',PRIVATE_BINFMT_SETUP,str(tools/'usr/bin/qemu-aarch64'),*args]
    return args


def run(rootfs,tools,command,log,writable=False,timeout=1800,backend='proot'):
    args=build_command(rootfs,tools,command,writable,backend)
    provenance={'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'helper_manifest_sha256':hashlib.sha256((Path(tools)/'manifest.json').read_bytes()).hexdigest()}
    log=Path(log)
    if log.exists():
        raise ValueError('Guest log exists; preserve prior evidence')
    log.parent.mkdir(parents=True,exist_ok=True)
    environment={k:v for k,v in os.environ.items()if k not in ('LD_PRELOAD','LD_LIBRARY_PATH')}
    with log.open('x') as stream:
        result=subprocess.run(args,stdout=stream,stderr=subprocess.STDOUT,env=environment,timeout=timeout)
    record={'status':'HOST_ARM64_USERLAND_EXECUTION_ONLY','exit_code':result.returncode,
        'rootfs':str(Path(rootfs).resolve()),'command':command,'guest_writable':writable,'backend':backend,
        'private_binfmt':backend=='namespace','subordinate_uid_mapping':backend=='namespace',
        'host_global_install':False,'host_binfmt_changed':False,'piano_boot_verified':False,
        'pid1_boot_verified':False,'log':str(log.resolve()),'execution_provenance':provenance,
        'log_sha256':hashlib.sha256(log.read_bytes()).hexdigest()}
    log.with_suffix(log.suffix+'.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs',type=Path,required=True)
    parser.add_argument('--tools',type=Path,default=ROOT/'build/distro-tools')
    parser.add_argument('--log',type=Path,required=True)
    parser.add_argument('--writable',action='store_true')
    parser.add_argument('--backend',choices=('proot','namespace'),default='proot')
    parser.add_argument('command',nargs=argparse.REMAINDER)
    args=parser.parse_args()
    command=args.command
    if command[:1]==['--']:command=command[1:]
    if not command:command=['/usr/bin/bash','-c',PROBE]
    result=run(args.rootfs,args.tools,command,args.log,args.writable,backend=args.backend)
    print(json.dumps(result,indent=2))
    raise SystemExit(result['exit_code'])
