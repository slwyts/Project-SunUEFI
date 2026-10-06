#!/usr/bin/env python3
"""Fold real DMA consumers and the standard USB PHY chain; host-only."""
import argparse
import copy
import json
from pathlib import Path
import struct
import tempfile
from build_piano_full_dtb import ROOT, execute, libcheck, sha, reference_offsets
from compose_piano_dtb import write_fdt

ADSP = '/soc/remoteproc-adsp@03000000/glink-edge'
DAIS = ADSP + '/qcom,gpr/service@3/dais'
PCI = '/soc/pcie@1c00000'
REAL = '/soc/iommu@15000000'
LEGACY = '/soc/apps-smmu@15000000'
RAMOOPS = '/reserved-memory/ramoops-region'
SRAM = '/soc/mmio-sram@0x17b4e000'
BOOT_FIX_FIELDS = {(RAMOOPS, 'record-size'), (RAMOOPS, 'pmsg-size'), (SRAM, 'reg')}
USB = '/soc/ssusb@a600000'
HS = '/soc/hsphy@88e3000'
SS = '/soc/ssphy@88e8000'
REPEATER = '/soc/qcom,spmi@c42d000/qcom,pmih010x@7/eusb2-repeater@fd00'
USB_FIX_FIELDS = {(USB, 'phys'), (USB, 'phy-names'), (HS, 'phys'),
                  (REPEATER, 'compatible'), (REPEATER, '#phy-cells')}
USB_SUPPLIES = {(REPEATER, 'vdd3-supply'): 'ldob5', (REPEATER, 'vdd18-supply'): 'ldob15',
                (HS, 'vdd-supply'): 'ldod2', (HS, 'vdda12-supply'): 'ldog3',
                (SS, 'vdda-phy-supply'): 'ldod2', (SS, 'vdda-pll-supply'): 'ldog3'}
USB_SUPPLY_FIX_FIELDS = set(USB_SUPPLIES)
# These resources and limits agree between the same-board ROM, Android
# regulator consumers, command DB and the standard Piano PMIC regulator data.
USB_RAILS = {
    'ldob5': ('/soc/rsc@16500000/regulators-4/ldo5', 'b', 'qcom,pm8550-rpmh-regulators', 3100000, 3148000),
    'ldob15': ('/soc/rsc@16500000/regulators-4/ldo15', 'b', 'qcom,pm8550-rpmh-regulators', 1800000, 1800000),
    'ldod2': ('/soc/rsc@16500000/regulators-0/ldo2', 'd', 'qcom,pm8550ve-rpmh-regulators', 880000, 912000),
    'ldog3': ('/soc/rsc@16500000/regulators-6/ldo3', 'g', 'qcom,pm8550ve-rpmh-regulators', 1200000, 1256000),
}
USB_RAIL_NODES = {row[0] for row in USB_RAILS.values()}
USB_SUPPLY_EVIDENCE = ROOT / 'private/analysis/piano-usb-supply-runtime/evidence.json'
USB_SUPPLY_EVIDENCE_SHA = 'ade69b5782b07fa4a67710effd49fc728ad686285bb756ed6fd6719825295379'
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
    'arch/arm64/boot/dts/qcom/pmih0108.dtsi': 'f09272308fe1e0419ce86a30db74d4047efd7149a4b3ae4f100840d9ef1a17ca',
    'drivers/phy/qualcomm/phy-qcom-m31-eusb2.c': '4e75319b6b3862778be33234b6385d19c684e10bcf88ab74b894627ca0839142',
    'drivers/phy/qualcomm/phy-qcom-eusb2-repeater.c': '74fb34dcc0ad2377d12969e78a20c7e307f24ed46acd1ad4c150397ac0964c6d',
    'drivers/phy/qualcomm/phy-qcom-qmp-combo.c': '76c68d618de56c49d3d4e73af65e225eb19c1a2c4b51687a927480f9f18f0e33',
    'include/dt-bindings/phy/phy-qcom-qmp.h': '98993f931712dae608075c60149f7fec21405dda335bae06dcf3f448598bb995',
    'drivers/regulator/qcom-rpmh-regulator.c': 'bd7f9cd8dcdb065ae38bc13c0ef6acc887a5bd796758c99c915819ec3fc2e596',
    'drivers/soc/qcom/cmd-db.c': 'e6082231f11d6e85f663840ea40d4fdba36d41b76683fcdabadd002681f92f2f',
}


