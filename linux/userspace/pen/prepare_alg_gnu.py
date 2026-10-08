#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Create a pinned ALG GNU-linkage copy; never execute or edit the source blob."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

PIN='26f86f74781e70d03958271b100b31865d3eb80b69f30774ce1b1e25cb24f1c9'
UND=['__cxa_finalize','__cxa_atexit','__stack_chk_fail','clock_gettime','__vsnprintf_chk',
     'memcpy','memset','pthread_mutex_lock','pthread_mutex_unlock','printf','__memcpy_chk',
     'calloc','free','vsnprintf','__memset_chk','atan2','atan','puts','putchar','exit','fopen',
     'fgets','__strlen_chk','fclose','malloc','pow','qsort','__sF','fwrite','pthread_mutex_init',
     'pthread_mutex_destroy','memmove']
RENAME={'pthread_mutex_lock':'piano_mutex_lock','pthread_mutex_unlock':'piano_mutex_unlock',
        'pthread_mutex_init':'piano_mutex_init','pthread_mutex_destroy':'piano_mutex_destroy',
        '__strlen_chk':'pstrlen_chk','__sF':'pSF','fwrite':'pfwr'}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    source=a.source.resolve();output=a.output.resolve();original=source.read_bytes()
    if hashlib.sha256(original).hexdigest()!=PIN:raise ValueError('pinned original ALG SHA differs')
    if output==source or output.exists() or output.is_relative_to(source.parent):
        raise ValueError('use a new output outside the original capture directory')
    b=bytearray(original)
    if b[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',b,18)[0]!=183:
        raise ValueError('expected pinned ELF64 AArch64')
    sho=struct.unpack_from('<Q',b,40)[0];shstep,shcount=struct.unpack_from('<HH',b,58)
    sections=[struct.unpack_from('<II4QII2Q',b,sho+i*shstep) for i in range(shcount)]
    dynsym=next(s for s in sections if s[1]==11);strings=sections[dynsym[6]]
    st,stsize=strings[4:6]
    def name(off):return bytes(b[st+off:b.index(0,st+off)]).decode()
    imports=[];referenced=[]
    for pos in range(dynsym[4],dynsym[4]+dynsym[5],dynsym[9]):
        no,info,other,section,value,size=struct.unpack_from('<IBBHQQ',b,pos)
        if no:referenced.append((no,name(no)))
        if section==0 and no:imports.append((no,name(no)))
    if [n for _,n in imports]!=UND:raise ValueError('actual32-import ABI changed')
    version=next(s for s in sections if s[1]==0x6fffffff)
    for pos in range(version[4],version[4]+version[5],2):
        v=struct.unpack_from('<H',b,pos)[0]
        if v not in (0,1,2,3):raise ValueError('unexpected symbol version')
        if v in (2,3):struct.pack_into('<H',b,pos,1)
    for s in sections:
        if s[1]==9:raise ValueError('unsupported REL section')
        if s[1] in (0x60000001,0x60000002):
            raise ValueError('Android packed relocations require another loader')
    # ELF SHT_RELA is4. All pinned relocations are ordinary AArch64 RELA.
    for s in sections:
        if s[1]==4:
            for pos in range(s[4],s[4]+s[5],s[9]):
                _,info,_=struct.unpack_from('<QQq',b,pos)
                if (info&0xffffffff) not in (257,1025,1026,1027):
                    raise ValueError('unsupported AArch64 relocation')
    dynamic=next(s for s in sections if s[1]==6)
    entries=[struct.unpack_from('<qQ',b,pos) for pos in range(dynamic[4],dynamic[4]+dynamic[5],16)]
    entries=entries[:next(i for i,e in enumerate(entries) if e[0]==0)]
    needed=[v for t,v in entries if t==1]
    if [name(v) for v in needed]!=['libdl.so','liblog.so','libc.so','libm.so']:
        raise ValueError('actual dependencies changed')
    start,end=min(needed),max(v+len(name(v))+1 for v in needed)
    if any(start<=off<end for off,_ in referenced):raise ValueError('dependency pool aliases a symbol')
    names=['libdl.so.2','libc.so.6','libm.so.6'];pool=b'';new_offsets=[]
    for n in names:new_offsets.append(start+len(pool));pool+=n.encode()+b'\0'
    if len(pool)>end-start:raise ValueError('GNU names exceed pinned dependency pool')
    b[st+start:st+end]=pool+b'\0'*(end-start-len(pool))
    for off,old in imports:
        if old in RENAME:
            new=RENAME[old]
            if len(new)>len(old):raise ValueError('rename needs ELF expansion')
            if any(off<x<off+len(old)+1 for x,_ in referenced):raise ValueError('symbol suffix alias')
            b[st+off:st+off+len(old)+1]=new.encode()+b'\0'*(len(old)+1-len(new))
    soname=next(v for t,v in entries if t==14);old=name(soname);new='PianoPenAlg.gnu.so'
    if len(new)>len(old):raise ValueError('SONAME expansion')
    b[st+soname:st+soname+len(old)+1]=new.encode()+b'\0'*(len(old)+1-len(new))
    # Remove obsolete Bionic version requirements and unused liblog dependency.
    # VERSYM must leave with VERNEED: GNU ld.so dereferences l_versions for
    # each remaining symbol index, even global1, but no table is then built.
    kept=[(1,v) for v in new_offsets]+[(t,v) for t,v in entries
                                     if t not in (1,0x6ffffff0,0x6ffffffe,0x6fffffff)]
    for i in range(dynamic[5]//16):
        struct.pack_into('<qQ',b,dynamic[4]+16*i,*(kept[i] if i<len(kept) else (0,0)))
    for s in sections:
        if s[2]&4 and bytes(b[s[4]:s[4]+s[5]])!=original[s[4]:s[4]+s[5]]:
            raise ValueError('executable instructions changed')
    output.parent.mkdir(parents=True,exist_ok=True);output.write_bytes(b)
    result={'original_sha256':PIN,'derived_sha256':hashlib.sha256(b).hexdigest(),
            'original_bytes':len(original),'derived_bytes':len(b),'needed':names,
            'actual_undefined_symbols':UND,'symbol_renames':RENAME,
            'libc_versions_removed':['libc.so:LIBC(index2)','libm.so:LIBC(index3)'],
            'android_packed_relocations':False,'executable_code_changed':False,
            'original_modified':False,'vendor_code_executed':False}
    output.with_suffix(output.suffix+'.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
