#!/usr/bin/env python3
"""Create host-only standard empty NV/FTW snapshot and journal slots.

This never contacts a device or provisions the existing unreserved gap.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess,tempfile,uuid
ROOT=Path(__file__).resolve().parents[1]
def emit(volume_uuid,out):
 out=Path(out)
 if out.exists()and any(out.iterdir()):raise ValueError('output must be empty; preserve previous seed')
 out.mkdir(parents=True,exist_ok=True)
 inc=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
 sources=[ROOT/'tools/emit_nv_seed.c',ROOT/'bootprofiles/uefi-app/PianoNvJournal.c',ROOT/'bootprofiles/uefi-app/PianoNvFvb.c']
 with tempfile.TemporaryDirectory(prefix='nv-seed-host-')as tmp:
  exe=Path(tmp)/'emit'
  subprocess.run(['cc','-O2','-std=gnu11','-DNO_MSABI_VA_FUNCS','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include'),*map(str,sources),'-o',str(exe)],check=True)
  subprocess.run([str(exe),volume_uuid.bytes_le.hex(),str(out/'nv-ftw.bin'),str(out/'nv-slot-a.bin'),str(out/'nv-slot-b.bin')],check=True)
 files={p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}for p in sorted(out.glob('*.bin'))}
 result={'volume_uuid':str(volume_uuid),'uuid_wire':'EFI bytes_le','layout_id':1,'sequence':1,'snapshot_bytes':589824,'nv_bytes':262144,'working_bytes':65536,'spare_bytes':262144,'slot_blocks':768,'payload_first_block':1,'commit_block':767,'ftw_seed':'erased FF; standard FTW initializes workspace; no fabricated FTW record','files':files,'sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in sources},'device_io':False,'provisioned':False,'runtime_nv_set_supported':False}
 (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n');return result
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--volume-uuid',type=uuid.UUID,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();print(json.dumps(emit(a.volume_uuid,a.output),indent=2))
