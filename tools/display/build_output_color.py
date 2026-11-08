#!/usr/bin/env python3
"""Derive Piano display conversion firmware from the stock QDCM calibration."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

# Linear BT.2020 RGB to P3-D65 RGB, with no chromatic adaptation.
BT2020_TO_P3 = (
    (1.3435783, -0.2821797, -0.0613986),
    (-0.0652975, 1.0757879, -0.0104905),
    (0.0028218, -0.0195985, 1.0167767),
)
PANELS = {'boe': (1, '42_02_0a'), 'csot': (2, '35_02_0b')}


def decode_payload(value):
    raw = bytes.fromhex(value)
    decoded = bytearray(raw)
    start = len(raw) % 2
    decoded[start::2], decoded[start + 1::2] = raw[start + 1::2], raw[start::2]
    return decoded.decode()


def pq_eotf(value):
    power = value ** (32 / 2523)
    return (max(power - 3424 / 4096, 0) /
            (2413 / 128 - 2392 / 128 * power)) ** (16384 / 2610) * 10000


def interpolate(table, rgb):
    values = [min(max(x, 0), 1) * 16 for x in rgb]
    base = [min(int(x), 15) for x in values]
    fraction = [x - n for x, n in zip(values, base)]
    result = [0.0, 0.0, 0.0]
    for b in (0, 1):
        for g in (0, 1):
            for r in (0, 1):
                corners = (r, g, b)
                weight = math.prod(f if c else 1 - f for c, f in zip(corners, fraction))
                index = base[0] + r + (base[1] + g) * 17 + (base[2] + b) * 289
                for channel in range(3):
                    result[channel] += weight * table[index * 3 + channel]
    return [min(max(round(x), 0), 4095) for x in result]


def build(source, output, vendor):
    panel_id, model = PANELS[vendor]
    data = json.loads(source.read_text())
    panel_name, = [key for key in data if key.startswith('xiaomi_')]
    expected = 'xiaomi_p81_' + model + '_vid_mode_dualdsi_dsc_panel'
    if panel_name != expected:
        raise ValueError('Calibration does not match the selected panel: ' + vendor)
    mode = {key: json.loads(decode_payload(data[panel_name]['native_hdr'][key]))
            for key in ('PostBlendGC', 'PostBlendGamut', 'PostBlendHdrBlob')}
    hdr = ET.fromstring(mode['PostBlendHdrBlob']['blob'])
    peak = float(hdr.findtext('panel_max_luminance'))
    black = float(hdr.findtext('panel_min_luminance'))
    if (hdr.findtext('panel_color_mode') != 'DCIP3' or
            hdr.findtext('output_gamma_space/space') != '2p2' or
            not 400 < peak < 1000 or not 0 <= black < 2):
        raise ValueError('Unsupported stock panel encoding or luminance')
    payload = bytearray()
    # PQ is absolute; the compositor tone maps to the reported panel peak.
    # BT.2020 SDR uses the same calibration after its gamma-2.2 EOTF.
    for transfer in (lambda x: min(pq_eotf(x) / peak, 1), lambda x: x ** 2.2):
        for i in range(1025):
            value = round(transfer(i / 1024) * 65535)
            payload += struct.pack('<4H', value, value, value, 0)
    gc = json.loads(decode_payload(data['Common']['PostBlendGC']))['Custom'][
        str(mode['PostBlendGC']['Custom'])]
    for channel in ('lutG', 'lutB', 'lutR'):
        values = [int(x) for x in gc[channel].split(',') if x.strip()]
        if len(values) != 1280 or any(not 0 <= x <= 1023 for x in values):
            raise ValueError('Invalid stock high-precision gamma table')
        for i in range(0, len(values), 2):
            payload += struct.pack('<I', values[i] | values[i + 1] << 16)
    gamut = [int(x) for x in json.loads(decode_payload(data['Common']['PostBlendGamut']))[
        'Custom'][str(mode['PostBlendGamut']['Custom'])].split(',') if x.strip()]
    if len(gamut) != 4913 * 3 or any(not 0 <= x <= 4095 for x in gamut):
        raise ValueError('Invalid stock 17-cube gamut table')
    for b in range(17):
        for g in range(17):
            for r in range(17):
                rgb = (r / 16, g / 16, b / 16)
                p3 = [sum(value * coefficient for value, coefficient in zip(rgb, row))
                      for row in BT2020_TO_P3]
                payload += struct.pack('<3H', *interpolate(gamut, p3))
    header = struct.pack('<8s6I', b'DPUCOLR1', 1, panel_id, round(peak * 1000),
                         round(black * 1000), 32 + len(payload), 0)
    output.write_bytes(header + payload)
    return {'bytes': output.stat().st_size,
            'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
            'source': '/odm/etc/display/' + source.name,
            'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'mode': 'native_hdr', 'peak_luminance': peak, 'min_luminance': black}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boe-source', type=Path, required=True)
    parser.add_argument('--csot-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rom', required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Use a fresh output directory; existing firmware is preserved')
    args.output.mkdir(parents=True)
    files = {f'piano-{vendor}-display.bin': build(
        getattr(args, vendor + '_source'), args.output / f'piano-{vendor}-display.bin', vendor)
        for vendor in PANELS}
    record = {'schema_version': 1, 'component': 'piano-display-calibration',
              'rom': args.rom, 'directory': 'qcom/sm8750', 'files': files}
    (args.output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
