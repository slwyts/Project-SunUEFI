#!/usr/bin/env python3
"""Stage the standard Linux RAM debug tools and explicit pure-data configuration.

No service start, host network change, device or storage operation. Forced
kernel command lines need this distro config to select the debug transport.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT=Path(__file__).resolve().parents[1]
FILES={'piano-debug-bootstrap':'usr/local/sbin/piano-debug-bootstrap',
       'piano-debug-stage':'usr/local/sbin/piano-debug-stage',
       'piano-linux-debug.service':'usr/lib/systemd/system/piano-linux-debug.service',
       'piano-linux-serial.service':'usr/lib/systemd/system/piano-linux-serial.service'}


def safe(root,name):
    path=root/name
    for parent in (path,*path.parents):
        if parent==root:break
        if parent.is_symlink():raise ValueError('Debug destination traverses a guest symlink')
    return path


def stage(root,usb='acm-ncm',shell=True):
    root=Path(root).resolve()
    if not root.is_relative_to(ROOT/'build/distros') or not root.is_dir():
        raise ValueError('Explicit derived workspace GNU root required')
    if usb not in ('off','acm-ncm'):raise ValueError('Unsupported Linux USB debug transport')
    for relative in (*FILES.values(),'etc/piano/linux-debug.conf','usr/share/piano-provenance/linux-debug.json'):
        safe(root,relative)
    units=root/'etc/systemd/system/multi-user.target.wants'
    for name in ('piano-linux-debug.service','piano-linux-serial.service'):
        link=units/name
        if link.exists()or link.is_symlink():
            if not link.is_symlink()or str(link.readlink())!='/usr/lib/systemd/system/'+name:
                raise ValueError('Existing debug service link differs')
    records={}
    for name,relative in FILES.items():
        source=ROOT/'bootprofiles/linux-userspace'/name;target=safe(root,relative)
        target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
        target.chmod(0o644 if name.endswith('.service')else 0o755)
        records[relative]=hashlib.sha256(target.read_bytes()).hexdigest()
    config=safe(root,'etc/piano/linux-debug.conf');config.parent.mkdir(parents=True,exist_ok=True)
    config.write_text(f'usb={usb}\nshell={int(shell)}\nipv4=192.168.77.1/30\nrecovery_seconds=0\n');config.chmod(0o600)
    records['etc/piano/linux-debug.conf']=hashlib.sha256(config.read_bytes()).hexdigest()
    units.mkdir(parents=True,exist_ok=True)
    for name in ('piano-linux-debug.service','piano-linux-serial.service'):
        link=units/name
        if name=='piano-linux-serial.service'and not shell:
            if link.is_symlink():link.unlink()
        elif not link.is_symlink():link.symlink_to('/usr/lib/systemd/system/'+name)
    record={'status':'LINUX_RAM_DEBUG_STAGED_NOT_ENUMERATION_VERIFIED','files':records,
            'usb_requested':usb,'serial_shell_requested':shell,'uses_actual_role_and_udc_checks':True,
            'gadget_enumeration_verified':False,'adb_daemon_started':False,'host_service_started':False}
    safe(root,'usr/share/piano-provenance/linux-debug.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--rootfs',type=Path,required=True)
    parser.add_argument('--usb',choices=('off','acm-ncm'),default='acm-ncm');parser.add_argument('--no-shell',action='store_true')
    args=parser.parse_args();print(json.dumps(stage(args.rootfs,args.usb,not args.no_shell),indent=2))
