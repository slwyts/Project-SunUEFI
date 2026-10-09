#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract numeric Piano/P81c calibration facts from a caller-supplied ini.

The output contains only the geometry, calibration and 100 mapping numbers
used by the open core. No proprietary library or complete firmware ini is
copied. Select by the touch controller's LCDid, not the display panel label.
"""
import argparse
import hashlib
import json
from pathlib import Path


GEOMETRY = {
    'hw.hal_row_num': 40, 'hw.hal_col_num': 60, 'hw.hal_node_num': 2400,
    'hw.display_x_resolution': 2136, 'hw.display_y_resolution': 3200,
    'hw.x_flip': 0, 'hw.y_flip': 0, 'hw.xy_flip': 0,
    'project_infor.super_resolution': 100,
    'stylus.stylus_num_col1': 12, 'stylus.stylus_num_row1': 40,
    'stylus.stylus_num_col2': 60, 'stylus.stylus_num_row2': 8,
    'stylus.stylus_en': 1,
}
COMMON = (
    'tilt_calibration_en', 'rx_l_edge_param', 'rx_r_edge_param',
    'tx_l_edge_param', 'tx_r_edge_param', 'limit_rx_start', 'limit_rx_end',
    'limit_tx_start', 'limit_tx_end', 'up_threshold', 'ring_threshold',
    'kalman_q_div_min', 'kalman_q_div_max', 'kalman_lamda_div_min',
    'kalman_lamda_div_max', 'first_jitter_lock_dis', 'stable_jitter_lock_dis',
    'move_jitter_lock_dis',
)
PROFILE = ('tilt_calibration_thd', 'tilt_calibration_rate', 'tip_slope_rx', 'tip_slope_tx')


def parse_ini(raw):
    result = {}
    section = logical = ''
    for line in raw.decode('utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        continuation = line.endswith('\\')
        if continuation:
            line = line[:-1]
        logical += line
        if continuation or ('{' in logical and '}' not in logical):
            continue
        if '=' in logical:
            key, value = logical.split('=', 1)
            full = section + '.' + key.strip()
            if full in result:
                raise ValueError('duplicate ini key: ' + full)
            result[full] = value.strip()
        else:
            section = logical.strip()
        logical = ''
    if logical:
        raise ValueError('incomplete ini')
    return result


def integer(fields, key):
    text = fields[key].strip()
    if not text or any(c not in '-+0123456789' for c in text):
        raise ValueError('not an integer: ' + key)
    number = int(text, 10)
    if not -(1 << 31) <= number < (1 << 31):
        raise ValueError('integer outside core range: ' + key)
    return number


def array(fields, key, size):
    text = fields[key]
    if not text.startswith('{') or not text.endswith('}'):
        raise ValueError('not an ini array: ' + key)
    parts = text[1:-1].split(',')
    # Match the core parser's accepted trailing comma in multiline OEM maps.
    if parts and parts[-1] == '':
        parts.pop()
    values = [integer({'value': v}, 'value') for v in parts]
    if len(values) != size:
        raise ValueError('wrong array length: ' + key)
    return values


def extract(raw):
    if len(raw) > 1024 * 1024:
        raise ValueError('ini exceeds core input limit')
    fields = parse_ini(raw)
    output = {'calibration.format_version': 1}
    for key, expected in GEOMETRY.items():
        if integer(fields, key) != expected:
            raise ValueError('not supported Piano geometry: ' + key)
        output[key] = expected
    prefixes = [p for p in ('stylus', 'stylus_2', 'stylus_3')
                if 'stylus.' + p + '_vendor_id' in fields and
                integer(fields, 'stylus.' + p + '_vendor_id') == 3]
    if len(prefixes) != 1:
        raise ValueError('expected one P81c/vendor3 calibration')
    prefix = prefixes[0]
    output['stylus.stylus_vendor_id'] = 3
    for suffix in COMMON:
        key = 'stylus.stylus_' + suffix
        output[key] = integer(fields, key)
    for suffix in PROFILE:
        key = 'stylus.' + prefix + '_' + suffix
        if key not in fields:
            key = 'stylus.stylus_' + suffix
        output['stylus.stylus_' + suffix] = integer(fields, key)
    output['stylus.stylus_coor_diff'] = array(fields, 'stylus.' + prefix + '_coor_diff', 6)
    output['stylus.stylus_angle'] = array(fields, 'stylus.stylus_angle', 6)
    mx = array(fields, 'mapping.default_mapping_x', 2400)
    my = array(fields, 'mapping.default_mapping_y', 2400)
    x = [mx[row * 60] for row in range(40)]
    y = my[:60]
    if any(mx[row * 60 + col] != x[row] or my[row * 60 + col] != y[col]
           for row in range(40) for col in range(60)):
        raise ValueError('expanded mapping is not separable into40/60 axes')
    for values, extent in ((x, 2136), (y, 3200)):
        if values[0] < 0 or values[-1] >= extent or any(a >= b for a, b in zip(values, values[1:])):
            raise ValueError('mapping outside Piano bounds or not increasing')
    output['mapping.mapping_40'] = x
    output['mapping.mapping_60'] = y
    return output


def format_facts(values, rom, source, digest, panel):
    if any('\n' in value or '\r' in value for value in (rom, source)):
        raise ValueError('source metadata must be one line')
    lines = [
        '# SPDX-License-Identifier: MIT',
        '# Project SunUEFI numeric pen calibration facts; format1.',
        '# Source-ROM: ' + rom,
        '# Source-file: ' + source,
        '# Source-SHA256: ' + digest,
        '# Touch-LCDid: ' + ('0 (CSOT)' if panel == 'csot' else '1 (BOE)'),
        '# Only open-core numeric inputs, not the complete original configuration.',
    ]
    previous = None
    for key, value in values.items():
        section, name = key.split('.', 1)
        if section != previous:
            lines += ['', section]
            previous = section
        encoded = '{' + ','.join(map(str, value)) + '}' if isinstance(value, list) else str(value)
        lines.append('    ' + name + '=' + encoded)
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ini', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--touch-panel', choices=('boe', 'csot'), required=True)
    parser.add_argument('--source-rom', required=True)
    parser.add_argument('--source-file', help='Original ROM path; defaults to the input basename')
    args = parser.parse_args()
    try:
        raw = args.ini.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        values = extract(raw)
        text = format_facts(values, args.source_rom, args.source_file or args.ini.name,
                            digest, args.touch_panel)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(json.dumps({'output': str(args.output), 'touch_lcd_id': 0 if args.touch_panel == 'csot' else 1,
                          'source_rom': args.source_rom, 'source_sha256': digest,
                          'sha256': hashlib.sha256(text.encode()).hexdigest(),
                          'scalar_keys': sum(not isinstance(v, list) for v in values.values()),
                          'mapping_numbers': 100}, indent=2))
    except (OSError, KeyError, ValueError) as error:
        parser.exit(2, str(error) + '\n')


if __name__ == '__main__':
    main()
