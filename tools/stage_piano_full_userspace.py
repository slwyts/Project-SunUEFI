#!/usr/bin/env python3
"""Stage the complete public hardware service overlay with RAM-root policy.

Does not run it, mount blocks, prepare firmware or build a boot image. Rootfs
must be an explicitly supplied workspace directory; source originals stay intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
EXPECTED='fd6266d73f3442b23362260c3aa0c86782e0b52c'


def stage(rootfs, source):
    rootfs,source=Path(rootfs).resolve(),Path(source).resolve()
    if not rootfs.is_relative_to(ROOT) or not rootfs.is_dir():
        raise ValueError('Rootfs must be an existing workspace directory')
    commit=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    if commit!=EXPECTED or subprocess.check_output(['git','-C',str(source),'status','--porcelain'],text=True):
        raise ValueError('Expected fixed clean complete userspace source')
    overlay=source/'rootfs/overlay'
    if not overlay.is_dir():raise ValueError('Complete upstream overlay missing')
    copied={}
    for path in sorted(overlay.rglob('*')):
        name=path.relative_to(overlay)
        target=rootfs/name
        # Never follow guest-root absolute links while writing on the host.
        if any(parent.is_symlink()for parent in target.parents if parent!=rootfs and parent.is_relative_to(rootfs)):
            raise ValueError('Guest symlink parent in overlay destination')
        if path.is_dir():target.mkdir(parents=True,exist_ok=True)
        elif path.is_symlink():
            target.parent.mkdir(parents=True,exist_ok=True)
            if target.exists() or target.is_symlink():target.unlink()
            target.symlink_to(path.readlink())
        elif path.is_file():
            target.parent.mkdir(parents=True,exist_ok=True)
            if target.is_symlink():raise ValueError('Refuse writing through guest symlink')
            shutil.copy2(path,target)
            copied[name.as_posix()]=hashlib.sha256(path.read_bytes()).hexdigest()
    # Replace only persistence policy. All display/input/radio/audio/camera
    # service files and dependency links copied above remain present.
    (rootfs/'etc/fstab').write_text('# Piano RAM root. No Android mounts or growfs.\n')
    rules=rootfs/'etc/udev/rules.d/01-piano-protect-android.rules'
    rules.parent.mkdir(parents=True,exist_ok=True)
    rules.write_text('SUBSYSTEM=="block", KERNELS=="1d84000.*", ENV{UDISKS_IGNORE}="1", RUN+="/usr/sbin/blockdev --setro /dev/%k"\n')
    units=rootfs/'etc/systemd/system'
    for unit in ('piano-swapfile.service','systemd-growfs-root.service','qbootctl.service'):
        target=units/unit
        if target.exists() or target.is_symlink():target.unlink()
        target.symlink_to('/dev/null')
    init=ROOT/'linux/userspace/pianoinit'
    shutil.copy2(init,rootfs/'pianoinit');(rootfs/'pianoinit').chmod(0o755)
    result={'status':'COMPLETE_SERVICE_OVERLAY_STAGED_NOT_BOOT_VERIFIED','source_commit':commit,
        'source_files':copied,'root_policy':'RAM_ONLY_NO_ANDROID_BLOCK_MOUNT',
        'hardware_services_removed':False,'userdata_growfs':False,'boot_slot_writes':False,'ufs_originals_readonly_policy':True,
        'ram_root_limit_bytes':4294967296,'ram_migration_extra_bytes':'source rootfs bytes + 512MiB headroom',
        'hardware_prepare_required':True,'piano_boot_verified':False,'pid1_verified':False}
    (rootfs.parent/(rootfs.name+'-piano-stage.json')).write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs',type=Path,required=True)
    parser.add_argument('--source',type=Path,default=ROOT/'upstream/debian-piano-current')
    args=parser.parse_args();result=stage(args.rootfs,args.source)
    print(json.dumps({key:result[key]for key in ('status','source_commit','root_policy','hardware_services_removed')},indent=2))
