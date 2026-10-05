#!/usr/bin/env python3
"""Validate actual kernel-managed identity and stage1 DMA context evidence.

No modules, maps, block access or ready marker. The kernel compares stage1
registers to its own live domain and serialized group/context state before
exposing this read-only ABI. This is configuration evidence, not a DMA test.
"""
import argparse
import json
from pathlib import Path
import re

SCOPES = {
    'mdss-prebind': ('ae00000.display-subsystem', '/soc/display-subsystem@ae00000', 'identity', [(0x800, 2)]),
    'mdss': ('ae00000.display-subsystem', '/soc/display-subsystem@ae00000', 'stage1', [(0x800, 2)]),
    'gpu': ('3d00000.gpu', '/soc/gpu@3d00000', 'stage1', [(0, 0), (1, 0)]),
    'gmu': ('3d6c000.gmu', '/soc/gmu@3d6c000', 'stage1', [(5, 0)]),
    'video': ('aa00000.video-codec-ml', '/soc/video-codec-ml@aa00000', 'stage1', [(0x1940, 0), (0x1947, 0)]),
    'camera': ('ad27000.isp-ml', '/soc/isp-ml@ad27000', 'stage1', [(0x1c00, 0)]),
}
BASE_FIELDS = {'sid', 'mask', 'slot', 'origin', 'smr', 's2cr', 'expected', 'cb', 'sctlr', 'cbar', 'fsr'}
CONTEXT_FIELDS = {'ttbr0', 'ttbr1', 'tcr', 'tcr2', 'mair0', 'mair1'}


def fields(line, context):
    tokens = line.split()
    wanted = BASE_FIELDS | (CONTEXT_FIELDS if context else set())
    if len(tokens) != len(wanted) or any(token.count('=') != 1 for token in tokens):
        raise ValueError('context ABI token count or syntax differs')
    pairs = [token.split('=', 1) for token in tokens]
    row = dict(pairs)
    if len(row) != len(pairs) or set(row) != wanted:
        raise ValueError('context ABI duplicated or unknown fields')
    if row['origin'] not in ('kernel-installed', 'firmware-adopted'):
        raise ValueError('unknown route origin')
    numeric = {}
    for name, value in row.items():
        if name == 'origin':
            continue
        syntax = '0|[1-9][0-9]{0,2}' if name in ('slot', 'cb') else '[0-9a-f]{1,4}' if name in ('sid', 'mask') else '[0-9a-f]{16}' if name in ('ttbr0', 'ttbr1') else '[0-9a-f]{8}'
        if not re.fullmatch(syntax, value):
            raise ValueError('context ABI numeric width/syntax differs: ' + name)
        numeric[name] = int(value, 10 if name in ('slot', 'cb') else 16)
    if numeric['slot'] >= 128 or numeric['cb'] > 255:
        raise ValueError('stream slot or context bank out of range')
    return row, numeric


def verify(text, mode, pairs):
    context = mode == 'stage1'
    if mode not in ('identity', 'stage1') or not pairs or len(pairs) > (8 if context else 16):
        raise ValueError('unsupported declared domain/scope')
    lines = text.splitlines()
    kind = 'context' if context else 'route'
    if len(lines) != len(pairs) + 1 or lines[0] != f'piano-dma-{kind}-v1 domain={mode} ids={len(pairs)}':
        raise ValueError('missing exact kernel domain/scope ABI')
    result = []
    common = None
    for line, (sid, mask) in zip(lines[1:], pairs):
        row, n = fields(line, context)
        if n['sid'] != sid or n['mask'] != mask:
            raise ValueError('wrong consumer SID/mask')
        route_sid, route_mask = n['smr'] & 0xffff, (n['smr'] >> 16) & 0x7fff
        if not n['smr'] & 0x80000000 or mask & ~route_mask or (sid & ~route_mask) != (route_sid & ~route_mask):
            raise ValueError('hardware SMR does not cover the actual consumer')
        if n['s2cr'] & 0x030300ff != n['expected'] & 0x030300ff:
            raise ValueError('hardware route differs from kernel-private encoding')
        route_mode = (n['s2cr'] >> 16) & 3
        if route_mode == 0:
            if n['cb'] != n['s2cr'] & 0xff or (n['cbar'] >> 16) & 3 != 1 or n['fsr'] & 0xc00001fe:
                raise ValueError('context bank routing or fault state differs')
            if bool(n['sctlr'] & 1) != context:
                raise ValueError('translation enable differs from the actual domain')
        elif route_mode != 1 or context:
            raise ValueError('unknown or unexpected bypass route')
        if context:
            if row['origin'] != 'kernel-installed':
                raise ValueError('stage1 domain is not kernel-installed')
            enabled = [not n['tcr'] & 0x80, not n['tcr'] & 0x800000]
            if not any(enabled) or any(on and not n[f'ttbr{index}'] & 0x0000fffffffff000 for index, on in enumerate(enabled)):
                raise ValueError('enabled stage1 table has no physical root')
            snapshot = tuple(n[key] for key in ('cb', 'sctlr', 'cbar', 'fsr', 'ttbr0', 'ttbr1', 'tcr', 'tcr2', 'mair0', 'mair1'))
            if common is not None and snapshot != common:
                raise ValueError('one master has inconsistent context bank snapshots')
            common = snapshot
        result.append(dict(row, domain=mode, configuration_readback_verified=True, dma_transfer_verified=False))
    return result


def read_scope(scope, sysfs=Path('/sys')):
    device, node, mode, pairs = SCOPES[scope]
    directory = sysfs / 'bus/platform/devices' / device
    expected = sysfs / ('firmware/devicetree/base' + node)
    if (directory / 'of_node').resolve(strict=True) != expected.resolve(strict=True):
        raise ValueError('kernel device does not match the exact DT consumer')
    provider_node = '/soc/iommu@3da0000' if scope in ('gpu', 'gmu') else '/soc/iommu@15000000'
    provider = sysfs / ('firmware/devicetree/base' + provider_node)
    handle = (provider / 'phandle').read_bytes()
    if len(handle) != 4 or handle == bytes(4) or (provider / '#iommu-cells').read_bytes() != b'\0\0\0\2':
        raise ValueError('actual IOMMU provider shape differs')
    wanted = b''.join(handle + sid.to_bytes(4, 'big') + mask.to_bytes(4, 'big') for sid, mask in pairs)
    if (expected / 'iommus').read_bytes() != wanted:
        raise ValueError('consumer does not reference the expected real IOMMU provider')
    attr = 'piano_dma_context' if mode == 'stage1' else 'piano_dma_route'
    first, second = (directory / attr).read_text(), (directory / attr).read_text()
    if first != second:
        raise ValueError('context changed across fresh kernel reads')
    return {'scope': scope, 'domain': mode, 'routes': verify(first, mode, pairs),
            'marker_written': False, 'memory_ownership_authorized': False, 'dma_transfer_verified': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scope', choices=SCOPES, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(read_scope(args.scope), indent=2))
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
