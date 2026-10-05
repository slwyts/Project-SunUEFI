#!/usr/bin/env python3
"""Generate the compiled SimpleInit digest from the real bounded AA64 EFI app."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def digest_header(app):
    app=Path(app)
    if app.is_symlink() or not app.is_file():raise ValueError('Expected a regular built SimpleInit EFI file')
    raw=app.read_bytes()
    if not 4096<=len(raw)<=64*1024*1024 or raw[:2]!=b'MZ':raise ValueError('Invalid product application size or DOS header')
    pe=struct.unpack_from('<I',raw,60)[0]
    if pe>len(raw)-24 or raw[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',raw,pe+4)[0]!=0xaa64:
        raise ValueError('Product SimpleInit must be an AA64 PE application')
    opt=pe+24;optional=struct.unpack_from('<H',raw,pe+20)[0]
    if optional<112 or optional>len(raw)-opt or struct.unpack_from('<H',raw,opt)[0]!=0x20b or struct.unpack_from('<H',raw,opt+68)[0]!=10:
        raise ValueError('Wrong PE optional header or subsystem')
    digest=hashlib.sha256(raw).digest()
    header='// Generated from the real built SimpleInit.efi; do not edit.\n#pragma once\n'
    header+=f'#define PIANO_PRODUCT_SIMPLEINIT_BYTES {len(raw)}U\n'
    header+='STATIC CONST UINT8 mPianoProductSimpleInitSha256[32]={'+','.join(f'0x{x:02x}' for x in digest)+'};\n'
    return header,{'app_bytes':len(raw),'sha256':digest.hex(),'image_bytes':struct.unpack_from('<I',raw,opt+56)[0],
        'trust_scope':'compiled product source identity; runtime rechecks full digest and actual PE parser'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();header,manifest=digest_header(args.app)
    with args.output.open('x') as out:out.write(header)
    print(json.dumps(manifest,indent=2))


if __name__=='__main__':main()
