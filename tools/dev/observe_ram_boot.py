#!/usr/bin/env python3
"""Observe only the SunUEFI USB ACM gadget and capture a RAM Linux report.

The optional console command reads kernel/mount state, then reboots to Android.
Never sends commands to an unrelated serial device.
"""
import argparse
import glob
import json
import os
from pathlib import Path
import select
import termios
import time


def sunuefi_port():
    for device in glob.glob('/dev/ttyACM*'):
        path = (Path('/sys/class/tty') / Path(device).name / 'device').resolve()
        for parent in (path, *path.parents):
            serial = parent / 'serial'
            product = parent / 'product'
            if serial.is_file() and product.is_file():
                if (serial.read_text().strip() == 'SunUEFI-RAM' and
                        product.read_text().strip() == 'Linux RAM serial console'):
                    return device
                break
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--test-id', type=int, required=True)
    ap.add_argument('--seconds', type=int, default=230)
    ap.add_argument('--report-and-reboot', action='store_true')
    args = ap.parse_args()
    out = Path(__file__).resolve().parent.parent / 'private/analysis' / f'usb-test-{args.test_id}'
    out.mkdir(exist_ok=False)
    deadline = time.monotonic() + args.seconds
    fd = None
    device = None
    sent = False
    observed = False
    report = b"\nuname -a; cat /proc/cmdline; cat /proc/mounts; echo SUNUEFI_LINUX_USERLAND_OK; reboot -f\n"
    try:
        with (out / 'console.txt').open('wb') as log:
            while time.monotonic() < deadline:
                if fd is None:
                    device = sunuefi_port()
                    if device is None:
                        time.sleep(1)
                        continue
                    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                    config = termios.tcgetattr(fd)
                    config[0] = config[1] = config[3] = 0
                    config[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
                    config[4] = config[5] = termios.B115200
                    termios.tcsetattr(fd, termios.TCSANOW, config)
                    print(f'SunUEFI gadget enumerated at {device}', flush=True)
                    time.sleep(2)
                if args.report_and_reboot and not sent:
                    os.write(fd, report)
                    sent = True
                if select.select([fd], [], [], 1)[0]:
                    try:
                        data = os.read(fd, 65536)
                    except OSError:
                        break
                    if not data:
                        break
                    log.write(data)
                    log.flush()
                    observed |= b'SUNUEFI_LINUX_USERLAND_OK' in data
    finally:
        if fd is not None:
            os.close(fd)
        result = {'usb_device': device, 'report_sent': sent,
                  'userland_marker_in_output': observed,
                  'note': 'Inspect console report; an echoed input alone is not OS boot proof'}
        (out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
