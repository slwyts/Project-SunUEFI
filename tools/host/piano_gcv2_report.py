#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Summarize actual read-only KMS snapshot and kernel log; never set a LUT."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot', type=Path, required=True, help='piano_drm_snapshot --blobs JSON')
    parser.add_argument('--kernel-log', type=Path, required=True, help='Actual dmesg/journal kernel text')
    parser.add_argument('--since-uptime', type=float, help='Only timestamped kernel messages after a probe start')
    args = parser.parse_args()
    snapshot = json.loads(args.snapshot.read_text())
    crtcs = []
    for obj in snapshot['objects']:
        if obj.get('kind') != 'crtc':
            continue
        props = {row['name']: row for row in obj.get('properties', []) if 'name' in row}
        row = {'kms_id': obj['id'], 'active': props.get('ACTIVE', {}).get('value'),
               'gamma_property_present': 'GAMMA_LUT' in props,
               'gamma_size': props.get('GAMMA_LUT_SIZE', {}).get('value')}
        blob = props.get('GAMMA_LUT', {}).get('blob')
        if blob and 'hex' in blob:
            data = bytes.fromhex(blob['hex'])
            row['blob'] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            if len(data) == 1024 * 8:
                values = list(struct.iter_unpack('<4H', data))
                row['blob']['channel_summary'] = [
                    {'first': values[0][n], 'last': values[-1][n],
                     'monotonic': all(a[n] <= b[n] for a, b in zip(values, values[1:]))}
                    for n in range(3)]
                row['blob']['reserved_zero'] = all(value[3] == 0 for value in values)
        crtcs.append(row)
    messages = []
    for line in args.kernel_log.read_text().splitlines():
        if not any(term in line for term in ['GC2', 'REGDMA', 'SMMU', 'smmu', 'iommu fault']):
            continue
        timestamp = re.search(r'\[\s*(\d+\.\d+)\]', line)
        if args.since_uptime is not None:
            if timestamp is None or float(timestamp[1]) < args.since_uptime:
                continue
        messages.append(line)
    print(json.dumps({'snapshot_read_only': snapshot.get('read_only'),
                      'snapshot_globally_atomic': snapshot.get('globally_atomic_snapshot'),
                      'snapshot_errors': snapshot.get('errors'), 'crtcs': crtcs,
                      'kernel_messages': messages,
                      'completion_messages': [line for line in messages if 'REGDMA completed' in line],
                      'failure_messages': [line for line in messages if any(term in line.lower() for term in
                                           ['failure', 'failed', 'unconfirmed', 'fault', 'timeout'])],
                      'hardware_result': 'Review actual completion, target mask, optical output and retirement; no automatic pass',
                      'screenshots': 'Compositor capture is before hardware Gamma and cannot prove optical warmth'}, indent=2))


if __name__ == '__main__':
    main()
