#!/usr/bin/env python3
"""Pinned Debian amd64 helper extraction in workspace; no guest/global install."""
import argparse
import hashlib
import html
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
PINS={
 'qemu-user':{'filename':'qemu-user_10.0.13+ds-0+deb13u1_amd64.deb','pool':'q/qemu','bytes':71089852,'sha256':'ca6ede739327a20ae5a3498230c8a3a9a072c586e131bc6bcc1201eb1725e336','files':('usr/bin/qemu-aarch64',)},
 'proot':{'filename':'proot_5.1.0-1.3+b1_amd64.deb','pool':'p/proot','bytes':75876,'sha256':'7ca9260edf1ca6666f9b33e1da551638055934d673117acd135ff5367f420744','files':('usr/bin/proot',)},
 'libtalloc2':{'filename':'libtalloc2_2.4.3+samba4.22.11+dfsg-0+deb13u1_amd64.deb','pool':'s/samba','bytes':63644,'sha256':'b4cc8aa74c8b7c05a64efb6577acc955f522a8e39d8f37fe60b1b3fe6634ce96','files':('usr/lib/x86_64-linux-gnu/libtalloc.so.2.4.3','usr/lib/x86_64-linux-gnu/libtalloc.so.2')},
}


def sha(path):
    h=hashlib.sha256()
    with path.open('rb')as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def verify_package(path,pin):
    if not path.is_file()or path.is_symlink()or path.stat().st_size!=pin['bytes']or sha(path)!=pin['sha256']:
        raise ValueError('Pinned Debian package size/SHA mismatch: '+path.name)


def verify_page(raw,pin):
    text=raw.decode('utf-8');plain=re.sub(r'\s+',' ',html.unescape(re.sub('<[^>]+>',' ',text)))
    size=re.search(r'Exact Size\s+(\d+)\s+Byte',plain)
    digest=re.search(r'SHA256 checksum\s+([a-f0-9]{64})',plain)
    if pin['filename']not in plain or not size or int(size[1])!=pin['bytes']or not digest or digest[1]!=pin['sha256']:
        raise ValueError('Official download-page metadata changed; update reviewed package pins')


def fetch(package,pin,folder):
    page='https://packages.debian.org/trixie/amd64/'+package+'/download'
    url='https://deb.debian.org/debian/pool/main/'+pin['pool']+'/'+pin['filename']
    target=folder/pin['filename'];record={'download_page':page,'url':url,**{key:pin[key]for key in ('filename','bytes','sha256')}}
    if target.exists():verify_package(target,pin);return target,record
    with urllib.request.urlopen(page,timeout=30)as response:raw=response.read(262145)
    if len(raw)>262144:raise ValueError('Official package metadata exceeds bound')
    verify_page(raw,pin);record['download_page_sha256']=hashlib.sha256(raw).hexdigest()
    partial=target.with_suffix(target.suffix+'.part');received=0;h=hashlib.sha256()
    print('Downloading '+package+' ('+str(pin['bytes'])+' bytes)',flush=True)
    try:
        with urllib.request.urlopen(url,timeout=60)as response,partial.open('xb')as output:
            if not response.url.startswith('https://'):raise ValueError('Package redirect lost HTTPS')
            while True:
                block=response.read(1024*1024)
                if not block:break
                received+=len(block)
                if received>pin['bytes']:raise ValueError('Package exceeds pinned size')
                output.write(block);h.update(block)
        if received!=pin['bytes']or h.hexdigest()!=pin['sha256']:raise ValueError('Downloaded package size/SHA mismatch')
        partial.rename(target)
    except BaseException:
        partial.unlink(missing_ok=True);raise
    return target,record


def member_path(name):
    if name.startswith('./'):name=name[2:]
    path=PurePosixPath(name)
    if path.is_absolute()or '..'in path.parts or not name or name.startswith('/'):
        raise ValueError('Unsafe Debian data.tar path')
    return path.as_posix()


