#!/usr/bin/env python3
"""Recover checksum-valid DMA journal copies without changing the capture."""
import argparse
import collections
import json
from pathlib import Path
import re
import zlib

def check(text):
    records={};copies=collections.Counter();bad=0
    pattern=re.compile(r'SUNUEFI_DMA(?:_COPY)? (phase=.+) crc32=([0-9A-F]{8})$')
    fields=re.compile(r'phase=(\S+) seq=(\d+) dev=(\S+) sid=([0-9A-F]+) pa=([0-9A-F]+) '
        r'iova=([0-9A-F]+) bytes=(\d+) reserved=(\d+) dir=(\S+) align=(\d+) attrs=([0-9A-F]+) '
        r'cache=(\S+) status=(.+) cmd=(\S+)$')
    for line in text.splitlines():
        if not line.startswith(('SUNUEFI_DMA phase=','SUNUEFI_DMA_COPY phase=')):continue
        match=pattern.fullmatch(line)
        if not match:bad+=1;continue
        body,crc=match.groups()
        try:valid=zlib.crc32(body.encode('ascii'))==int(crc,16)
        except UnicodeError:valid=False
        values=fields.fullmatch(body)
        if not valid or not values:bad+=1;continue
        phase,sequence,dev,sid,pa,iova,size,reserved,direction,align,attrs,cache,status,command=values.groups()
        sequence=int(sequence)
        if sequence in records and records[sequence]['body']!=body:
            raise ValueError('Conflicting checksum-valid records for one sequence')
        records[sequence]={'body':body,'phase':phase,'dev':dev,'iova':int(iova,16),'cmd':command}
        copies[sequence]+=1
    if set(records)!=set(range(220)):raise ValueError('Missing checksum-valid DMA journal sequence')
    phases=collections.Counter(r['phase'] for r in records.values())
    if phases!={'allocate':7,'map':6,'submit':102,'complete':102,'unmap':3}:
        raise ValueError('Incomplete DMA lifecycle')
    commands=[records[s]['cmd'] for s in sorted(records) if records[s]['phase']=='submit' and records[s]['iova']==0x40000000]
    allowed={'NOP','QUERY_READ_DEVICE_DESCRIPTOR','QUERY_READ_FULL_DEVICE_DESCRIPTOR','QUERY_READ_CURRENT_POWER_MODE',
             'RESUME_DEVICE_ACTIVE_NO_DATA','REPORT_LUNS','READ_CAPACITY_16','READ_LBA_10'}
    if len(commands)!=37 or not set(commands)<=allowed:raise ValueError('Incomplete or unexpected DMA command journal')
    return {'records_checked':220,'all_selected_record_crcs_match':True,'damaged_copies_rejected':bad,
            'records_with_one_valid_copy':[s for s in sorted(records) if copies[s]<2],
            'commands_verified':37,'original_log_modified':False,
            'verified_records':[records[s]['body'] for s in sorted(records)]}

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--test-id',type=int,required=True);args=ap.parse_args()
    root=Path(__file__).resolve().parent.parent;out=root/f'private/analysis/ramlog-test-{args.test_id}'
    record=check((out/'uefi.txt').read_text());verified=record.pop('verified_records')
    (out/'dma-verified.txt').write_text('\n'.join(verified)+'\n')
    record['test_id']=args.test_id
    (out/'dma-integrity.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))

if __name__=='__main__':main()
