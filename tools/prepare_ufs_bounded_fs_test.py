#!/usr/bin/env python3
"""Verify PC whole-gap archive plus the fixed sparse FAT12 image; emit C header."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json

ROOT=Path(__file__).resolve().parents[1]
IMAGE_SHA='1e6d98314d880f88d6537fb466d3ec87b294422538bdc5795c574b6a1e713add'
EXTENTS={0:'f7c5bd6cbaf25dc1f131c7cf025fbd570756a941fa955283dc7dfe5ecd6b196d',1:'528ce1b62d75b107203528ebccd3eaf4be41536f2bad96a2a3e81807297c8f6e',3:'528ce1b62d75b107203528ebccd3eaf4be41536f2bad96a2a3e81807297c8f6e',5:'54ed6534db7b1bcef9fa1397349237b7d894c7ab75612c73245108dc421e56f3'}
HEADER='PianoUfsBoundedFsFormat.h'

def _archive():
    spec=importlib.util.spec_from_file_location('piano_archive',ROOT/'tools/prepare_ufs_write_test.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def typed(actual,expected):
    if type(actual) is not type(expected):return False
    if isinstance(expected,dict):return actual.keys()==expected.keys() and all(typed(actual[k],v) for k,v in expected.items())
    if isinstance(expected,list):return len(actual)==len(expected) and all(typed(a,b) for a,b in zip(actual,expected))
    return actual==expected

def verify(image_dir=None):
    archive=_archive();archive.verify_capture()
    folder=Path(image_dir) if image_dir else ROOT/'artifacts/ufs/test-window'
    image=folder/'fat12.img';manifest=folder/'manifest.json'
    if image.is_symlink() or manifest.is_symlink():raise ValueError('FAT source must be regular local files')
    raw=image.read_bytes();meta_raw=manifest.read_bytes();meta=json.loads(meta_raw,object_pairs_hook=archive._unique_object)
    if len(raw)!=14680064 or hashlib.sha256(raw).hexdigest()!=IMAGE_SHA:raise ValueError('Pinned FAT image bytes/SHA drift')
    expected=[{'logical_lba':lba,'physical_lba':375040+lba,'bytes':4096,'sha256':sha} for lba,sha in EXTENTS.items()]
    if not typed(meta.get('device_operations'),False) or meta.get('status')!='HOST_IMAGE_ONLY_NO_DEVICE_FORMAT' or meta.get('format')!='FAT12-superfloppy':raise ValueError('FAT manifest mode drift')
    for key,value in {'lun':4,'first_physical_lba':375040,'last_physical_lba':378623,'image':{'bytes':14680064,'sha256':IMAGE_SHA}}.items():
        if not typed(meta.get(key),value):raise ValueError('FAT manifest geometry/hash drift')
    geometry={'sector_bytes':4096,'sectors':3584,'cluster_sectors':1,'reserved_sectors':1,'fat_copies':2,'fat_sectors':2,'root_entries':128,'first_data_sector':6,'clusters':3578,'nonzero_blocks':expected}
    if not typed(meta.get('geometry'),geometry):raise ValueError('FAT sparse geometry drift')
    actual=[lba for lba in range(3584) if any(raw[lba*4096:(lba+1)*4096])]
    if actual!=list(EXTENTS):raise ValueError('Unexpected nonzero FAT sector')
    blocks=[raw[lba*4096:(lba+1)*4096] for lba in EXTENTS]
    if any(hashlib.sha256(block).hexdigest()!=sha for block,sha in zip(blocks,EXTENTS.values())):raise ValueError('FAT extent SHA drift')
    if image.read_bytes()!=raw or manifest.read_bytes()!=meta_raw:raise ValueError('FAT source changed between independent reads')
    return blocks

def render(blocks):
    array=_archive()._c_array
    out=['// Verified PC capture1 plus pinned FAT12 host image only.','// SPDX-License-Identifier: BSD-2-Clause-Patent','#pragma once','#include <Uefi.h>','#define PIANO_UFS_FAT_FORMAT_PC_VERIFIED TRUE','STATIC CONST UINT32 mPianoFatLbas[4]={0,1,3,5};',array('mPianoFatImageSha',bytes.fromhex(IMAGE_SHA))]
    for i,(data,sha) in enumerate(zip(blocks,EXTENTS.values())):out.extend([array('mPianoFatBlock'+str(i),data),array('mPianoFatBlockSha'+str(i),bytes.fromhex(sha))])
    out.append('STATIC CONST UINT8 *mPianoFatBlocks[4]={mPianoFatBlock0,mPianoFatBlock1,mPianoFatBlock2,mPianoFatBlock3};')
    out.append('STATIC CONST UINT8 *mPianoFatBlockHashes[4]={mPianoFatBlockSha0,mPianoFatBlockSha1,mPianoFatBlockSha2,mPianoFatBlockSha3};')
    return ('\n'.join(out)+'\n').encode('ascii')

def prepare(output,image_dir=None):
    p=Path(output)
    if p.name!=HEADER or p.is_symlink():raise ValueError('Invalid FAT header output')
    data=render(verify(image_dir));p.parent.mkdir(parents=True,exist_ok=True)
    # Use the same independently checked atomic host-output durability pattern.
    import os,tempfile
    with tempfile.NamedTemporaryFile(dir=p.parent,delete=False) as f:
        tmp=Path(f.name);f.write(data);f.flush();os.fsync(f.fileno())
    try:
        if tmp.read_bytes()!=data:raise ValueError('FAT header readback mismatch')
        os.replace(tmp,p);fd=os.open(p.parent,os.O_DIRECTORY)
        try:os.fsync(fd)
        finally:os.close(fd)
        if p.read_bytes()!=data:raise ValueError('FAT header final readback mismatch')
    finally:tmp.unlink(missing_ok=True)
    return p.resolve()

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=ROOT/'build/ufs-write-test'/HEADER);args=parser.parse_args()
    print(prepare(args.output))
if __name__=='__main__':main()
