#!/usr/bin/env python3
"""Decode verified local NT36532 module layouts; no device or hardware access."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def elf_sections(data):
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected little-endian ELF64')
    start = struct.unpack_from('<Q', data, 40)[0]
    size, count, strings = struct.unpack_from('<HHH', data, 58)
    if size != 64 or start + size * count > len(data):
        raise ValueError('Invalid ELF sections')
    rows = [struct.unpack_from('<IIQQQQIIQQ', data, start + i * size) for i in range(count)]
    names = data[rows[strings][4]:rows[strings][4]+rows[strings][5]]
    result = {}
    for i, row in enumerate(rows):
        name = names[row[0]:].split(b'\0', 1)[0].decode()
        result[name] = {'index': i, 'row': row, 'data': data[row[4]:row[4]+row[5]]}
    return result, rows


class Btf:
    def __init__(self, raw, base=None):
        magic, version, flags, hdr, toff, tlen, soff, slen = struct.unpack_from('<HBB5I', raw)
        if magic != 0xEB9F or version != 1 or flags != 0 or hdr < 24:
            raise ValueError('Unexpected BTF header')
        if hdr + max(toff+tlen, soff+slen) > len(raw):
            raise ValueError('Truncated BTF')
        self.base = base
        self.strings = raw[hdr+soff:hdr+soff+slen]
        self.string_start = base.string_start + len(base.strings) if base else 0
        self.type_start = base.type_start + len(base.types) if base else 1
        self.types = []
        cursor, end = hdr+toff, hdr+toff+tlen
        while cursor < end:
            name, info, value = struct.unpack_from('<III', raw, cursor)
            kind, vlen = (info >> 24) & 31, info & 0xFFFF
            extra = {1:4, 2:0, 3:12, 4:12*vlen, 5:12*vlen,
                     6:8*vlen, 7:0, 8:0, 9:0, 10:0, 11:0,
                     12:0, 13:8*vlen, 14:4, 15:12*vlen, 16:0,
                     17:4, 18:0, 19:12*vlen}.get(kind)
            if extra is None or cursor+12+extra > end:
                raise ValueError(f'Invalid BTF kind/length: {kind}')
            payload = raw[cursor+12:cursor+12+extra]
            self.types.append({'id':self.type_start+len(self.types), 'name':self.name(name),
                               'kind':kind, 'vlen':vlen, 'value':value,
                               'kflag':bool(info >> 31), 'payload':payload})
            cursor += 12 + extra

    def name(self, offset):
        if self.base and offset < self.string_start:
            return self.base.name(offset)
        offset -= self.string_start
        if not 0 <= offset < len(self.strings):
            raise ValueError('Invalid BTF string offset')
        end = self.strings.find(b'\0',offset)
        if end < 0:
            raise ValueError('Unterminated BTF string')
        return self.strings[offset:end].decode()

    def type(self, ident):
        if self.base and ident < self.type_start:
            return self.base.type(ident)
        return self.types[ident-self.type_start]

    def scalar_size(self, ident):
        for _ in range(32):
            t = self.type(ident)
            if t['kind'] in (8,9,10,11,18):
                ident=t['value']
            elif t['kind'] in (1,6,16,19):
                return t['value']
            else:
                return None
        raise ValueError('BTF type loop')

    def members(self, t):
        for i in range(t['vlen']):
            name, ident, bits = struct.unpack_from('<III', t['payload'], i*12)
            if t['kflag']:
                width, bits = bits >> 24, bits & 0xFFFFFF
            else:
                width = 0
            yield {'name':self.name(name), 'type_id':ident, 'bit_offset':bits,
                   'bitfield_width':width, 'scalar_bytes':self.scalar_size(ident)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--capture',type=Path,default=Path('private/captures/touch-test24'))
    ap.add_argument('--output',type=Path,default=Path('private/analysis/nt36532-layout.json'))
    args = ap.parse_args()
    manifest=json.loads((args.capture/'manifest.json').read_text())
    inputs={}
    for name in ('nt36532_touch.ko','vmlinux.btf'):
        raw=(args.capture/name).read_bytes()
        if hashlib.sha256(raw).hexdigest()!=manifest['files'][name]['sha256']:
            raise ValueError('Capture hash mismatch: '+name)
        inputs[name]=raw
    sections, rows=elf_sections(inputs['nt36532_touch.ko'])
    btf=Btf(sections['.BTF']['data'],Btf(inputs['vmlinux.btf']))
    layout=next(t for t in btf.types if t['name']=='nvt_ts_mem_map' and t['kind']==4)
    symsec=sections['.symtab'];sr=symsec['row'];strings=rows[sr[6]]
    names=inputs['nt36532_touch.ko'][strings[4]:strings[4]+strings[5]]
    maps={}
    for offset in range(0,len(symsec['data']),24):
        n,info,other,section,value,size=struct.unpack_from('<IBBHQQ',symsec['data'],offset)
        name=names[n:].split(b'\0',1)[0].decode()
        if name not in ('NT36532_cascade_memory_map','NT36532_single_memory_map'):
            continue
        if size != layout['value']:
            raise ValueError('Symbol and BTF layout sizes differ')
        row=rows[section];raw=inputs['nt36532_touch.ko'][row[4]+value:row[4]+value+size]
        def decode_field(ident,start,depth=0):
            if depth>8:raise ValueError('Nested type limit exceeded')
            length=btf.scalar_size(ident)
            if length in (1,2,4,8):
                if start+length>len(raw):raise ValueError('Member exceeds memory map')
                return f'0x{int.from_bytes(raw[start:start+length],"little"):08X}'
            t=btf.type(ident)
            while t['kind'] in (8,9,10,11,18):t=btf.type(t['value'])
            if t['kind']!=4:raise ValueError('Unsupported type in memory map')
            decoded={}
            for member in btf.members(t):
                if member['bit_offset']%8 or member['bitfield_width']:
                    raise ValueError('Unsupported bitfield in memory map')
                decoded[member['name']]=decode_field(member['type_id'],start+member['bit_offset']//8,depth+1)
            return decoded
        decoded=decode_field(layout['id'],0)
        maps[name]=decoded
    if len(maps)!=2:raise ValueError('Missing NT36532 maps')
    result={'panel':manifest['panel'],'chip_trim':manifest['chip_trim'],
            'active_firmware':manifest['active_firmware'],
            'module_sha256':manifest['files']['nt36532_touch.ko']['sha256'],
            'struct_size':layout['value'],'maps':maps,'hardware_operations_performed':False}
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'struct_size':layout['value'],'maps':{n:{k:v for k,v in d.items()
          if any(s in k for s in ('EVENT_BUF','BOOT_RDY','POR_CD','CRC_EN','SW_RST'))}
          for n,d in maps.items()},'path':str(args.output)},indent=2))


if __name__=='__main__':
    main()
