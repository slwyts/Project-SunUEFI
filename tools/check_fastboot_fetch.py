#!/usr/bin/env python3
"""Fetch exactly xbl_config_a using stock fastboot and compare the original backup."""
import argparse
import hashlib
import json
import subprocess
import time
from pathlib import Path
from check_fastboot_debug import parse_variable


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-id',required=True,type=int)
    args=parser.parse_args()
    if args.test_id<1:parser.error('Invalid test ID')
    root=Path(__file__).resolve().parents[1]
    original=root/'private/captures/2026-10-03-piano/xbl_config_a.img'
    capture=json.loads((original.parent/'manifest.json').read_text())
    data=original.read_bytes();expected=hashlib.sha256(data).hexdigest()
    if expected!=capture['files'][original.name]['sha256']:
        raise RuntimeError('Original PC backup provenance changed')
    out=root/f'private/analysis/fastboot-fetch-host-test-{args.test_id}'
    out.mkdir(parents=True,exist_ok=False)
    result={'test_id':args.test_id,'partition':'xbl_config_a','status':'pending','commands':[],
            'device_write_commands':False}
    def command(*argv):
        run=subprocess.run(['fastboot','-s','SunUEFI-piano',*argv],capture_output=True,text=True,timeout=35)
        output=run.stdout+run.stderr
        result['commands'].append({'args':list(argv),'exit_code':run.returncode,'output':output})
        if run.returncode:raise RuntimeError('fastboot command failed: '+argv[0])
        return output
    try:
        until=time.monotonic()+180
        while time.monotonic()<until:
            devices=subprocess.run(['fastboot','devices'],capture_output=True,text=True,timeout=5)
            if any(line.split()[:2]==['SunUEFI-piano','fastboot'] for line in devices.stdout.splitlines()):break
            time.sleep(1)
        else:raise RuntimeError('UEFI fastboot not enumerated')
        if parse_variable(command('getvar','product'),'product')!='piano-sunuefi':
            raise RuntimeError('Wrong firmware product')
        maximum=int(parse_variable(command('getvar','max-fetch-size'),'max-fetch-size'),16)
        size=int(parse_variable(command('getvar','partition-size:xbl_config_a'),'partition-size:xbl_config_a'),16)
        if not 0<maximum<=65536 or size!=len(data):raise RuntimeError('Unexpected partition or chunk geometry')
        target=out/'xbl_config_a.img'
        command('fetch','xbl_config_a',str(target.resolve()))
        actual=target.read_bytes()
        if actual!=data:raise RuntimeError('Fetched bytes differ from original PC backup')
        result.update(status='VERIFIED_LIVE_FASTBOOT_UFS_PARTITION_FETCH',bytes=len(actual),
                      sha256=hashlib.sha256(actual).hexdigest(),byte_equal=True,max_fetch_size=maximum)
        command('oem','status')
        command('reboot')
    except Exception as exc:
        result['status']='failed';result['error']=str(exc);raise
    finally:
        (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k!='commands'},indent=2))


if __name__=='__main__':main()
