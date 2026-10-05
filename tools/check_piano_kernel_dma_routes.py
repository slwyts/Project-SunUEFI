#!/usr/bin/env python3
"""Read actual kernel-owned per-master route ABI. No module load/MMIO/marker."""
import argparse
import json
import re
from pathlib import Path

DEVICES = {'a600000.ssusb': 0x40, '1d84000.ufshc': 0x60,
           'a00000.qcom,gpi-dma': 0xb6, 'ac0000.qcom,qupv3_1_geni_se': 0xa3,
           '800000.qcom,gpi-dma': 0x436, '8c0000.qcom,qupv3_2_geni_se': 0x423}
NODES = {'a600000.ssusb': '/soc/ssusb@a600000', '1d84000.ufshc': '/soc/ufshc@1d84000',
         'a00000.qcom,gpi-dma': '/soc/qcom,gpi-dma@a00000',
         'ac0000.qcom,qupv3_1_geni_se': '/soc/qcom,qupv3_1_geni_se@ac0000',
         '800000.qcom,gpi-dma': '/soc/qcom,gpi-dma@800000',
         '8c0000.qcom,qupv3_2_geni_se': '/soc/qcom,qupv3_2_geni_se@8c0000'}


def verify(text, sid):
    lines = text.splitlines()
    if not lines or lines[0] != 'piano-dma-route-v1 domain=identity ids=1' or len(lines) != 2:
        raise ValueError('missing exact kernel ABI/attached identity domain')
    tokens = lines[1].split()
    if len(tokens) != 11 or any(item.count('=') != 1 for item in tokens):
        raise ValueError('route ABI token count/shape mismatch')
    pairs = [item.split('=', 1) for item in tokens]
    if len({key for key, _ in pairs}) != len(pairs):
        raise ValueError('duplicate route ABI key')
    values = dict(pairs)
    expected_keys = {'sid', 'mask', 'slot', 'origin', 'smr', 's2cr', 'expected', 'cb', 'sctlr', 'cbar', 'fsr'}
    if set(values) != expected_keys or values['origin'] not in ('firmware-adopted', 'kernel-installed'):
        raise ValueError('route ABI fields/origin mismatch')
    for key in ('smr', 's2cr', 'expected', 'sctlr', 'cbar', 'fsr'):
        if not re.fullmatch('[0-9a-f]{8}', values[key]):
            raise ValueError('register must be exactly eight lowercase hex digits')
    for key in ('sid', 'mask'):
        if not re.fullmatch('[0-9a-f]{1,4}', values[key]):
            raise ValueError('SID/mask width or syntax mismatch')
    for key in ('slot', 'cb'):
        if not re.fullmatch('0|[1-9][0-9]{0,2}', values[key]):
            raise ValueError('slot/CB width or syntax mismatch')
    n = {key: int(value, 10 if key in ('slot', 'cb') else 16) for key, value in values.items() if key != 'origin'}
    if n['sid'] != sid or n['mask'] or n['slot'] < 0 or n['slot'] >= 128 or n['cb'] > 255:
        raise ValueError('master SID/mask/slot mismatch')
    if not n['smr'] & (1 << 31) or n['smr'] & 0xffff != sid or (n['smr'] >> 16) & 0x7fff:
        raise ValueError('actual valid SMR does not cover exact master')
    if (n['s2cr'] & 0x030300ff) != (n['expected'] & 0x030300ff):
        raise ValueError('hardware route differs from kernel private encoding')
    mode = (n['s2cr'] >> 16) & 3
    if mode == 0:
        if n['cb'] != n['s2cr'] & 0xff or n['sctlr'] & 1 or n['fsr'] & 0xc00001fe or (n['cbar'] >> 16) & 3 != 1:
            raise ValueError('emulated/adopted passthrough CB invalid/faulted')
    elif mode != 1:
        raise ValueError('route faults or has unknown translation mode')
    ownership = 'kernel-installed-route' if values['origin'] == 'kernel-installed' else 'kernel-attached-inherited-bypass'
    return dict(values, verified='kernel-private-and-hardware-readback', ownership=ownership, hardware_dma_tested=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--devices', nargs='*', choices=DEVICES, default=list(DEVICES))
    args = ap.parse_args()
    if not args.devices:
        ap.exit(1, 'no masters requested\n')
    report = {}
    try:
        for device in args.devices:
            directory = Path('/sys/bus/platform/devices') / device
            expected_node = Path('/sys/firmware/devicetree/base' + NODES[device])
            if (directory / 'of_node').resolve(strict=True) != expected_node.resolve(strict=True):
                raise ValueError('platform device is not the expected actual DT master: ' + device)
            path = directory / 'piano_dma_route'
            first = path.read_text()
            second = path.read_text()
            if first != second:
                raise ValueError('kernel route changed across reads: ' + device)
            report[device] = verify(first, DEVICES[device])
    except (OSError, ValueError) as error:
        ap.exit(1, str(error) + '\n')
    print(json.dumps({'route_readback': report, 'marker_written': False,
                      'dma_tested': False, 'memory_ownership_authorized': False}))


if __name__ == '__main__':
    main()