def extract_needed(package,pin,output):
    """bsdtar reads ar/data.tar; no maintainer script, full package or device node."""
    verify_package(package,pin)
    entries=subprocess.check_output(['bsdtar','-tf',str(package)],text=True).splitlines()
    data=[name for name in entries if re.fullmatch(r'data\.tar\.(xz|gz|zst)',name)]
    if len(data)!=1:raise ValueError('Expected exactly one bounded Debian data archive')
    expected=set(pin['files']);found={};links=[];seen=set()
    process=subprocess.Popen(['bsdtar','-xOf',str(package),data[0]],stdout=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=process.stdout,mode='r|*')as archive:
            for member in archive:
                name=member_path(member.name)
                if name not in expected:continue
                if name in seen:raise ValueError('Duplicate selected Debian member')
                seen.add(name);target=output/name;target.parent.mkdir(parents=True,exist_ok=True)
                if target.exists()or target.is_symlink():raise ValueError('Refusing to overwrite extracted helper: '+name)
                if member.isreg():
                    if member.size<64 or member.size>64*1024*1024:raise ValueError('Selected helper ELF size out of bounds')
                    source=archive.extractfile(member);head=source.read(64)
                    if head[:6]!=b'\x7fELF\x02\x01'or int.from_bytes(head[18:20],'little')!=62:raise ValueError('Not amd64 ELF helper')
                    h=hashlib.sha256();h.update(head);written=len(head)
                    with target.open('xb')as stream:
                        stream.write(head)
                        while written<member.size:
                            block=source.read(min(1024*1024,member.size-written))
                            if not block:raise ValueError('Short selected member')
                            stream.write(block);h.update(block);written+=len(block)
                    target.chmod(0o755 if '/bin/'in name else 0o644)
                    found[name]={'bytes':written,'sha256':h.hexdigest()}
                elif member.issym():
                    leaf=PurePosixPath(member.linkname)
                    sibling=(PurePosixPath(name).parent/leaf).as_posix()
                    if leaf.is_absolute()or len(leaf.parts)!=1 or sibling not in expected:raise ValueError('Unsafe selected helper symlink')
                    links.append((target,member.linkname));found[name]={'symlink':member.linkname}
                else:raise ValueError('Selected helper is not an ordinary file or bounded symlink')
        if process.wait()!=0:raise ValueError('bsdtar data extraction failed')
        if seen!=expected:raise ValueError('Required amd64 helper missing from pinned package')
        for target,link in links:
            if not(target.parent/link).is_file():raise ValueError('Missing selected symlink target')
            target.symlink_to(link)
        return found
    finally:
        process.stdout.close()
        if process.poll()is None:process.terminate();process.wait()


def commands(folder,loader):
    return {'qemu':[str(folder/'usr/bin/qemu-aarch64')],
            'proot':[str(loader),'--library-path',str(folder/'usr/lib/x86_64-linux-gnu'),str(folder/'usr/bin/proot')]}


def inspect(folder,manifest):
    expected={name for pin in PINS.values()for name in pin['files']}
    if set(manifest['files'])!=expected or set(manifest['packages'])!=set(PINS):raise ValueError('Unexpected helper manifest shape')
    for package,pin in PINS.items():
        row=manifest['packages'][package]
        if any(row.get(key)!=pin[key]for key in ('filename','bytes','sha256')):raise ValueError('Helper package identity differs from reviewed pins')
        verify_package(folder/'downloads'/pin['filename'],pin)
    for name,row in manifest['files'].items():
        path=folder/name
        if 'symlink'in row:
            if not path.is_symlink()or path.readlink().as_posix()!=row['symlink']:raise ValueError('Helper symlink changed: '+name)
        elif not path.is_file()or path.is_symlink()or path.stat().st_size!=row['bytes']or sha(path)!=row['sha256']:
            raise ValueError('Extracted helper changed: '+name)
    launch=commands(folder,Path(manifest['loader']));checks={}
    for name,args in launch.items():
        check=subprocess.run([*args,'--version'],capture_output=True,text=True,timeout=15)
        if check.returncode!=0:raise ValueError('Host helper failed to run: '+name+' '+check.stderr)
        checks[name]=check.stdout.strip()
    help_result=subprocess.run([*launch['proot'],'--help'],capture_output=True,text=True,timeout=15)
    if help_result.returncode!=0 or 'proot'not in help_result.stdout.lower():raise ValueError('PRoot help probe failed')
    return {'versions':checks,'proot_help_exit':help_result.returncode,'launch':launch,'arm_guest_executed':False}


def prepare(folder=ROOT/'build/distro-tools'):
    # Deliberately fixed workspace location. No arbitrary extraction into host.
    if folder.resolve()!=ROOT/'build/distro-tools':raise ValueError('Helper output must be workspace build/distro-tools')
    folder.mkdir(parents=True,exist_ok=True);marker=folder/'manifest.json'
    if marker.exists():
        manifest=json.loads(marker.read_text());result=inspect(folder,manifest);return manifest,result
    if (folder/'usr').exists():raise ValueError('Incomplete helper extraction present; inspect it before retrying')
    downloads=folder/'downloads';downloads.mkdir(exist_ok=True);records={};files={}
    for name,pin in PINS.items():
        _,record=fetch(name,pin,downloads);records[name]=record
    loader=Path('/lib64/ld-linux-x86-64.so.2').resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='.stage-',dir=folder)as temporary:
        staging=Path(temporary)
        for name,pin in PINS.items():files.update(extract_needed(downloads/pin['filename'],pin,staging))
        (staging/'usr').rename(folder/'usr')
    manifest={'status':'WORKSPACE_HOST_HELPERS_VERIFIED','architecture':'amd64','packages':records,'files':files,'loader':str(loader),'global_install':False,'host_config_changed':False,'arm_guest_executed':False}
    result=inspect(folder,manifest);manifest['validation']=result
    marker.write_text(json.dumps(manifest,indent=2)+'\n');return manifest,result


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    _,result=prepare();print(json.dumps(result,indent=2))


if __name__=='__main__':main()
