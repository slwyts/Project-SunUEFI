#!/usr/bin/env python3
"""Add board providers to the captured USB module set, using host files only."""
import hashlib
import json
from pathlib import Path
import shutil
import struct

PROVIDERS = ('qcom_hwspinlock', 'qcom_aoss', 'msm_qmp', 'qcom_cpucp',
             'qcom_scmi_vendor', 'qcom_scmi_client', 'clk_scmi',
             'qcom_pdc', 'clk_rpmh', 'tcsrcc_sun',
             'icc_bcm_voter', 'icc_rpmh', 'qnoc_sun', 'arm_smmu',
             'pinctrl_spmi_gpio', 'i2c_msm_geni', 'phy_msm_m31_eusb2',
             'qti_fixed_regulator', 'qcom_i2c_pmic')
FORBIDDEN = ('ufs', 'mmc', 'blackbox', 'mtdoops', 'charger_partition',
             'bootmonitor', 'dload', 'dmesg_dumper')


def modinfo(path):
    b = path.read_bytes()
    if b[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', b, 18)[0] != 183:
        raise ValueError(f'Not an ARM64 little-endian module: {path.name}')
    offset = struct.unpack_from('<Q', b, 40)[0]
    step, count, strings_index = struct.unpack_from('<HHH', b, 58)
    if step != 64 or offset + step * count > len(b) or strings_index >= count:
        raise ValueError('Invalid ELF section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', b, offset + i * step) for i in range(count)]
    strings = sections[strings_index]
    names = b[strings[4]:strings[4] + strings[5]]
    for section in sections:
        if names[section[0]:].split(b'\0', 1)[0] == b'.modinfo':
            if section[4] + section[5] > len(b):
                raise ValueError('Invalid module info bounds')
            pairs = [x.decode().split('=', 1) for x in b[section[4]:section[4] + section[5]].split(b'\0') if b'=' in x]
            info = dict(pairs)
            return info
    raise ValueError('No module metadata')


def main():
    root = Path(__file__).resolve().parent.parent
    directory = root / 'private/analysis/linux-module-set'
    old = json.loads((directory / 'manifest.json').read_text())
    reference = next(iter(old['modules'].values()))
    reference_vermagic = modinfo(directory / reference['file'])['vermagic']
    for entry in old['modules'].values():
        if hashlib.sha256((directory / entry['file']).read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Previously selected module hash mismatch')
    index = {}
    # Prefer previously selected, hash-checked modules over other copies.
    for source in ('vendor-modules', 'usb-modules', 'linux-module-set'):
        for path in sorted((root / 'private/analysis' / source).glob('*.ko')):
            info = modinfo(path)
            name = info.get('name', path.stem).replace('-', '_')
            index[name] = (path, info)
    seeds = list(PROVIDERS) + list(old['modules'])
    selected, active = {}, set()

    def visit(name):
        name = name.replace('-', '_')
        if name in selected:
            return
        if name in active:
            raise ValueError('Module dependency cycle')
        if any(word in name for word in FORBIDDEN):
            raise ValueError('Persistent storage/logging dependency refused: ' + name)
        if name not in index:
            raise ValueError('Missing captured dependency: ' + name)
        path, info = index[name]
        # Stock GKI 6.6.118 accepts this firmware's 6.6.57 vendor KMI modules.
        # Keep the captured vendor baseline; normal insmod still checks CRCs.
        if info.get('vermagic', '') != reference_vermagic:
            raise ValueError('Module kernel version mismatch: ' + name)
        active.add(name)
        deps = [dep.replace('-', '_') for dep in info.get('depends', '').split(',') if dep]
        for dep in deps:
            visit(dep)
        active.remove(name)
        data = path.read_bytes()
        selected[name] = {'file': path.name, 'bytes': len(data),
                          'sha256': hashlib.sha256(data).hexdigest(), 'deps': deps}

    for name in seeds:
        visit(name)
    for name, entry in selected.items():
        source = index[name][0]
        target = directory / entry['file']
        if source != target:
            shutil.copyfile(source, target)
    result = {'modules': selected, 'missing': [],
              'provider_seeds': list(PROVIDERS), 'original_roots': list(old['modules']),
              'vendor_vermagic': reference_vermagic,
              'selection': 'Captured USB modules plus piano DT clock/GPIO/I2C/interconnect/IOMMU/PHY providers'}
    (directory / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'modules': len(selected), 'bytes': sum(x['bytes'] for x in selected.values()),
                      'missing': [], 'providers': list(PROVIDERS)}, indent=2))


if __name__ == '__main__':
    main()
