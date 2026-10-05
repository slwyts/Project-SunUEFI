#!/usr/bin/env python3
"""Select CRC-valid SMMU diagnostic mirrors; preserve the original capture."""
import argparse
import json
import re
import zlib
from pathlib import Path


def decode(raw):
    prefixes=list(re.finditer(rb'SUNUEFI_SMMU_OWNED_DIAG(?:_COPY)? ',raw))
    records={};invalid=0;unsealed=0
    for index,prefix in enumerate(prefixes):
        end=prefixes[index+1].start() if index+1<len(prefixes) else len(raw)
        segment=raw[prefix.end():min(end,prefix.end()+900)]
        match=re.match(rb'([^\r\n]{1,800}?) crc32=([0-9A-Fa-f]{8})(?:\r?\n|$)',segment)
        if match is None:unsealed+=1;continue
        body=match[1]
        if zlib.crc32(body)!=int(match[2],16):invalid+=1;continue
        if any(byte<32 or byte>126 for byte in body):raise ValueError('CRC-valid non-ASCII diagnostic')
        fields=dict(re.findall(r'(\w+)=([^ ]+)',body.decode()))
        seq=fields.get('seq')
        if seq is None or not seq.isdecimal():raise ValueError('CRC-valid diagnostic lacks sequence')
        key=int(seq)
        if key in records and records[key]['body']!=body.decode():raise ValueError('Conflicting valid mirrors')
        if key not in records:records[key]={'body':body.decode(),'fields':fields,'valid_copies':0}
        records[key]['valid_copies']+=1
    if not records:raise ValueError('No CRC-valid owned SMMU records')
    return {'valid_records':len(records),'invalid_copies':invalid,'unsealed_prefixes':unsealed,
            'records':[records[key] for key in sorted(records)]}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();result=decode(args.capture.read_bytes())
    with args.output.open('x') as stream:stream.write(json.dumps(result,indent=2)+'\n')
    for row in result['records']:
        if row['fields'].get('phase') in ('baseline-final','close-rejected','close-before','close-after','close-live','peer-final'):
            print(row['body'])


if __name__=='__main__':main()
