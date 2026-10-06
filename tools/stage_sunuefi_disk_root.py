#!/usr/bin/env python3
"""Stage only the two planned SunUEFI partitions in a derived distro tree."""
import argparse,json,shutil
from pathlib import Path
BASE=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument("--rootfs",type=Path,required=True)
ap.add_argument("--plan",type=Path,required=True)
a=ap.parse_args()
root=a.rootfs.resolve()
if not root.is_relative_to(BASE/"build/distros") or not root.is_dir():raise SystemExit("Expected derived workspace distro")
for name in ('etc','etc/piano','etc/udev','etc/udev/rules.d','usr','usr/lib','usr/lib/piano','usr/local','usr/local/sbin','boot','boot/efi'):
 if (root/name).is_symlink():raise SystemExit('Refuse writing through a guest directory symlink: '+name)
plan=json.loads(a.plan.read_text())
esp,linux=plan['new_partitions'];rid=linux['partition_guid'];eid=esp['partition_guid']
if (esp['name'],linux['name'])!=('sunuefi_esp','sunuefi_linux'):raise SystemExit('Unexpected partition names')
(root/'etc/piano').mkdir(exist_ok=True)
(root/'etc/piano/root-policy.json').write_text(json.dumps({'version':1,'mode':'partition','root_partuuid':rid,'root_label':'sunuefi_linux','esp_partuuid':eid,'esp_label':'sunuefi_esp'},indent=2)+'\n')
(root/'etc/fstab').write_text(f'# Dedicated SunUEFI partitions; no Android mounts.\nPARTUUID={rid} / ext4 defaults,noatime 0 1\nPARTUUID={eid} /boot/efi vfat umask=0077,nofail,x-systemd.device-timeout=10s 0 2\n')
(root/'boot/efi').mkdir(parents=True,exist_ok=True)
(root/'etc/udev/rules.d/01-piano-protect-android.rules').write_text(f'''SUBSYSTEM!="block", GOTO="sunuefi_storage_end"
KERNELS!="1d84000.*", GOTO="sunuefi_storage_end"
ENV{{UDISKS_IGNORE}}="1"
ENV{{DEVTYPE}}!="partition", GOTO="sunuefi_storage_end"
IMPORT{{builtin}}="blkid"
ENV{{ID_PART_ENTRY_UUID}}=="{rid}", GOTO="sunuefi_storage_end"
ENV{{ID_PART_ENTRY_UUID}}=="{eid}", GOTO="sunuefi_storage_end"
ATTR{{ro}}=="1", GOTO="sunuefi_storage_end"
RUN+="/usr/sbin/blockdev --setro /dev/%k"
LABEL="sunuefi_storage_end"
''')
p=root/'usr/lib/piano/piano-ram-hardware-prepare';shutil.copyfile(BASE/'bootprofiles/linux-userspace/piano-ram-hardware-prepare',p);p.chmod(0o755)
p=root/'usr/lib/piano/piano-disk-hardware-prepare'
if p.is_symlink() or p.exists():p.unlink()
p.symlink_to('piano-ram-hardware-prepare')
print('staged disk root policy, two PARTUUID mounts, original partitions read-only, shared hardware helper')

shutil.copy2(BASE/'bootprofiles/linux-userspace/piano-debug-bootstrap',root/'usr/local/sbin/piano-debug-bootstrap')
