#!/usr/bin/env python3
"""Remove the unused downstream keyboard panel dependency; host-only."""
import argparse
import copy
import json
from pathlib import Path
import struct

from build_piano_full_dtb import ROOT, libcheck, reference_offsets, sha
from compose_piano_dtb import write_fdt

KEYBOARD = '/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000/nanosic@4c'
PMIC = '/soc/rsc@16500000/regulators-4'
LEGACY_PANELS = tuple('/soc/qcom,mdss_mdp@ae00000/' + name for name in (
    'qcom,mdss_dsi_p81_42_02_0a_dualdsi_dsc_vid',
    'qcom,mdss_dsi_p81_35_02_0b_dualdsi_dsc_vid',
    'qcom,mdss_dsi_nt37801_wqhd_plus_vid'))
SUPPLIES = {'vddio-supply': (PMIC + '/ldo11', 1800000, 'ldob11'),
            'dvdd-supply': (PMIC + '/ldo14', 3296000, 'ldob14')}
FIX_FIELDS = {(KEYBOARD, 'panel')}
STATUS = 'HOST_KEYBOARD_SUPPLIERS_FOLDED_KERNEL_READBACK_REQUIRED'
FILENAME = 'Piano-full-linux-keyboard-suppliers.dtb'
INPUTS = {
    'drivers/hid/hid-nanosic-wn8030.c': '632324fe94f52e54de80d735840349223309451fa6e6398dde4386d01bb2c33c',
    'drivers/of/property.c': '60df87ac381220fc6b091bf470cb5e9d824dd87d958c5d3c47a08b92dfe929d3',
    'drivers/regulator/qcom-rpmh-regulator.c': 'bd7f9cd8dcdb065ae38bc13c0ef6acc887a5bd796758c99c915819ec3fc2e596',
}


def cell(value):
    return struct.pack('>I', value)


def repair(parsed):
    """The pinned HID driver consumes GPIOs/vddio/dvdd, never a DRM panel.

    OF's generic panel parser creates fw_devlink suppliers even when the
    consumer driver does not read the property. Test111 was deferred on the
    retained downstream NT37801 node. Preserve the already normalized RPMh
    rails, voltage constraints, IRQs, GPIOs and every other property.
    """
    tree = parsed['tree']
    node, pmic = tree[KEYBOARD], tree[PMIC]
    if node.get('compatible') != b'nanosic,wn8030-piano\0' or node.get('reg') != cell(0x4c):
        raise ValueError('keyboard consumer identity differs')
    raw = node.get('panel', b'')
    if len(raw) != len(LEGACY_PANELS) * 4 or tuple(
            parsed['phandles'].get(handle) for handle in struct.unpack('>3I', raw)) != LEGACY_PANELS:
        raise ValueError('keyboard legacy panel suppliers differ')
    if pmic.get('compatible') != b'qcom,pm8550-rpmh-regulators\0' or pmic.get('qcom,pmic-id') != b'b\0':
        raise ValueError('keyboard RPMh provider identity differs')
    for key, (path, voltage, resource) in SUPPLIES.items():
        rail = tree[path]
        if (node.get(key) != rail.get('phandle') or not rail.get('phandle')
                or rail.get('regulator-min-microvolt') != cell(voltage)
                or rail.get('regulator-max-microvolt') != cell(voltage)):
            raise ValueError('keyboard supply/provider constraint differs: ' + resource)
        reference_offsets(parsed, KEYBOARD, key, node[key])
    fixed = copy.deepcopy(parsed)
    del fixed['tree'][KEYBOARD]['panel']
    return fixed


def validate_delta(before, after):
    expected = repair(before)
    if (after['tree'] != expected['tree'] or after['phandles'] != before['phandles']
            or after['reservations'] != before['reservations'] or after['boot_cpu'] != before['boot_cpu']):
        raise ValueError('keyboard repair changed unrelated DTB data')


def fold(args):
    data = args.base.read_bytes()
    if sha(data) != args.base_sha256:
        raise ValueError('base DTB identity mismatch')
    for name, pin in INPUTS.items():
        if sha((args.kernel_tree / name).read_bytes()) != pin:
            raise ValueError('reviewed keyboard/OF/RPMh source drift: ' + name)
    before = libcheck(data, args.libfdt)
    result = write_fdt(repair(before))
    after = libcheck(result, args.libfdt)
    validate_delta(before, after)
    out = args.output_dir.resolve()
    if not out.is_relative_to(ROOT / 'private/analysis') or out.exists():
        raise ValueError('output must be a new private/analysis directory')
    out.mkdir(parents=True)
    (out / FILENAME).write_bytes(result)
    report = {'status': STATUS, 'base_sha256': sha(data),
              'source_keyboard_inputs': INPUTS, 'output_sha256': sha(result),
              'output_bytes': len(result),
              'changes': [{'path': KEYBOARD, 'property': 'panel'}],
              'new_nodes': [], 'removed_unused_panel_suppliers': list(LEGACY_PANELS),
              'keyboard_supply_resources': {key: {'provider': path, 'resource': resource,
                                                  'microvolt': voltage}
                                            for key, (path, voltage, resource) in SUPPLIES.items()},
              'memory_ownership_authorized': False, 'keyboard_hardware_verified': False,
              'regulator_hardware_verified': False, 'userspace_mmio': False}
    (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--base-sha256', required=True)
    ap.add_argument('--kernel-tree', type=Path, default=ROOT / 'build/kernel-worktrees/piano-smmu-context')
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--libfdt', type=Path, default=ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1')
    args = ap.parse_args()
    try:
        print(json.dumps(fold(args), indent=2))
    except (OSError, ValueError) as error:
        ap.exit(1, str(error) + '\n')
