#!/usr/bin/env python3
"""Seal already verified read-only milestone artifacts; no device commands."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tests',type=int,nargs=2,required=True);args=ap.parse_args()
    root=Path(__file__).resolve().parent.parent;results=[]
    for n in args.tests:
        private=root/'private/analysis'
        record=json.loads((private/f'stage0-test-{n}.json').read_text())
        partitions=json.loads((private/f'partition-verification-test-{n}.json').read_text())
        android=json.loads((private/f'ufs-gpt-android-test-{n}/manifest.json').read_text())
        journal=json.loads((private/f'ramlog-test-{n}/dma-integrity.json').read_text())
        log=(private/f'ramlog-test-{n}/uefi.txt').read_text()
        assert record['fastboot_boot']['exit_code']==0 and not record['flash_commands_performed']
        assert partitions['all_26_match'] and android['all_six_match'] and not android['device_writes']
        assert journal['records_checked']==220 and journal['all_selected_record_crcs_match']
        assert journal['commands_verified']==37 and not journal['original_log_modified']
        for marker in ('SUNUEFI_UFS_NOP_RESULT Success','SUNUEFI_UFS_QUERY_AFTER_RESUME Success',
            'SUNUEFI_UFS_GPT_MILESTONE luns=6 valid_gpts=6 physical_ufs_writes=0',
            'SUNUEFI_SMMU_DETACH_VERIFIED ufs_stream_absent=1 other_streams_unchanged=1',
            'SUNUEFI_UFS_READONLY_DMA_END status=Success physical_ufs_writes=0','PIANO_STAGE0_RETURN_TO_ANDROID'):
            assert marker in log,marker
        assert len(re.findall('SUNUEFI_UFS_GPT_VERIFIED',log))==6
        assert len(re.findall('SUNUEFI_UFS_LBA_READ',log))==21
        result={'test_id':n,'image_sha256':record['image_sha256'],'dma_commands':37,'dma_records_verified':220,
                'metadata_blocks_read':21,'ordinary_luns':6,'verified_gpts':6,'android_gpt_comparison':True,
                'all_26_boot_partitions_match':True,'smmu_detach_verified':True,'android_auto_recovery':True,
                'physical_ufs_writes':False,'dma_journal':journal}
        results.append(result)
    assert results[0]['image_sha256']==results[1]['image_sha256']
    out=root/'artifacts/dma-milestone';out.mkdir(exist_ok=True)
    source=root/f'artifacts/tests/stage0-test-{args.tests[0]}'
    for old,new in [('piano-stage0-UNTESTED.img','piano-ufs-readonly-gpt-verified.img'),
                    ('piano-stage0.fd','piano-ufs-readonly-gpt-verified.fd')]:
        shutil.copyfile(source/old,out/new)
    files={p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
           for p in out.iterdir() if p.suffix in ('.img','.fd')}
    assert files['piano-ufs-readonly-gpt-verified.img']['sha256']==results[0]['image_sha256']
    manifest={'status':'VERIFIED_PIANO_READ_ONLY_UFS_DMA_GPT_WITH_CHECKSUM_JOURNAL','date':'2026-10-05',
        'build_id':json.loads((source/'manifest.json').read_text())['build_id'],'tests':results,'files':files,
        'luns':android['luns'],'boot_method':'fastboot boot only','recovery_timer_seconds':60,
        'limitations':['Raw ramoops text can lose bytes; checksum-valid journal copies selected, raw captures preserved',
                       'USB/GPI DMA hardware paths and high 64-bit IOVA not validated',
                       'UFS BlockIO not integrated; physical block writes disabled']}
    for path in (out/'manifest.json',root/'private/analysis/dma-milestone-2026-10-05.json'):
        path.write_text(json.dumps(manifest,indent=2)+'\n')
    for result in results:
        path=root/f'private/analysis/stage0-test-{result["test_id"]}.json'
        record=json.loads(path.read_text());record['result']='Read-only UFS GPT milestone and checksum journal verified'
        record['verification']=result;path.write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'status':manifest['status'],'tests':args.tests,'files':files},indent=2))

if __name__=='__main__':main()
