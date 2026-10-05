#!/usr/bin/env python3
"""Fold real ADSP/FastRPC and PCIe IOMMU consumers; host-only and narrowly scoped."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
from build_piano_full_dtb import ROOT, execute, libcheck, sha, reference_offsets

ADSP = '/soc/remoteproc-adsp@03000000/glink-edge'
DAIS = ADSP + '/qcom,gpr/service@3/dais'
PCI = '/soc/pcie@1c00000'
REAL = '/soc/iommu@15000000'
LEGACY = '/soc/apps-smmu@15000000'
MASTERS = {DAIS: [(0x1001, 0x80), (0x1041, 0x20)],
           **{ADSP + f'/qcom,fastrpc/compute-cb@{i}':
              ([(0x1007, 0x40), (0x1067, 0), (0x1087, 0)] if i == 5 else
               [(0x1002 + i, 0x80), (0x1042 + i, 0x20)]) for i in range(1, 7)}}
INPUTS = {
    'drivers/misc/fastrpc.c': '453952957b8a6b93d2c3a8685e391004361e3a730b2213042dec216b62157c4c',
    'sound/soc/qcom/qdsp6/q6apm-dai.c': '0b33b392c02ec5a8849d502590a07f84dd87fcbfa5b44b86884dcf5e91a7abc8',
    'sound/soc/qcom/qdsp6/q6apm.c': '95cb0313537e43d0647ac3dbb2d0fd3af4b81fca5335ca86f4ca05d16bda28ab',
    'drivers/soc/qcom/apr.c': 'b7fe338b2ee7bb0db766bb937b079892b48b9c3e52d5f625e838d0429d5d41da',
    'drivers/pci/controller/dwc/pcie-qcom.c': '908f2dae60dbcf2b1bd7913e0512da9831be7f70bacb649bfeecab63a1407618',
    'drivers/of/base.c': '9889bd90c30c5fc1be5c4c5707f341e9e94fc06081b2fde349f0e76952948ade',
    'drivers/iommu/of_iommu.c': '45b8483228d75a20a8d0aa3360ef8571ac1e1edbab373cb733a6bdaa3dedf61d',
    'arch/arm64/boot/dts/qcom/sm8750.dtsi': '531d6a6d53c8e23b94e8ba2747386f3265c1930bfa24c8db4f8fd1c0086940e0',
}


def encode(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def validate(parsed, repaired):
    provider = REAL if repaired else LEGACY
    handle = int.from_bytes(parsed['tree'][provider]['phandle'], 'big')
    if parsed['tree'][provider]['#iommu-cells'] != encode(2):
        raise ValueError('expected actual two-cell apps IOMMU provider')
    for path, pairs in MASTERS.items():
        raw = b''.join(encode(handle, sid, mask) for sid, mask in pairs)
        if parsed['tree'][path]['iommus'] != raw:
            raise ValueError('DSP consumer SID/mask/provider differs: ' + path)
        reference_offsets(parsed, path, 'iommus', raw)
    mapping = encode(0, handle, 0x1400, 0, 1, 0x100, handle, 0x1401, 0, 1) if repaired else b''
    if parsed['tree'][PCI]['iommu-map'] != mapping:
        raise ValueError('exact PCI RID/SID map differs')
    # No guessed wildcard RID mask can turn a different endpoint into ours.
    if parsed['tree'][PCI].get('iommu-map-mask') not in (None, encode(0xffffffff)):
        raise ValueError('unexpected PCI RID mask')


def fold(args):
    data = args.base.read_bytes()
    if sha(data) != args.base_sha256:
        raise ValueError('base DTB identity mismatch')
    for name, pin in INPUTS.items():
        if sha((args.kernel_tree / name).read_bytes()) != pin:
            raise ValueError('reviewed DMA consumer/OF source drift: ' + name)
    before = libcheck(data, args.libfdt)
    validate(before, False)
    rom_data = args.rom.read_bytes()
    if sha(rom_data) != '8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc':
        raise ValueError('current ROM capture drift')
    rom = libcheck(rom_data, args.libfdt)
    old = int.from_bytes(rom['tree'][LEGACY]['phandle'], 'big')
    for path, pairs in MASTERS.items():
        actual = '/soc/spf_core_platform/qcom,msm-audio-ion' if path == DAIS else path
        if rom['tree'][actual]['iommus'] != b''.join(encode(old, sid, mask) for sid, mask in pairs):
            raise ValueError('ROM DSP stream pairs disagree: ' + path)
    if rom['tree'][PCI]['iommu-map'] != encode(0, old, 0x1400, 1, 0x100, old, 0x1401, 1):
        raise ValueError('ROM PCI RID/SID mapping differs')
    out = args.output_dir.resolve()
    if not out.is_relative_to(ROOT / 'private/analysis') or out.exists():
        raise ValueError('output must be a new private/analysis directory')
    overlay = ROOT / 'configs/linux/dtb/piano-linux-dsp-pcie-dma.dtso'
    out.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix='piano-dsp-pci-') as temporary:
        tmp = Path(temporary)
        execute(['cpp', '-nostdinc', '-undef', '-x', 'assembler-with-cpp', '-o', tmp / 'in.dts', overlay], out / 'cpp.log')
        execute([args.dtc, '-@', '-I', 'dts', '-O', 'dtb', '-o', tmp / 'in.dtbo', tmp / 'in.dts'], out / 'dtc.log')
        execute([args.fdtoverlay, '-i', args.base, '-o', tmp / 'out.dtb', tmp / 'in.dtbo'], out / 'fold.log')
        result = (tmp / 'out.dtb').read_bytes()
    after = libcheck(result, args.libfdt)
    if set(before['tree']) != set(after['tree']) or before['reservations'] != after['reservations'] or before['phandles'] != after['phandles']:
        raise ValueError('DSP/PCI repair changed tree/reservations/phandle identities')
    changed = {(p, k) for p in before['tree'] for k in set(before['tree'][p]) | set(after['tree'][p])
               if before['tree'][p].get(k) != after['tree'][p].get(k)}
    expected = {(p, 'iommus') for p in MASTERS} | {(PCI, 'iommu-map')}
    if changed != expected:
        raise ValueError('unexpected or missing property delta: ' + str(changed ^ expected))
    validate(after, True)
    target = out / 'Piano-full-linux-managed-dsp-pcie.dtb'
    target.write_bytes(result)
    report = {'status': 'HOST_DSP_PCIE_BINDINGS_FOLDED_KERNEL_READBACK_REQUIRED',
              'base_sha256': sha(data), 'rom_sha256': sha(rom_data), 'source_dma_inputs': INPUTS,
              'overlay_sha256': sha(overlay.read_bytes()), 'output_sha256': sha(result),
              'output_bytes': len(result), 'changes': [{'path': p, 'property': k} for p, k in sorted(changed)],
              'dsp_consumers': {p: pairs for p, pairs in MASTERS.items()},
              'pci_rid_sid': [[0, 0x1400], [0x100, 0x1401]],
              'domain_forced': False, 'hardware_dma_verified': False,
              'pci_parf_hardware_table_verified': False, 'userspace_mmio': False}
    (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--base-sha256', required=True)
    ap.add_argument('--kernel-tree', type=Path, default=ROOT / 'build/kernel-worktrees/piano-smmu-context')
    ap.add_argument('--rom', type=Path, default=ROOT / 'private/analysis/android-memory-2026-10-05/live.dtb')
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--dtc', type=Path, default=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/dtc')
    ap.add_argument('--fdtoverlay', type=Path, default=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/fdtoverlay')
    ap.add_argument('--libfdt', type=Path, default=ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1')
    args = ap.parse_args()
    try:
        print(json.dumps(fold(args), indent=2))
    except (OSError, ValueError) as error:
        ap.exit(1, str(error) + '\n')
