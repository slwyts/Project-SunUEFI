#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Initialize a connected Piano Focus Pen Pro through its original BLE protocol.

The supplied board profile contains radio parameters, never pairing keys or
device identities. This client does not read THP frames or generate input.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import struct
import time

FE11 = '0000fe11-aa6c-462a-964a-7f2ed5b3e512'
FE12 = '0000fe12-aa6c-462a-964a-7f2ed5b3e512'


def read_dock():
    events = []
    for event in Path('/sys/class/input').glob('event*'):
        try:
            if (event / 'device/name').read_text().strip() == 'Xiaomi Piano Pen Dock':
                events.append(event)
        except FileNotFoundError:
            continue
    if len(events) != 1:
        raise RuntimeError('Piano pen-dock switch is unavailable')
    fd = os.open('/dev/input/' + events[0].name,
                 os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        bits = bytearray(8)
        fcntl.ioctl(fd, 0x8008451b, bits, True)  # EVIOCGSW(8)
        return bool(int.from_bytes(bits, 'little') & (1 << 15))
    finally:
        os.close(fd)


def profile(path):
    data = json.loads(path.read_text())
    if data.get('version') != 1 or data.get('device') != 'piano':
        raise ValueError('unsupported pen radio profile')
    lengths = {'parameter_prefix': 1, 'timing': 7, 'first_id_cycle': 1,
               'second_id_cycle': 1, 'intervals': 12, 'parameter_tail': 2,
               'frequency': 4, 'frequency_interval': 2, 'voltage': 2}
    for key, size in lengths.items():
        data[key] = bytes.fromhex(data[key])
        if len(data[key]) != size:
            raise ValueError('invalid ' + key + ' size')
    if type(data['double_tap']) is not bool:
        raise ValueError('double_tap must be boolean')
    if type(data['squeeze_motor_level']) is not int or not 0 <= data['squeeze_motor_level'] <= 3:
        raise ValueError('invalid squeeze motor level')
    for key in ('squeeze_threshold_down', 'squeeze_threshold_up'):
        if type(data[key]) is not int or not 0 <= data[key] <= 65535:
            raise ValueError('invalid squeeze threshold')
    return data


class Pen:
    def __init__(self, address, stationary):
        from gi.repository import Gio, GLib
        self.Gio, self.GLib = Gio, GLib
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        objects = self.bus.call_sync('org.bluez', '/', 'org.freedesktop.DBus.ObjectManager',
                                     'GetManagedObjects', None, None,
                                     Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
        devices = [(p, o['org.bluez.Device1']) for p, o in objects.items()
                   if 'org.bluez.Device1' in o and
                   o['org.bluez.Device1'].get('Address', '').upper() == address.upper() and
                   o['org.bluez.Device1'].get('Connected') and
                   o['org.bluez.Device1'].get('Modalias') == 'bluetooth:v0022p5081d0001']
        if len(devices) != 1:
            raise RuntimeError('expected one connected Focus Pen Pro')
        self.base, device = devices[0]
        adapter = objects[device['Adapter']]['org.bluez.Adapter1']
        self.host_address = bytes.fromhex(adapter['Address'].replace(':', ''))
        if len(self.host_address) != 6:
            raise RuntimeError('invalid actual adapter address')
        chars = {o['org.bluez.GattCharacteristic1']['UUID']: p for p, o in objects.items()
                 if p.startswith(self.base + '/') and 'org.bluez.GattCharacteristic1' in o}
        self.write, self.notify = chars[FE11], chars[FE12]
        self.events = []
        self.serial = 0
        self.stationary = stationary
        self.last_stationary = None
        self.last_dock = None
        self.stop = False
        self.failure = None
        self.subscription = None
        self.notifying = False

    def log(self, kind, **fields):
        print(json.dumps({'kind': kind, 'boottime_ns': time.clock_gettime_ns(time.CLOCK_BOOTTIME),
                          **fields}), flush=True)

    def call(self, path, method, args=None):
        return self.bus.call_sync('org.bluez', path, 'org.bluez.GattCharacteristic1', method,
                                 args, None, self.Gio.DBusCallFlags.NONE, 5000, None)

    def changed(self, connection, sender, path, interface, member, args, userdata):
        _, fields, _ = args.unpack()
        if 'Value' not in fields:
            return
        value = bytes(fields['Value'])
        self.serial += 1
        self.events.append((self.serial, value))
        self.events = self.events[-64:]
        self.log('reply', hex=value.hex())
        if (self.stationary and len(value) == 3 and value[:2] in (b'\xd2\x01', b'\x53\x01')
                and value[2] in (0, 1) and value[2] != self.last_stationary):
            try:
                self.stationary.write_text(str(value[2]) + '\n')
                self.last_stationary = value[2]
                self.log('stationary_forwarded', value=value[2])
            except OSError as error:
                self.failure = error

    def pump(self):
        context = self.GLib.MainContext.default()
        while context.pending():
            context.iteration(False)
        if self.failure:
            raise self.failure
        if self.stop:
            raise InterruptedError('pen initialization interrupted')

    def request(self, payload, response, size=None):
        self.pump()
        before = self.serial
        self.call(self.write, 'WriteValue', self.GLib.Variant('(aya{sv})',
                  (payload, {'type': self.GLib.Variant('s', 'command')})))
        self.log('request', hex=payload.hex())
        until = time.monotonic() + 3
        while time.monotonic() < until:
            self.pump()
            for serial, value in self.events:
                if serial > before and value.startswith(response) and (size is None or len(value) == size):
                    return value
            time.sleep(.01)
        raise TimeoutError('no response to opcode ' + payload[:1].hex())

    def open(self):
        self.subscription = self.bus.signal_subscribe(
            'org.bluez', 'org.freedesktop.DBus.Properties', 'PropertiesChanged', self.notify,
            None, self.Gio.DBusSignalFlags.NONE, self.changed, None)
        self.call(self.notify, 'StartNotify')
        self.notifying = True

    def initialize(self, p):
        self.request(b'\x52\x00', b'\xd2\x01', 3)
        self.request(b'\x00\x00', b'\x80\x01', 3)
        self.request(b'\x01\x00', b'\x81\x02', 4)
        self.request(b'\x02\x00', b'\x82\x02', 4)
        self.request(b'\x03\x00', b'\x83\x03', 5)
        assigned = []
        for cycle in ('first_id_cycle', 'second_id_cycle'):
            pen_id = self.request(b'\x04\x00', b'\x84\x01', 3)[2:3]
            assigned.append(pen_id[0])
            parameters = (self.host_address + p['parameter_prefix'] + pen_id + p['timing'] +
                          p[cycle] + p['intervals'] + p['parameter_tail'])
            self.request(b'\x30' + bytes([len(parameters)]) + parameters, b'\xb0\x00', 2)
            self.request(b'\x31\x08' + pen_id + b'\x03' + p['frequency'] +
                         p['frequency_interval'], b'\xb1\x00', 2)
            self.request(b'\x32\x03' + pen_id + p['voltage'], b'\xb2\x00', 2)
        # The protocol uses Unix seconds; report/frame timing uses BOOTTIME.
        self.request(b'\x33\x04' + struct.pack('>I', int(time.time()) & 0xffffffff), b'\xb3\x00', 2)
        attached = read_dock()
        self.request(b'\x51\x01' + bytes([0 if attached else 1]), b'\xd1\x00', 2)
        self.last_dock = attached
        self.request(b'\x5f\x01' + bytes([int(p['double_tap'])]), b'\xdf\x01\x01', 3)
        self.request(b'\x5a\x01' + bytes([p['squeeze_motor_level']]), b'\xda\x01\x01', 3)
        self.request(b'\x5c\x04' + struct.pack('>HH', p['squeeze_threshold_down'],
                     p['squeeze_threshold_up']), b'\xdc\x01\x01', 3)
        self.request(b'\x61\x01\x01', b'\xe1\x00', 2)
        self.log('initialized', assigned_ids=assigned, attached=attached,
                 drawing_not_established=True)

    def close(self):
        try:
            if self.notifying:
                try:
                    self.call(self.notify, 'StopNotify')
                except self.GLib.Error as error:
                    self.log('notification_cleanup_error', message=str(error))
        finally:
            if self.subscription is not None:
                self.bus.signal_unsubscribe(self.subscription)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address', required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--stationary-device', type=Path)
    parser.add_argument('--observe-seconds', type=float, default=10)
    args = parser.parse_args()
    if not 0 <= args.observe_seconds <= 300:
        parser.error('observe-seconds must be between 0 and 300')
    p = profile(args.profile)
    pen = Pen(args.address, args.stationary_device)
    def stop(signum, frame):
        pen.stop = True
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        pen.open()
        pen.initialize(p)
        until = time.monotonic() + args.observe_seconds
        while not pen.stop and time.monotonic() < until:
            pen.pump()
            time.sleep(.01)
    finally:
        pen.close()


if __name__ == '__main__':
    main()
