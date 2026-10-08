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

from build_piano_ram_bootstrap import guest_resolve

ROOT=Path(__file__).resolve().parents[1]
EXPECTED='a75f8c5d5fa099d65c171ac839c2e3bb6c63ec45'


def stage_gnome_power(rootfs):
    """Install the existing GNOME power service's local cover support."""
    rootfs = Path(rootfs).resolve()
    if not rootfs.is_relative_to(ROOT / 'build/distros') or not rootfs.is_dir():
        raise ValueError('GNOME power files require a workspace rootfs')
    overlay = ROOT / 'linux/desktops/gnome/power-overlay'
    copied = {}
    for source in sorted(overlay.rglob('*')):
        if not source.is_file() or '__pycache__' in source.parts:
            continue
        relative = source.relative_to(overlay)
        # Debian links /etc/default/locale to /etc/locale.conf. Resolve guest
        # links inside this root rather than following host absolute paths.
        destination = guest_resolve(rootfs, relative.as_posix())
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(0o755 if relative.as_posix() == 'usr/lib/piano/power-button' else 0o644)
        copied[destination.relative_to(rootfs).as_posix()] = hashlib.sha256(source.read_bytes()).hexdigest()
    # Use the same GNOME defaults as the multi-distro assembler. In particular,
    # the public schema override disables animation; local dconf takes priority.
    defaults = ROOT / 'linux/desktops/gnome/defaults.ini'
    relative = Path('etc/dconf/db/local.d/00-sunuefi')
    destination = rootfs / relative
    if destination.is_symlink() or any(parent.is_symlink() for parent in destination.parents
            if parent != rootfs and parent.is_relative_to(rootfs)):
        raise ValueError('Guest symlink in GNOME defaults destination')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(defaults, destination)
    destination.chmod(0o644)
    copied[relative.as_posix()] = hashlib.sha256(defaults.read_bytes()).hexdigest()
    wants = rootfs / 'etc/systemd/user/graphical-session.target.wants'
    if any(parent.is_symlink() for parent in (wants, *wants.parents)
           if parent != rootfs and parent.is_relative_to(rootfs)):
        raise ValueError('Guest symlink in GNOME user service directory')
    wants.mkdir(parents=True, exist_ok=True)
    link = wants / 'piano-power-button.service'
    if link.exists() or link.is_symlink():
        link.unlink()
    link.symlink_to('/usr/lib/systemd/user/piano-power-button.service')
    return copied


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
    copied.update(stage_gnome_power(rootfs))
    # Use the same camera BSP module policy in the complete default release.
    # The public SoC of:* autoload block otherwise leaves this driver unbound.
    flash_source = ROOT / 'linux/bsp/optional/camera/etc/modules-load.d/piano-flash.conf'
    flash_target = rootfs / 'etc/modules-load.d/piano-flash.conf'
    if flash_target.is_symlink() or any(parent.is_symlink() for parent in flash_target.parents
            if parent != rootfs and parent.is_relative_to(rootfs)):
        raise ValueError('Guest symlink in flash module policy destination')
    flash_target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(flash_source, flash_target)
    flash_target.chmod(0o644)
    copied['etc/modules-load.d/piano-flash.conf'] = hashlib.sha256(flash_source.read_bytes()).hexdigest()
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
