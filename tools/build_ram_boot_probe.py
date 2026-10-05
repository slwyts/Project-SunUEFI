#!/usr/bin/env python3
"""Build a small returning AA64 EFI application; no prepare or device commands."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(output):
    output = Path(output)
    if output.exists():
        raise ValueError('Output directory already exists; refusing to overwrite a probe')
    source = ROOT/'bootprofiles/uefi-app/PianoRamBootProbe.c'
    header = source.with_suffix('.h')
    includes = ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
    host = ROOT/'build/host-tools/usr'
    env = {**os.environ, 'LD_LIBRARY_PATH': str(host/'lib')}
    output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix='probe-build-') as temporary:
        obj = Path(temporary)/'probe.obj'
        binary = output/'PianoRamBootProbe.efi'
        compile_cmd = [str(host/'bin/clang'), '--target=aarch64-windows-msvc',
            '-O2', '-ffreestanding', '-fshort-wchar', '-fno-stack-protector', '-fno-builtin',
            '-Wall', '-Wextra', '-Werror', '-I', str(includes), '-I', str(includes/'AArch64'),
            '-c', str(source), '-o', str(obj)]
        link_cmd = [str(host/'bin/lld-link'), '/machine:arm64', '/dll',
            '/entry:PianoRamBootProbeEntry', '/subsystem:efi_application', '/nodefaultlib',
            '/base:0', '/timestamp:0', '/filealign:512', '/out:'+str(binary), str(obj)]
        subprocess.run(compile_cmd, env=env, check=True)
        subprocess.run(link_cmd, env=env, check=True)
        # Verify the actual parser accepts the product; not just file magic.
        inspector = Path(temporary)/'inspect'
        subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror',
            '-I',str(includes),'-I',str(includes/'X64'),
            str(ROOT/'tools/inspect_fastboot_boot.c'),'-o',str(inspector)],check=True)
        parsed = subprocess.run([str(inspector),str(binary)],check=True,capture_output=True,text=True).stdout
        raw = binary.read_bytes()
        pe = struct.unpack_from('<I',raw,60)[0]
        opt = pe+24
        relocation_rva, relocation_bytes = struct.unpack_from('<II',raw,opt+112+5*8)
        if not relocation_rva or relocation_bytes < 12:
            raise ValueError('Probe lacks a real base relocation')
        info = {'status':'BUILT_NOT_DEVICE_EXECUTED','file':binary.name,'bytes':len(raw),
            'sha256':sha(binary),'source_sha256':sha(source),'header_sha256':sha(header),
            'machine':'AA64','subsystem':'EFI_APPLICATION','entry_rva':hex(struct.unpack_from('<I',raw,opt+16)[0]),
            'image_bytes':struct.unpack_from('<I',raw,opt+56)[0],
            'relocation_rva':hex(relocation_rva),'relocation_bytes':relocation_bytes,
            'variable_attributes':2,'nonvolatile_variable_writes':False,'block_or_device_io':False,
            'compiler':subprocess.check_output([str(host/'bin/clang'),'--version'],env=env,text=True).splitlines()[0],
            'linker_sha256':sha(host/'bin/lld-link'),'compiler_sha256':sha((host/'bin/clang').resolve()),
            'parser_output':parsed}
        (output/'manifest.json').write_text(json.dumps(info,indent=2)+'\n')
        return info


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    print(json.dumps(build(args.output),indent=2))


if __name__=='__main__':main()
