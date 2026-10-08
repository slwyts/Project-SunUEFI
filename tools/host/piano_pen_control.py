#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Observe Focus Pen Pro vendor state, optionally forward real state to NVT.

Run on the tablet as root for --forward-stationary. This diagnostic does not
pair a pen, read the touch FIFO, change scan mode, or create input devices.
"""
import argparse
import json
from pathlib import Path
import signal
import time

FE11 = '0000fe11-aa6c-462a-964a-7f2ed5b3e512'
FE12 = '0000fe12-aa6c-462a-964a-7f2ed5b3e512'
STATIONARY = Path('/proc/nvt_thp_pen_stationary')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address', required=True, help='connected pen Bluetooth address')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--screen-on', action='store_true', help='send OEM 61 01 01')
    parser.add_argument('--query-state', action='store_true', help='send OEM 52 00')
    parser.add_argument('--forward-stationary', action='store_true',
                        help='forward only actual D2/53 01 state to the NVT driver')
    args = parser.parse_args()
    parts = args.address.split(':')
    if len(parts) != 6 or any(len(p) != 2 or any(c not in '0123456789abcdefABCDEF' for c in p) for p in parts):
        parser.error('address must contain six hexadecimal octets')
    if not 1 <= args.seconds <= 300:
        parser.error('seconds must be between 1 and 300')
    if args.forward_stationary and not STATIONARY.exists():
        parser.error('stationary driver interface is unavailable')

    from gi.repository import Gio, GLib
    connection = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    objects = connection.call_sync('org.bluez', '/', 'org.freedesktop.DBus.ObjectManager',
                                   'GetManagedObjects', None, None, Gio.DBusCallFlags.NONE,
                                   5000, None).unpack()[0]
    devices = [(path, interfaces['org.bluez.Device1']) for path, interfaces in objects.items()
               if 'org.bluez.Device1' in interfaces and
               interfaces['org.bluez.Device1'].get('Address', '').upper() == args.address.upper() and
               interfaces['org.bluez.Device1'].get('Connected') and
               interfaces['org.bluez.Device1'].get('Modalias') == 'bluetooth:v0022p5081d0001']
    if len(devices) != 1:
        raise RuntimeError('expected one connected Focus Pen Pro with this address')
    base, device = devices[0]
    characteristics = {}
    for path, interfaces in objects.items():
        if path.startswith(base + '/'):
            prop = interfaces.get('org.bluez.GattCharacteristic1', {})
            if prop.get('UUID') in (FE11, FE12):
                characteristics[prop['UUID']] = path
    if FE11 not in characteristics or FE12 not in characteristics:
        raise RuntimeError('Focus Pen Pro vendor characteristics are unavailable')
    write, notify = characteristics[FE11], characteristics[FE12]
    stopped = False
    last_state = None
    errors = []

    def emit(kind, **fields):
        print(json.dumps({'kind': kind, 'boottime_ns': time.clock_gettime_ns(time.CLOCK_BOOTTIME),
                          **fields}), flush=True)

    def call(path, method, parameters=None):
        return connection.call_sync('org.bluez', path, 'org.bluez.GattCharacteristic1',
                                    method, parameters, None, Gio.DBusCallFlags.NONE, 5000, None)

    def command(data):
        call(write, 'WriteValue', GLib.Variant('(aya{sv})',
             (bytes.fromhex(data), {'type': GLib.Variant('s', 'command')})))
        emit('command', hex=data)

    def changed(conn, sender, path, interface, member, parameters, userdata):
        nonlocal last_state, stopped
        _, properties, _ = parameters.unpack()
        if 'Value' not in properties:
            return
        value = bytes(properties['Value'])
        emit('vendor_notify', hex=value.hex())
        if (args.forward_stationary and len(value) == 3 and
                value[:2] in (b'\x53\x01', b'\xd2\x01') and
                value[2] in (0, 1) and last_state != value[2]):
            try:
                STATIONARY.write_text(str(value[2]) + '\n')
                last_state = value[2]
                emit('stationary_forwarded', value=last_state, source=value.hex())
            except OSError as error:
                errors.append(error)
                stopped = True
                emit('stationary_error', errno=error.errno)

    def interrupt(signum, frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    subscription = connection.signal_subscribe(
        'org.bluez', 'org.freedesktop.DBus.Properties', 'PropertiesChanged', notify,
        None, Gio.DBusSignalFlags.NONE, changed, None)
    subscribed = False
    try:
        call(notify, 'StartNotify')
        subscribed = True
        if args.screen_on:
            command('610101')
        if args.query_state:
            command('5200')
        emit('ready', forward_stationary=args.forward_stationary)
        deadline = time.monotonic() + args.seconds
        context = GLib.MainContext.default()
        while not stopped and time.monotonic() < deadline:
            while context.pending():
                context.iteration(False)
            time.sleep(.01)
    finally:
        if subscribed:
            try:
                call(notify, 'StopNotify')
            except GLib.Error as error:
                errors.append(error)
                emit('cleanup_error', message=str(error))
        connection.signal_unsubscribe(subscription)
        emit('stopped')
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