def encode(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def repair_boot_geometry(parsed):
    """Two observed test107 binding fixes, preserving both physical regions."""
    tree = parsed['tree']
    soc, reserved = tree['/soc'], tree['/reserved-memory']
    ram, sram = tree[RAMOOPS], tree[SRAM]
    if (soc.get('#address-cells'), soc.get('#size-cells')) != (encode(1), encode(1)):
        raise ValueError('unexpected SRAM parent geometry')
    if (reserved.get('#address-cells'), reserved.get('#size-cells')) != (encode(2), encode(2)):
        raise ValueError('unexpected ramoops parent geometry')
    if (ram.get('compatible') != b'ramoops\0' or ram.get('reg') != encode(0, 0xa3500000, 0, 0x400000)
            or ram.get('console-size') != encode(0x200000) or ram.get('record-size') != encode(0x40000)
            or ram.get('pmsg-size') != encode(0) or ram.get('ftrace-size') not in (None, encode(0))
            or ram.get('ecc-size') not in (None, encode(0))):
        raise ValueError('unexpected ramoops region/layout')
    if (sram.get('compatible') != b'mmio-sram\0' or sram.get('reg') != encode(0, 0x17b4e000, 0, 0x400)
            or sram.get('#address-cells') != encode(2) or sram.get('#size-cells') != encode(2)):
        raise ValueError('unexpected SRAM region/layout')
    fixed = copy.deepcopy(parsed)
    fixed['tree'][RAMOOPS].pop('record-size')
    fixed['tree'][RAMOOPS]['pmsg-size'] = encode(0x200000)
    fixed['tree'][SRAM]['reg'] = encode(0x17b4e000, 0x400)
    return fixed


def validate_boot_fix_delta(before, after):
    """Permit exactly these fields only when their entire nodes match the repair."""
    expected = repair_boot_geometry(before)
    for path in (RAMOOPS, SRAM):
        if after['tree'][path] != expected['tree'][path]:
            raise ValueError('boot geometry repair differs: ' + path)


def repair_usb_phy_chain(parsed):
    """Replace the observed NULL PHY path with normal supplier dependencies.

    The PMIH0108 fd00 block compatible is from the pinned SoC PMIC DTSI.
    Downstream tuning is preserved, not claimed converted. QMP argument 0 is
    QMP_USB43DP_USB3_PHY in the pinned sm8750.dtsi/phy-qcom-qmp.h binding.
    This reference does not invent its missing lane/orientation/role graph.
    """
    tree = parsed['tree']
    usb, hs, repeater = (tree[path] for path in (USB, HS, REPEATER))
    ss = tree[SS]
    nop = tree['/soc/usb_nop_phy']
    if (usb.get('compatible') != b'qcom,sm8750-dwc3\0qcom,snps-dwc3\0'
            or usb.get('phys') != nop.get('phandle')
            or usb.get('phy-names') != b'usb2-phy\0'
            or usb.get('maximum-speed') is not None
            or usb.get('dr_mode') != b'peripheral\0'
            or nop.get('compatible') != b'usb-nop-xceiv\0'):
        raise ValueError('unexpected USB NULL-PHY baseline')
    if (hs.get('compatible') != b'qcom,sm8750-m31-eusb2-phy\0'
            or hs.get('#phy-cells') != encode(0) or hs.get('phys') is not None
            or hs.get('usb-repeater') != repeater.get('phandle')
            or hs.get('reg', b'')[:8] != encode(0x88e3000, 0x29c)):
        raise ValueError('unexpected M31 PHY/repeater identity')
    if (repeater.get('compatible') != b'qcom,pmic-eusb2-repeater\0'
            or repeater.get('reg') != encode(0xfd00)
            or repeater.get('#phy-cells') is not None
            or tree[REPEATER.rsplit('/', 1)[0]].get('reg') != encode(7, 0)):
        raise ValueError('unexpected PMIH0108 fd00 repeater identity')
    if (ss.get('compatible') != b'qcom,sm8750-qmp-usb3-dp-phy\0'
            or ss.get('reg') != encode(0x88e8000, 0x4000)
            or ss.get('#phy-cells') != encode(1)):
        raise ValueError('unexpected QMP USB3 PHY identity')
    fixed = copy.deepcopy(parsed)
    fixed['tree'][USB]['phys'] = hs['phandle'] + ss['phandle'] + encode(0)
    fixed['tree'][USB]['phy-names'] = b'usb2-phy\0usb3-phy\0'
    fixed['tree'][HS]['phys'] = repeater['phandle']
    fixed['tree'][REPEATER]['compatible'] = b'qcom,pm8550b-eusb2-repeater\0'
    fixed['tree'][REPEATER]['#phy-cells'] = encode(0)
    return fixed


def validate_usb_fix_delta(before, after):
    expected = repair_usb_phy_chain(before)
    if set(after['tree']) - set(before['tree']):
        expected = repair_usb_supplies(expected)
    for path in (USB, HS, SS, REPEATER):
        if after['tree'][path] != expected['tree'][path]:
            raise ValueError('USB PHY chain repair differs: ' + path)
    for path in (USB, HS):
        reference_offsets(after, path, 'phys', after['tree'][path]['phys'])


def repair_usb_supplies(parsed):
    """Use observed RPMh outputs, without guessing input rails or vote modes.

    The normal qcom-rpmh driver still resolves each resource in the runtime
    command DB and fails registration if it is absent. Observed addresses
    never become DT MMIO resources or a replacement for that lookup.
    """
    raw = USB_SUPPLY_EVIDENCE.read_bytes()
    if sha(raw) != USB_SUPPLY_EVIDENCE_SHA:
        raise ValueError('read-only Android USB supply evidence drift')
    evidence = json.loads(raw)['resources']
    fixed = copy.deepcopy(parsed)
    next_handle = max(parsed['phandles']) + 1
    for resource, (path, pmic_id, compatible, minimum, maximum) in USB_RAILS.items():
        parent = parsed['tree'][path.rsplit('/', 1)[0]]
        if (path in parsed['tree'] or parent.get('qcom,pmic-id') != (pmic_id + '\0').encode()
                or parent.get('compatible') != (compatible + '\0').encode()
                or evidence[resource]['constraints_microvolt'] != [minimum, maximum]
                or evidence[resource]['type'] != 'pmic5-ldo'
                or not evidence[resource]['enabled']):
            raise ValueError('USB regulator resource/provider identity drift: ' + resource)
        legacy = '/soc/rsc@16500000/drv@2/rpmh-regulator-' + resource
        if (parsed['tree'][legacy].get('qcom,resource-name') != (resource + '\0').encode()
                or parsed['tree'][legacy].get('qcom,regulator-type') != b'pmic5-ldo\0'):
            raise ValueError('USB legacy regulator resource/type drift: ' + resource)
        children = [p for p in parsed['tree'] if p.rsplit('/', 1)[0] == legacy
                    and parsed['tree'][p].get('regulator-name') ==
                    (evidence[resource]['regulator_name'] + '\0').encode()]
        if len(children) != 1:
            raise ValueError('USB legacy regulator child is ambiguous: ' + resource)
        old = parsed['tree'][children[0]]
        if (old.get('regulator-min-microvolt') != encode(minimum)
                or old.get('regulator-max-microvolt') != encode(maximum)):
            raise ValueError('USB regulator voltage constraint drift: ' + resource)
        fixed['tree'][path] = {'regulator-name': ('piano_usb_' + resource + '\0').encode(),
                              'regulator-min-microvolt': encode(minimum),
                              'regulator-max-microvolt': encode(maximum),
                              'phandle': encode(next_handle)}
        fixed['phandles'][next_handle] = path
        next_handle += 1
    for (consumer, key), resource in USB_SUPPLIES.items():
        fixed['tree'][consumer][key] = fixed['tree'][USB_RAILS[resource][0]]['phandle']
    return fixed


def validate_usb_supply_delta(before, after):
    expected = repair_usb_supplies(before)
    if (set(after['tree']) != set(expected['tree'])
            or after['phandles'] != expected['phandles']
            or after['reservations'] != before['reservations']):
        raise ValueError('USB supply repair changed unrelated node/reservation/phandle identity')
    for path in USB_RAIL_NODES:
        if after['tree'][path] != expected['tree'][path]:
            raise ValueError('USB supply regulator differs: ' + path)
    for path, key in USB_SUPPLY_FIX_FIELDS:
        if after['tree'][path][key] != expected['tree'][path][key]:
            raise ValueError('USB supply reference differs: ' + path + ':' + key)
        reference_offsets(after, path, key, after['tree'][path][key])


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
    result = write_fdt(repair_usb_supplies(repair_usb_phy_chain(repair_boot_geometry(after))))
    after = libcheck(result, args.libfdt)
    validate_usb_supply_delta(before, after)
    changed = {(p, k) for p in before['tree'] for k in set(before['tree'][p]) | set(after['tree'][p])
               if before['tree'][p].get(k) != after['tree'][p].get(k)}
    expected = ({(p, 'iommus') for p in MASTERS} | {(PCI, 'iommu-map')}
                | BOOT_FIX_FIELDS | USB_FIX_FIELDS | USB_SUPPLY_FIX_FIELDS)
    if changed != expected:
        raise ValueError('unexpected or missing property delta: ' + str(changed ^ expected))
    validate(after, True)
    validate_boot_fix_delta(before, after)
    validate_usb_fix_delta(before, after)
    target = out / 'Piano-full-linux-managed-dsp-pcie.dtb'
    target.write_bytes(result)
    report = {'status': 'HOST_DSP_PCIE_BINDINGS_FOLDED_KERNEL_READBACK_REQUIRED',
              'base_sha256': sha(data), 'rom_sha256': sha(rom_data), 'source_dma_inputs': INPUTS,
              'overlay_sha256': sha(overlay.read_bytes()), 'output_sha256': sha(result),
              'output_bytes': len(result), 'changes': [{'path': p, 'property': k} for p, k in sorted(changed)],
              'new_nodes': sorted(USB_RAIL_NODES),
              'dsp_consumers': {p: pairs for p, pairs in MASTERS.items()},
              'pci_rid_sid': [[0, 0x1400], [0x100, 0x1401]],
              'usb_phy_consumers': {USB: [HS, SS], HS: [REPEATER]},
              'usb3_phy_argument': 0,
              'usb_supplies_converted': True, 'usb_vendor_tuning_converted': False,
              'usb_supply_evidence_sha256': USB_SUPPLY_EVIDENCE_SHA,
              'usb_supply_resources': {resource: row[0] for resource, row in USB_RAILS.items()},
              'usb_supply_input_topology_verified': False,
              'usb_repeater_mapping_source': 'arch/arm64/boot/dts/qcom/pmih0108.dtsi',
              'usb_hardware_verified': False,
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
