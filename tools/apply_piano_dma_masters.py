#!/usr/bin/env python3
"""Apply only six proven Linux DMA-master iommus bindings, host-only."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
from build_piano_full_dtb import ROOT, libcheck, sha, execute, reference_offsets

MASTERS = {
    '/soc/ssusb@a600000': 0x40,
    '/soc/ufshc@1d84000': 0x60,
    '/soc/qcom,gpi-dma@a00000': 0xb6,
    '/soc/qcom,qupv3_1_geni_se@ac0000': 0xa3,
    '/soc/qcom,gpi-dma@800000': 0x436,
    '/soc/qcom,qupv3_2_geni_se@8c0000': 0x423,
}


def apply(args):
    original = args.base.read_bytes()
    if sha(original) != args.base_sha256:
        raise ValueError('base DTB hash mismatch')
    before = libcheck(original, args.libfdt)
    source = ROOT / 'configs/linux/dtb/piano-linux-owned-dma.dtso'
    out = args.output_dir
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='piano-dma-bindings-') as directory:
        stage = Path(directory)
        execute(['cpp', '-nostdinc', '-undef', '-x', 'assembler-with-cpp', '-I', args.kernel_tree / 'include',
                 '-o', stage / 'masters.pp.dts', source], out / 'cpp.log')
        execute([args.dtc, '-@', '-I', 'dts', '-O', 'dtb', '-o', stage / 'masters.dtbo', stage / 'masters.pp.dts'], out / 'dtc.log')
        execute([args.fdtoverlay, '-i', args.base, '-o', stage / 'result.dtb', stage / 'masters.dtbo'], out / 'fold.log')
        data = (stage / 'result.dtb').read_bytes()
        after = libcheck(data, args.libfdt)
        if before['reservations'] != after['reservations'] or set(before['tree']) != set(after['tree']):
            raise ValueError('DMA binding patch changed tree or memory reservations')
        changes = []
        for path in before['tree']:
            for key in set(before['tree'][path]) | set(after['tree'][path]):
                a, b = before['tree'][path].get(key), after['tree'][path].get(key)
                if a != b:
                    if path not in MASTERS or key != 'iommus':
                        raise ValueError('unreviewed property change: ' + path + ':' + key)
                    changes.append({'path': path, 'property': key})
        records = []
        for path, sid in MASTERS.items():
            raw = after['tree'][path]['iommus']
            if len(raw) != 12 or int.from_bytes(raw[4:8], 'big') != sid or raw[8:] != bytes(4):
                raise ValueError('SID/mask mismatch')
            reference_offsets(after, path, 'iommus', raw)
            provider = after['phandles'][int.from_bytes(raw[:4], 'big')]
            if provider != '/soc/iommu@15000000':
                raise ValueError('DMA master still points to legacy/unmanaged SMMU')
            records.append({'path': path, 'sid': sid, 'mask': 0, 'provider': provider})
        dest = out / 'Piano-full-linux-owned-dma.dtb'
        if dest.exists() and dest.read_bytes() != data:
            raise ValueError('refuse replacement of different candidate')
        dest.write_bytes(data)
        manifest = {'status': 'HOST_FOLDED_KERNEL_ROUTE_READBACK_REQUIRED', 'hardware_verified': False,
                    'memory_ownership_authorized': False, 'base_sha256': sha(original),
                    'overlay_sha256': sha(source.read_bytes()), 'output_sha256': sha(data),
                    'output_bytes': len(data), 'changes': changes, 'masters': records,
                    'required_kernel_abi': 'piano-dma-route-v1',
                    'ownership': 'Kernel IOMMU attach/domain APIs; no ancestor route copied or userspace MMIO.'}
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--base-sha256', required=True)
    ap.add_argument('--kernel-tree', type=Path, required=True)
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--dtc', type=Path, default=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/dtc')
    ap.add_argument('--fdtoverlay', type=Path, default=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/fdtoverlay')
    ap.add_argument('--libfdt', type=Path, default=ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1')
    args = ap.parse_args()
    try:
        print(json.dumps(apply(args)))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        ap.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
