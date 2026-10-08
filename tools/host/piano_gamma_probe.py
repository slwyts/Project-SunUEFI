#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Inspect Mutter gamma; short probes save and restore its non-empty real ramps.

Run as the active GNOME user. Default is read-only. This uses the published
DisplayConfig GetResources/GetCrtcGamma/SetCrtcGamma ABI, no DRM-master takeover,
mode change, DPMS, brightness setting, IRQ synthesis or device MMIO.
"""
import argparse
import hashlib
import json
from pathlib import Path
import signal
import struct
import time

DEST = 'org.gnome.Mutter.DisplayConfig'
PATH = '/org/gnome/Mutter/DisplayConfig'


def digest(ramp):
    return hashlib.sha256(struct.pack('<' + str(len(ramp)) + 'H', *ramp)).hexdigest()


def summary(ramps):
    return [{'entries': len(row), 'sha256_le16': digest(row),
             'first': row[0] if row else None, 'last': row[-1] if row else None} for row in ramps]


def bypass_backend(snapshot_path, kms_id):
    if snapshot_path is None:
        raise ValueError('Empty gamma is bypass; provide --drm-snapshot with the actual backend size')
    data = snapshot_path.read_bytes()
    snapshot = json.loads(data)
    crtcs = [row for row in snapshot['objects'] if row['kind'] == 'crtc' and row['id'] == kms_id]
    if snapshot.get('errors') != 0 or snapshot.get('atomic_cap_errno') != 0 or len(crtcs) != 1:
        raise ValueError('DRM snapshot is incomplete or does not identify the same atomic KMS CRTC')
    props = {prop['name']: prop['value'] for prop in crtcs[0]['properties']}
    if (props.get('ACTIVE'), props.get('GAMMA_LUT'), props.get('GAMMA_LUT_SIZE')) != (1, 0, 1024):
        raise ValueError('Actual CRTC must be active with GAMMA_LUT=0 and GAMMA_LUT_SIZE=1024')
    return {'size': props['GAMMA_LUT_SIZE'], 'state': 'bypass', 'kms_id': kms_id,
            'snapshot': str(snapshot_path), 'snapshot_sha256': hashlib.sha256(data).hexdigest(),
            'device': snapshot.get('device')}


class Gamma:
    def __init__(self):
        from gi.repository import Gio, GLib
        self.Gio, self.GLib = Gio, GLib
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(self, method, signature=None, values=()):
        params = self.GLib.Variant(signature, values) if signature else None
        return self.bus.call_sync(DEST, PATH, DEST, method, params, None,
                                  self.Gio.DBusCallFlags.NONE, 5000, None).unpack()

    def resources(self):
        data = self.call('GetResources')
        crtcs = [{'api_id': row[0], 'kms_id': row[1], 'width': row[4],
                  'height': row[5], 'mode': row[6], 'active': row[6] >= 0 and row[4] > 0 and row[5] > 0}
                 for row in data[1]]
        return data[0], crtcs

    def get(self, serial, crtc):
        ramps = [list(row) for row in self.call('GetCrtcGamma', '(uu)', (serial, crtc))]
        if len(ramps) != 3 or len({len(row) for row in ramps}) != 1:
            raise ValueError('Actual Mutter reply has unequal/missing gamma channels')
        return ramps

    def set(self, serial, crtc, ramps):
        self.call('SetCrtcGamma', '(uuaqaqaq)', (serial, crtc, *ramps))

    def restore(self, original):
        if not original['ramps'][0]:
            raise RuntimeError('Mutter 48.7 empty-LUT restore crashed the real session; bypass restore is disabled pending its fix')
        serial, crtcs = self.resources()
        candidates = [row for row in crtcs if row['kms_id'] == original['kms_id'] and row['active']]
        if len(candidates) != 1:
            raise RuntimeError('Original KMS CRTC no longer active; no restore to a guessed output')
        self.set(serial, candidates[0]['api_id'], original['ramps'])
        actual = self.get(serial, candidates[0]['api_id'])
        return {'request_returned': True, 'software_readback_matches': actual == original['ramps'],
                'hardware_result': 'requires kernel completion and optical evidence'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--crtc', type=int, help='Actual Mutter API ID, from the read-only listing')
    parser.add_argument('--probe', choices=('identity', 'warm'), help='Explicit temporary Gamma change')
    parser.add_argument('--backup', type=Path, help='Required new file for the original real ramps')
    parser.add_argument('--drm-snapshot', type=Path,
                        help='Actual atomic DRM snapshot explaining empty gamma capacity; does not permit bypass writes')
    parser.add_argument('--restore-from', type=Path, help='Explicit recovery of an earlier saved ramp')
    parser.add_argument('--hold-seconds', type=float, default=4)
    args = parser.parse_args()
    if not 0 <= args.hold_seconds <= 30:
        parser.error('hold duration must be 0..30 seconds')
    if args.probe and (args.backup is None or args.restore_from):
        parser.error('an explicit probe requires --backup and cannot combine with --restore-from')
    client = Gamma()
    if args.restore_from:
        saved = json.loads(args.restore_from.read_text())
        result = client.restore(saved)
        print(json.dumps({'operation': 'explicit_restore', **result}, indent=2))
        if not result['software_readback_matches']:
            raise SystemExit(2)
        return
    serial, crtcs = client.resources()
    active = [row for row in crtcs if row['active'] and (args.crtc is None or row['api_id'] == args.crtc)]
    report = {'serial': serial, 'crtcs': crtcs, 'read_only': not bool(args.probe),
              'hardware_result': 'not established by a D-Bus reply'}
    for row in active:
        ramps = client.get(serial, row['api_id'])
        row['gamma'] = summary(ramps)
        if not args.probe and not ramps[0] and args.drm_snapshot:
            row['backend'] = bypass_backend(args.drm_snapshot, row['kms_id'])
    if not args.probe:
        print(json.dumps(report, indent=2))
        return
    if len(active) != 1:
        parser.error('choose exactly one actual active CRTC with --crtc; no guessed target')
    crtc = active[0]
    original = client.get(serial, crtc['api_id'])
    count = len(original[0])
    if count == 0:
        bypass_backend(args.drm_snapshot, crtc['kms_id'])
        raise ValueError('Actual bypass is supported, but this Mutter 48.7 cannot safely restore it; no probe sent')
    if count != 1024:
        raise ValueError('Piano GCv2 probe requires the actual standard 1024-entry Gamma backend')
    saved = {'mutter_api_id': crtc['api_id'], 'kms_id': crtc['kms_id'], 'ramps': original,
             'source': 'actual Mutter GetCrtcGamma reply', 'summary': summary(original)}
    args.backup.parent.mkdir(parents=True, exist_ok=True)
    with args.backup.open('x') as stream:
        json.dump(saved, stream)
        stream.write('\n')
    identity = [round(i * 65535 / (count - 1)) for i in range(count)]
    factors = (1.0, 1.0, 1.0) if args.probe == 'identity' else (1.0, 0.8, 0.6)
    ramps = [[round(value * factor) for value in identity] for factor in factors]
    # Warm is a defined channel-gain probe, not a calibrated Kelvin value.
    report.update(operation=args.probe, channel_gains=factors, requested=summary(ramps),
                  backup=str(args.backup), start_uptime=float(Path('/proc/uptime').read_text().split()[0]))
    def interrupted(signum, frame):
        raise KeyboardInterrupt('signal ' + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    requested = False
    try:
        # Even an uncertain reply may have changed the compositor state.
        requested = True
        client.set(serial, crtc['api_id'], ramps)
        report['request_returned'] = True
        report['software_readback_matches'] = client.get(serial, crtc['api_id']) == ramps
        time.sleep(args.hold_seconds)
    except BaseException as error:
        report['probe_error'] = str(error)
    finally:
        if requested:
            try:
                report['restore'] = client.restore(saved)
            except Exception as error:
                report['restore_error'] = str(error)
        report['end_uptime'] = float(Path('/proc/uptime').read_text().split()[0])
        print(json.dumps(report, indent=2))
    if ('probe_error' in report or 'restore_error' in report or
            report.get('software_readback_matches') is not True or
            report.get('restore', {}).get('software_readback_matches') is not True):
        raise SystemExit(2)


if __name__ == '__main__':
    main()
