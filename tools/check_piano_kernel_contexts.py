#!/usr/bin/env python3
"""Validate actual kernel-managed identity and stage1 DMA context evidence.

No modules, maps, block access or ready marker. The kernel compares stage1
registers to its own live domain and serialized group/context state before
exposing this read-only ABI. This is configuration evidence, not a DMA test.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import re

SCOPES = {
    'mdss-prebind': ('ae00000.display-subsystem', '/soc/display-subsystem@ae00000', 'identity', [(0x800, 2)]),
    'mdss': ('ae00000.display-subsystem', '/soc/display-subsystem@ae00000', 'stage1', [(0x800, 2)]),
    'gpu': ('3d00000.gpu', '/soc/gpu@3d00000', 'stage1', [(0, 0), (1, 0)]),
    'gmu': ('3d6c000.gmu', '/soc/gmu@3d6c000', 'stage1', [(5, 0)]),
    'video': ('aa00000.video-codec-ml', '/soc/video-codec-ml@aa00000', 'stage1', [(0x1940, 0), (0x1947, 0)]),
    'camera': ('ad27000.isp-ml', '/soc/isp-ml@ad27000', 'stage1', [(0x1c00, 0)]),
    'audio': (None, '/soc/remoteproc-adsp@03000000/glink-edge/qcom,gpr/service@3/dais', 'managed', [(0x1001, 0x80), (0x1041, 0x20)]),
    **{f'fastrpc-{i}': (None, '/soc/remoteproc-adsp@03000000/glink-edge/qcom,fastrpc/compute-cb@' + str(i),
       'managed', [(0x1007, 0x40), (0x1067, 0), (0x1087, 0)] if i == 5 else
       [(0x1002 + i, 0x80), (0x1042 + i, 0x20)]) for i in range(1, 7)},
    'radio': ('0000:01:00.0', '/soc/pcie@1c00000/pcie0_rp/wifi@0', 'managed', [(0x1401, 0)]),
}
BASE_FIELDS = {'sid', 'mask', 'slot', 'origin', 'smr', 's2cr', 'expected', 'cb', 'sctlr', 'cbar', 'fsr'}
CONTEXT_FIELDS = {'ttbr0', 'ttbr1', 'tcr', 'tcr2', 'mair0', 'mair1'}
MAX_SYSFS_TEXT_BYTES = 8192


def read_sysfs_text(path):
    """Read a bounded kernel snapshot without Python's FileIO EAGAIN-to-None.

    A sysfs show callback can return EAGAIN even on a blocking descriptor.
    FileIO turns that errno into None, which text decoding can turn into a
    TypeError. Direct read(2) preserves the actual OSError and its errno.
    Do not return partial evidence if a later read fails.
    """
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        chunks, size = [], 0
        while True:
            chunk = os.read(descriptor, MAX_SYSFS_TEXT_BYTES + 1 - size)
            if not chunk:
                return b''.join(chunks).decode('ascii')
            chunks.append(chunk)
            size += len(chunk)
            if size > MAX_SYSFS_TEXT_BYTES:
                raise ValueError('sysfs context evidence exceeds its bounded ABI')
    finally:
        os.close(descriptor)


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


def platform_consumer(sysfs, expected, driver):
    """Names of GPR/RPMsg-created platform children depend on real parent buses."""
    found = []
    for number, directory in enumerate((sysfs / 'bus/platform/devices').iterdir()):
        if number >= 4096:
            raise ValueError('platform inventory exceeds its bounded scope')
        try:
            matches = (directory / 'of_node').resolve(strict=True) == expected.resolve(strict=True)
        except FileNotFoundError:
            continue
        if matches:
            found.append(directory)
    if not found:
        raise FileNotFoundError(errno.ENOENT, 'normal DMA consumer has not been created', str(expected))
    if len(found) != 1:
        raise ValueError('DMA consumer must have exactly one actual platform device')
    directory = found[0]
    if (directory / 'driver').resolve(strict=True).name != driver:
        raise ValueError('actual DMA consumer driver is not bound')
    return directory


def pci_consumer(sysfs, device, expected, handle):
    directory = sysfs / 'bus/pci/devices' / device
    if (directory / 'of_node').resolve(strict=True) != expected.resolve(strict=True):
        raise ValueError('PCI endpoint does not match the actual wifi DT node')
    if read_sysfs_text(directory / 'vendor').strip() != '0x17cb' or read_sysfs_text(directory / 'device').strip() != '0x110e':
        raise ValueError('PCI endpoint is not the WCN7861 consumer')
    host = sysfs / 'firmware/devicetree/base/soc/pcie@1c00000'
    wanted = b''.join(value.to_bytes(4, 'big') for value in
                     (0, int.from_bytes(handle, 'big'), 0x1400, 0, 1,
                      0x100, int.from_bytes(handle, 'big'), 0x1401, 0, 1))
    if (host / 'iommu-map').read_bytes() != wanted:
        raise ValueError('normal PCI RID/SID map differs')
    if (host / 'iommu-map-mask').exists() and (host / 'iommu-map-mask').read_bytes() != b'\xff' * 4:
        raise ValueError('PCI RID mask differs')
    if (host / 'linux,pci-domain').read_bytes() != bytes(4):
        raise ValueError('PCI domain does not identify the selected endpoint')
    if (expected / 'reg').read_bytes() != bytes.fromhex('0001000000000000000000000000000000000000'):
        raise ValueError('PCI endpoint DT devfn differs')
    return directory


def read_scope(scope, sysfs=Path('/sys')):
    device, node, mode, pairs = SCOPES[scope]
    expected = sysfs / ('firmware/devicetree/base' + node)
    provider_node = '/soc/iommu@3da0000' if scope in ('gpu', 'gmu') else '/soc/iommu@15000000'
    provider = sysfs / ('firmware/devicetree/base' + provider_node)
    handle = (provider / 'phandle').read_bytes()
    if len(handle) != 4 or handle == bytes(4) or (provider / '#iommu-cells').read_bytes() != b'\0\0\0\2':
        raise ValueError('actual IOMMU provider shape differs')
    if scope == 'radio':
        directory = pci_consumer(sysfs, device, expected, handle)
    else:
        if device is None:
            directory = platform_consumer(sysfs, expected, 'q6apm-dai' if scope == 'audio' else 'qcom,fastrpc-cb')
        else:
            directory = sysfs / 'bus/platform/devices' / device
            if (directory / 'of_node').resolve(strict=True) != expected.resolve(strict=True):
                raise ValueError('kernel device does not match the exact DT consumer')
        wanted = b''.join(handle + sid.to_bytes(4, 'big') + mask.to_bytes(4, 'big') for sid, mask in pairs)
        if (expected / 'iommus').read_bytes() != wanted:
            raise ValueError('consumer does not reference the expected real IOMMU provider')
    if mode == 'managed':
        # Normal consumer attachment chooses the domain. Never force identity.
        # EAGAIN can also mean no domain or group contention. Only try the
        # independent identity getter; it must prove the actual identity domain.
        try:
            first = read_sysfs_text(directory / 'piano_dma_context')
            mode = 'stage1'
        except OSError as error:
            if error.errno != errno.EAGAIN:
                raise
            mode = 'identity'
            first = read_sysfs_text(directory / 'piano_dma_route')
    else:
        first = read_sysfs_text(directory / ('piano_dma_context' if mode == 'stage1' else 'piano_dma_route'))
    attr = 'piano_dma_context' if mode == 'stage1' else 'piano_dma_route'
    second = read_sysfs_text(directory / attr)
    if first != second:
        raise ValueError('context changed across fresh kernel reads')
    return {'scope': scope, 'consumer': directory.name, 'domain': mode, 'routes': verify(first, mode, pairs),
            'pci_parf_hardware_table_verified': False,
            'marker_written': False, 'memory_ownership_authorized': False, 'dma_transfer_verified': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scope', choices=SCOPES, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(read_scope(args.scope), indent=2))
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
