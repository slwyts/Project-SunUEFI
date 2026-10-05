#!/usr/bin/env python3
"""Run stock fastboot boot for the fixed returning EFI probe, never flash."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
from check_fastboot_debug import parse_variable

ROOT=Path(__file__).resolve().parents[1]
EXPECTED='e42f0ae416c1843ed1dcdaa87301f9a34b093becec9c829d46ce03388038d2b9'


def probe_file():
    folder=ROOT/'artifacts/ram-boot-probe/return-probe-v1'
    probe=folder/'PianoRamBootProbe.efi'
    manifest=json.loads((folder/'manifest.json').read_text())
    data=probe.read_bytes()
    if len(data)!=3584 or hashlib.sha256(data).hexdigest()!=EXPECTED or manifest.get('sha256')!=EXPECTED:
        raise ValueError('Fixed probe provenance does not match firmware allowlist')
    return probe


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-id',type=int,required=True)
    parser.add_argument('--execute',action='store_true')
    args=parser.parse_args()
    if args.test_id<1:parser.error('Test ID must be positive')
    probe=probe_file()
    if not args.execute:
        print(json.dumps({'status':'DRY_RUN_NO_DEVICE_ACTION','probe':str(probe),'sha256':EXPECTED,
            'serial':'SunUEFI-piano','command':['fastboot','-s','SunUEFI-piano','boot',str(probe)]},indent=2))
        return
    record=ROOT/f'private/analysis/stage0-test-{args.test_id}.json'
    if not record.exists() or json.loads(record.read_text()).get('fastboot_boot',{}).get('exit_code')!=0:
        raise ValueError('Matching RAM-only firmware boot record is missing')
    out=ROOT/f'private/analysis/fastboot-ram-boot-host-test-{args.test_id}'
    out.mkdir(exist_ok=False)
    result={'test_id':args.test_id,'status':'pending','commands':[],'probe_sha256':EXPECTED,
        'persistent_write_commands':False,'efi_execution_verified':False}
    def command(*argv):
        run=subprocess.run(['fastboot','-s','SunUEFI-piano',*argv],capture_output=True,text=True,timeout=30)
        output=run.stdout+run.stderr
        result['commands'].append({'args':list(argv),'exit_code':run.returncode,'output':output})
        if run.returncode:raise RuntimeError('Fastboot failed: '+argv[0])
        return output
    try:
        end=time.monotonic()+90
        while time.monotonic()<end:
            seen=subprocess.run(['fastboot','devices'],capture_output=True,text=True,timeout=5)
            if any(line.split()[:2]==['SunUEFI-piano','fastboot'] for line in seen.stdout.splitlines()):break
            time.sleep(1)
        else:raise RuntimeError('UEFI fastboot did not enumerate')
        if parse_variable(command('getvar','product'),'product')!='piano-sunuefi':raise RuntimeError('Wrong product')
        if parse_variable(command('getvar','SunUEFI:ram-boot'),'SunUEFI:ram-boot')!='enabled':raise RuntimeError('RAM boot backend disabled')
        if int(parse_variable(command('getvar','max-download-size'),'max-download-size'),16)!=64*1024*1024:
            raise RuntimeError('Unexpected download limit')
        command('boot',str(probe))
        result['status']='BOOT_COMMAND_ACCEPTED_AWAIT_FIRMWARE_RECORD'
        # CLI OKAY proves command acceptance only. The subsequent retained
        # firmware log/volatile-record validation must prove actual execution.
    except Exception as exc:
        result.update(status='failed',error=str(exc));raise
    finally:
        (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k!='commands'},indent=2))


if __name__=='__main__':main()
