#!/usr/bin/env python3
"""Fold exact normal TCSR clock consumers, preserving the full DMA-bound DTB."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
from build_piano_full_dtb import ROOT, execute, libcheck, sha, reference_offsets

REAL = '/soc/clock-controller@f204008'
WRONG = '/soc/syscon@1fc0000'
RPMH = '/soc/rsc@16500000/clock-controller-ml'
GCC = '/soc/clock-controller@100000'
UFS = '/soc/ufsphy_mem@1d80000'
PCIE = '/soc/phy@1c06000'
HS = '/soc/hsphy@88e3000'
SS = '/soc/ssphy@88e8000'
INPUTS = {
    'drivers/clk/qcom/tcsrcc-sm8750.c': '4c02d5110d6d1c4c7161ee70ee76a26046f17a62e481ad6c1914504794604aed',
    'arch/arm64/boot/dts/qcom/sm8750.dtsi': '531d6a6d53c8e23b94e8ba2747386f3265c1930bfa24c8db4f8fd1c0086940e0',
    'include/dt-bindings/clock/qcom,sm8750-tcsr.h': 'b797e8846389e5745381849f22062b880c15e290d11f7b8c33711b41356fdcae',
    'include/dt-bindings/clock/qcom,rpmh.h': '4a8c6b677a8367fcdfb11ec7c0f1371e30fb84e40917846aefe72272bb37c1b3',
    'include/dt-bindings/clock/qcom,sm8750-gcc.h': 'e8f4f6e4ab0d16d1e248073ce1409d935c3a1c4220dd259a8bd627cae02a06b9',
}
ALLOWED = {(REAL, 'compatible'), (REAL, 'clocks'), (WRONG, 'compatible'),
           *((path, 'clocks') for path in (UFS, PCIE, HS, SS))}


def cells(raw):
    if len(raw) % 4:
        raise ValueError('unaligned DT clock property')
    return list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))


def clocks(parsed, path):
    raw = parsed['tree'][path]['clocks']
    offsets = reference_offsets(parsed, path, 'clocks', raw)
    a = cells(raw)
    return [(parsed['phandles'][a[pos]], a[pos + 1:
              (offsets[i + 1] if i + 1 < len(offsets) else len(a))])
            for i, pos in enumerate(offsets)]


def expected(parsed, repaired):
    if parsed['tree'][REAL]['reg'] != bytes.fromhex('0f20400800003004'):
        raise ValueError('stock TCSRCC range mismatch')
    if parsed['tree'][WRONG]['reg'] != bytes.fromhex('01fc000000030000'):
        raise ValueError('wrong syscon legacy range mismatch')
    if cells(parsed['tree'][REAL]['#clock-cells']) != [1]:
        raise ValueError('real TCSR provider clock cells mismatch')
    ref = REAL if repaired else '/soc/ufs-clkref-ml'
    pcie_ref = REAL if repaired else '/pcie0-clkref-ml'
    wants = {UFS: [(RPMH, [0]), (GCC, [0x90]), (ref, [1] if repaired else [])],
             PCIE: [(GCC, [0x25]), (GCC, [0x27]), (pcie_ref, [0] if repaired else []),
                    (GCC, [0x29]), (GCC, [0x2b])],
             HS: [(REAL if repaired else WRONG, [2])],
             SS: [(GCC, [0xa2]), (REAL if repaired else WRONG, [3]),
                  (GCC, [0xa4]), (GCC, [0xa5])]}
    for path, want in wants.items():
        if clocks(parsed, path) != want:
            raise ValueError('exact PHY clock consumer mismatch: ' + path)
    compatible = b'qcom,sm8750-tcsr\0syscon\0' if repaired else b'qcom,sun-tcsrcc\0syscon\0'
    if parsed['tree'][REAL]['compatible'] != compatible:
        raise ValueError('real provider compatible drift')
    wrong = b'syscon\0' if repaired else b'qcom,sm8750-tcsr\0syscon\0'
    if parsed['tree'][WRONG]['compatible'] != wrong:
        raise ValueError('legacy syscon compatible drift')
    if repaired and clocks(parsed, REAL) != [(RPMH, [0])]:
        raise ValueError('normal TCSR XO parent mismatch')


def fold(args):
    original = args.base.read_bytes()
    if sha(original) != args.base_sha256:
        raise ValueError('base DTB SHA mismatch')
    for name, pin in INPUTS.items():
        if sha((args.kernel_tree / name).read_bytes()) != pin:
            raise ValueError('reviewed Linux clock source drift: ' + name)
    before = libcheck(original, args.libfdt)
    expected(before, False)
    # The current ROM live capture is an independent source of the address/IDs.
    rom_blob = args.rom.read_bytes()
    if sha(rom_blob) != '8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc':
        raise ValueError('current ROM capture identity mismatch')
    rom = libcheck(rom_blob, args.libfdt)
    if rom['tree'][REAL]['reg'] != before['tree'][REAL]['reg']:
        raise ValueError('current ROM and full candidate disagree on TCSRCC range')
    rom_ufs = clocks(rom, UFS)
    if rom_ufs[2] != (REAL, [1]):
        raise ValueError('current ROM UFS qref is not actual TCSRCC ID1')
    out = args.output_dir.resolve()
    if not out.is_relative_to(ROOT / 'private/analysis') or out.exists():
        raise ValueError('output must be a new private/analysis directory')
    overlay = ROOT / 'linux/dts/piano-linux-tcsr-clocks.dtso'
    out.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix='piano-tcsr-') as directory:
        tmp = Path(directory)
        execute(['cpp', '-nostdinc', '-undef', '-x', 'assembler-with-cpp', '-I', args.kernel_tree / 'include',
                 '-o', tmp / 'clocks.pp.dts', overlay], out / 'cpp.log')
        execute([args.dtc, '-@', '-I', 'dts', '-O', 'dtb', '-o', tmp / 'clocks.dtbo', tmp / 'clocks.pp.dts'], out / 'dtc.log')
        execute([args.fdtoverlay, '-i', args.base, '-o', tmp / 'out.dtb', tmp / 'clocks.dtbo'], out / 'fold.log')
        data = (tmp / 'out.dtb').read_bytes()
    after = libcheck(data, args.libfdt)
    if before['reservations'] != after['reservations'] or set(before['tree']) != set(after['tree']):
        raise ValueError('clock repair changed reservations or tree')
    changed = set()
    for path in before['tree']:
        for key in set(before['tree'][path]) | set(after['tree'][path]):
            if before['tree'][path].get(key) != after['tree'][path].get(key):
                changed.add((path, key))
    if changed != ALLOWED:
        raise ValueError('clock repair changed unexpected or missing properties: ' + str(changed ^ ALLOWED))
    expected(after, True)
    target = out / 'Piano-full-linux-managed-clocks.dtb'
    target.write_bytes(data)
    report = {'status': 'HOST_CLOCK_BINDINGS_FOLDED_NORMAL_DRIVER_READBACK_REQUIRED',
              'source_clock_inputs': INPUTS, 'rom_sha256': sha(rom_blob),
              'base_sha256': sha(original), 'overlay_sha256': sha(overlay.read_bytes()),
              'output_sha256': sha(data), 'output_bytes': len(data),
              'changes': [{'path': p, 'property': k} for p, k in sorted(changed)],
              'phy_clock_consumers': [UFS, PCIE, HS, SS],
              'hardware_verified': False, 'clock_register_written_by_host': False,
              'other_iommus_reservations_and_status_unchanged': True}
    (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--base-sha256', required=True)
    ap.add_argument('--kernel-tree', type=Path, default=ROOT / 'build/kernel-worktrees/piano-smmu-routes')
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


if __name__ == '__main__':
    main()
