#!/usr/bin/env python3
"""Read ramoops after an explicit RAM boot; never writes device files."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--test-id', type=int, required=True)
    ap.add_argument('--wait-seconds', type=int, default=100)
    args = ap.parse_args()
    adb = ['adb','-s',args.serial]
    root = Path(__file__).resolve().parent.parent
    record_path=root/'private/analysis'/f'stage0-test-{args.test_id}.json'
    deadline = time.monotonic() + args.wait_seconds
    # Do not mistake Android before adb reboot/fastboot boot has completed for
    # recovery from this test. The boot runner writes this record only after
    # the fastboot boot command has returned.
    while time.monotonic()<deadline:
        if record_path.exists():
            record=json.loads(record_path.read_text())
            if record.get('error') or record.get('fastboot_boot',{}).get('exit_code')!=0:
                raise SystemExit('Test did not complete a successful fastboot RAM boot')
            break
        time.sleep(1)
    else: raise SystemExit('RAM boot has not completed; refusing to collect old Android logs')
    while time.monotonic() < deadline:
        p = subprocess.run(adb+['shell','getprop','sys.boot_completed'],capture_output=True,text=True,timeout=5)
        if p.returncode == 0 and p.stdout.strip() == '1': break
        time.sleep(2)
    else: raise SystemExit('Android has not returned; manual recovery may be needed')
    console_name = 'console-ramoops-0'
    p = subprocess.run(adb+['exec-out',"su -c 'cat /sys/fs/pstore/console-ramoops-0'"],capture_output=True,timeout=20)
    if p.returncode or not p.stdout or p.stdout.startswith(b'cat:'):
        # The Linux DT can place its console where the stock DT expects pmsg.
        # Accept that fallback only with an actual kernel version record, and
        # retain the source name so Android pmsg is not mistaken for a console.
        console_name = 'pmsg-ramoops-0'
        p = subprocess.run(adb+['exec-out',"su -c 'cat /sys/fs/pstore/pmsg-ramoops-0'"],capture_output=True,timeout=20)
        if p.returncode or not re.search(rb'(?m)^\[\s*[0-9.]+\] Linux version ',p.stdout):
            raise SystemExit('Cannot read ramoops console or identify a Linux console in pmsg')
    out = root/'private/analysis'/f'ramlog-test-{args.test_id}'
    out.mkdir(exist_ok=False)
    (out/'console.txt').write_bytes(p.stdout)
    # A later kernel panic can be in dmesg-ramoops rather than the firmware
    # console. Preserve every named ramoops object without changing originals.
    pstore = {console_name: {'bytes': len(p.stdout),
                                  'sha256': hashlib.sha256(p.stdout).hexdigest(),
                                  'saved_as': 'console.txt'}}
    listing = subprocess.run(adb+['shell','ls','-1','/sys/fs/pstore'],
                             capture_output=True,text=True,timeout=10)
    if listing.returncode == 0:
        names = sorted(set(listing.stdout.splitlines()))
        for name in names:
            if name == console_name or not re.fullmatch(
                    r'(?:console|dmesg|pmsg|ftrace)-ramoops(?:-\d+)?',name):
                continue
            read = subprocess.run(adb+['exec-out','su -c '+shlex.quote('cat /sys/fs/pstore/'+name)],
                                  capture_output=True,timeout=20)
            if read.returncode or read.stdout.startswith(b'cat:'):
                pstore[name] = {'read_error': read.stderr.decode(errors='replace')}
                continue
            directory = out/'pstore'
            directory.mkdir(exist_ok=True)
            (directory/name).write_bytes(read.stdout)
            pstore[name] = {'bytes': len(read.stdout),
                           'sha256': hashlib.sha256(read.stdout).hexdigest(),
                           'saved_as': 'pstore/'+name}
    text = p.stdout.decode(errors='replace')
    marker = 'SUNUEFI_RAMLOG_BEGIN'
    index = max(text.rfind(marker),text.rfind('SUNUEFI_BLOCKIO_REPORT_BEGIN'))
    scope = 'session-or-blockio-report' if index >= 0 else 'none'
    product=text.rfind('SUNUEFI_EARLY_SMEM status=')
    if product>=0 and product>index:
        firmware=text.rfind('Firmware Version',0,product)
        index=firmware if firmware>=0 else product
        scope='product-cold-session-without-core-ready'
    if index < 0:
        # Large bulk uploads can wrap the session start. Preserve the final
        # USB/cleanup report, but explicitly avoid claiming a full session log.
        index = text.rfind('SUNUEFI_FASTBOOT_RESULT')
        if index >= 0:scope = 'fastboot-final-report-only'
    if index < 0:
        index = text.rfind('SUNUEFI_UFS_WINDOW_GUARD')
        if index >= 0:scope = 'bounded-ufs-final-report-only'
    if index < 0 and 'SUNUEFI_UFS_DMA_COMPLETE command=BLOCKIO_READ' in text and (
            'PIANO_KEY_EVENT' in text or 'bootitem-piano-setup' in text):
        # A busy filesystem probe can overwrite the entire startup prefix.
        # Preserve this observed firmware/UI tail without inventing a session
        # marker, CORE_READY record or cold memory authority.
        index=0
        scope='wrapped-uefi-tail-with-product-ui-events'
    segment = text[index:] if index >= 0 else ''
    (out/'uefi.txt').write_text(segment)
    linux = 'rdinit=/init ro nokaslr efi=novamap' in text and 'console=ttyGS0,115200' in text
    independent_init = 'PIANO_KERNEL_RAM BEGIN pid=1' in text
    disk_init = bool(re.search(r'(?m)^\[\s*[0-9.]+\] piano: DISK_INIT_BEGIN:', text))
    (out/'linux.txt').write_text(text if linux else '')
    summary = {'test_id':args.test_id,'console_bytes':len(p.stdout),
               'console_source':console_name,
               'linux_kernel_version':next((line for line in text.splitlines() if re.match(r'^\[\s*[0-9.]+\] Linux version ',line)),None),
               'uefi_marker_found':index>=0 and scope!='wrapped-uefi-tail-with-product-ui-events',
               'uefi_bytes':len(segment.encode()),'path':str(out/'uefi.txt'),
               'linux_ram_command_line_found':linux,
               'linux_init_process_started':disk_init or (linux and 'Run /init as init process' in text),
               'linux_disk_initramfs_userland_verified':disk_init,
               'linux_disk_root_failure':next((line for line in reversed(text.splitlines()) if 'piano: DISK_ROOT_REFUSED:' in line),None),
               'linux_ram_userland_marker':linux and 'SUNUEFI_RAM_INIT BEGIN pid=1' in text,
               'independent_kernel_ram_userland_marker':linux and independent_init,
               'pstore_files':pstore,'uefi_log_scope':scope}
    summary['product_core_ready']='PIANO_PRODUCT_CORE_READY' in segment
    summary['product_core_payload_security_violation']='SUNUEFI_PRODUCT_CORE_RETURN status=Security Violation' in segment
    summary['startup_prefix_overwritten']=scope=='wrapped-uefi-tail-with-product-ui-events'
    summary['simpleinit_menu_events_found']='PIANO_KEY_EVENT' in segment or 'bootitem-piano-setup' in segment
    summary['linux_disk_command_line_found']='rdinit=/pianoinit' in text and 'piano.root=PARTUUID=' in text
    if summary['linux_kernel_version']:
        (out/'linux.txt').write_text(text)
    (out/'manifest.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))
    if segment: print(segment[-18000:])
    if linux:
        for line in text.splitlines():
            if any(key in line for key in ('Linux version', 'Kernel command line',
                   'Run /init as init process', 'SUNUEFI_RAM_INIT', 'PIANO_KERNEL_RAM', 'reboot: Restarting')):
                print(line)

if __name__ == '__main__': main()
