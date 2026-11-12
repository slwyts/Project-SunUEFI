#!/usr/bin/env python3
"""Read only GPT metadata in Android and compare to a completed UEFI test."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial',required=True)
    ap.add_argument('--test-id',type=int,required=True)
    args=ap.parse_args()
    root=Path(__file__).resolve().parent.parent
    adb=['adb','-s',args.serial]
    def run(command):
        return subprocess.check_output(adb+['exec-out',command],timeout=20)
    if run('getprop ro.product.device').strip()!=b'piano' or run('getprop sys.boot_completed').strip()!=b'1':
        raise SystemExit('Expected authorized piano restored to Android')
    log=(root/f'private/analysis/ramlog-test-{args.test_id}/uefi.txt').read_text()
    if 'SUNUEFI_UFS_GPT_MILESTONE luns=6 valid_gpts=6 physical_ufs_writes=0' not in log:
        raise SystemExit('No complete six-LUN UEFI GPT milestone in this test')
    capacities={int(lun):(int(last,16),int(block)) for lun,last,block in re.findall(
        r'SUNUEFI_UFS_CAPACITY lun=(\d+) status=Success last_lba=([0-9A-F]+) block_bytes=(\d+)',log)}
    if not capacities:
        capacities={int(lun):(int(last,16),int(block)) for lun,last,block in re.findall(
            r'SUNUEFI_UFS_BLOCKIO_TEST lun=(\d+) read=Success gpt_signature=1 write=Write Protected readonly=1 last=([0-9A-F]+) block=(\d+)',log)}
    expected={int(lun):(int(hcrc,16),int(acrc,16),int(entry,16),int(array),int(count),int(active))
        for lun,hcrc,acrc,entry,array,count,active in re.findall(
        r'SUNUEFI_UFS_GPT_VERIFIED lun=(\d+) status=Success header_crc=([0-9A-F]+) array_crc=([0-9A-F]+) entry_lba=([0-9A-F]+) array_bytes=(\d+) entries=(\d+) active_partitions=(\d+)',log)}
    if set(expected)!=set(range(6)) or set(capacities)!=set(expected):
        raise SystemExit('Incomplete UEFI capacity/GPT evidence')
    out=root/f'private/analysis/ufs-gpt-android-test-{args.test_id}'
    out.mkdir(exist_ok=False)
    records=[]
    for lun in sorted(expected):
        last,block=capacities[lun]
        name=run(f"su -c 'ls /sys/class/scsi_device/0:0:0:{lun}/device/block'").decode().strip()
        if not re.fullmatch(r'sd[a-z]+',name) or block not in (512,4096):
            raise SystemExit('Invalid block device or block size')
        sectors=int(run(f"su -c 'cat /sys/block/{name}/size'").strip())
        logical=int(run(f"su -c 'cat /sys/block/{name}/queue/logical_block_size'").strip())
        if logical!=block or sectors*512!=(last+1)*block:
            raise SystemExit(f'LUN {lun} Android/UEFI capacity mismatch')
        # Exactly MBR and primary GPT header; no user partition content.
        first=run(f"su -c 'dd if=/dev/block/{name} bs={block} count=2 2>/dev/null'")
        if len(first)!=2*block or first[block:block+8]!=b'EFI PART':
            raise SystemExit('Missing complete GPT header')
        header=bytearray(first[block:])
        size,hcrc=struct.unpack_from('<II',header,12)
        if not 92<=size<=block:raise SystemExit('Invalid header size')
        struct.pack_into('<I',header,16,0)
        hentry,count,entry_size,acrc=struct.unpack_from('<QIII',header,72)
        array_bytes=count*entry_size
        if hcrc!=zlib.crc32(header[:size]) or not 0<array_bytes<=65536 or entry_size<128:
            raise SystemExit('Invalid GPT header CRC/array bounds')
        count_blocks=(array_bytes+block-1)//block
        first_usable=struct.unpack_from('<Q',header,40)[0]
        if hentry<2 or hentry+count_blocks>first_usable or hentry+count_blocks>last:
            raise SystemExit('Refusing GPT array read outside metadata area')
        data=run(f"su -c 'dd if=/dev/block/{name} bs={block} skip={hentry} count={count_blocks} 2>/dev/null'")
        if len(data)!=count_blocks*block:raise SystemExit('Incomplete array read')
        data=data[:array_bytes]
        active=sum(any(data[i:i+16]) for i in range(0,array_bytes,entry_size))
        actual=(hcrc,acrc,hentry,array_bytes,count,active)
        if actual!=expected[lun] or zlib.crc32(data)!=acrc:
            raise SystemExit(f'LUN {lun} UEFI/Android GPT mismatch')
        (out/f'lun-{lun}-mbr-header.bin').write_bytes(first)
        (out/f'lun-{lun}-entries.bin').write_bytes(data)
        record={'lun':lun,'block_device':name,'capacity_bytes':sectors*512,'logical_block_bytes':logical,
                'header_crc':f'{hcrc:08X}','array_crc':f'{acrc:08X}','entries':count,'active_partitions':active,
                'matches_uefi':True,'device_writes':False}
        records.append(record)
        print(json.dumps(record),flush=True)
    summary={'test_id':args.test_id,'all_six_match':True,'device_writes':False,'luns':records}
    (out/'manifest.json').write_text(json.dumps(summary,indent=2)+'\n')

if __name__=='__main__':main()
