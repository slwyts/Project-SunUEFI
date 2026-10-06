#!/usr/bin/env python3
"""Pinned host-only display MMIO mapping candidate; no prepare or target access."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from compose_piano_dtb import read_fdt

ROOT = Path(__file__).resolve().parents[1]
NATIVE = 'platforms/pianoProductPkg/Library/MemoryMapLib/MemoryMapLib.c'
DTB = 'private/captures/2026-10-03-piano/live.dtb'
DTS = 'kernels/linux-piano/arch/arm64/boot/dts/qcom/sm8750.dtsi'
NATIVE_DTB = 'private/analysis/xbl_config_a-0x8358.dtb'
CLOCK_PE = 'upstream/Mu-Silicium/Binaries/piano/ProductFoundation/ClockDxe/ClockDxe.efi'
PINS = {
    DTB: 'a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7',
    NATIVE: '04ef5a00cee0123d4870a82670e677bfec2af5c360342fc5e0d9092bd7cbb9df',
    NATIVE_DTB: '634ec73dc6d69a07b5af8246e03b0d2ae84dfb9ce9901135121c220443c9cd10',
    # Product staging retains CESTA startup clock refs; the immutable original
    # and exact derivation are checked by piano_inherited_clock.
    CLOCK_PE: '3f459c822c8d8f87ca832b37709c3b56548319bfe60ff01ffd37ddc56d72d967',
    DTS: '531d6a6d53c8e23b94e8ba2747386f3265c1930bfa24c8db4f8fd1c0086940e0',
    'kernels/linux-piano/drivers/clk/qcom/gcc-sm8750.c': '3ac38ce713871b541dd7d4bee7007b14fee36d3a2fc6faa951e7c82fd53da960',
    'kernels/linux-piano/drivers/clk/qcom/dispcc-sm8750.c': 'd33b53c94c12f6f30118dbb20f5c7714aa14ee3421cec436379425cfee2be164',
    'kernels/linux-piano/drivers/gpu/drm/msm/disp/dpu1/dpu_kms.c': 'f8cf490fe9a74c3deb219b28345b66b54085d4b0eef34f3e7e765465f4dfba99',
    'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include/Library/MemoryMapLib.h': 'c60c4b80e81388d40148cc014a25f22a20a89a1722c6dd0f1e784efa3268816b',
    'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Library/MemoryInitPeiLib/MemoryInitPei.c': 'dced274a5e31484b647e643fa255439a9a6f6eb5fc210671233614ed80ef2edb',
    'upstream/Mu-Silicium/Mu_Basecore/UefiCpuPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c': 'bf3be06245f00c6d1b24b94eb0068f607387780966297070836799fea2aa3521',
    'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Core/Dxe/Gcd/Gcd.c': '3d82f7ff075dc491c8934979e8153dfb112fcab39148a4559a54c301c7dd034a',
    'upstream/Mu-Silicium/Mu_Basecore/ArmPkg/Drivers/CpuDxe/AArch64/Mmu.c': '173073a5a9e827be71aa0222e7af26c6122d159eb314f79bfcc07c6c95d3b533',
    'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/Library/ArmLib.h': '45b870a9f883a953281bde367b8bc40aa1c79783d786d437a1a45bfdcbb8d97c',
    'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Core/Dxe/DxeMain.h': 'b0088aa8b24c9636a47a44b4349e51e9c55fb6151807fb7f2d89cea147242ea3',
}
WINDOWS = (
    ('Piano_Display_GCC', '/soc/clock-controller@100000', 0x100000, 0x1f4200, 0x1f5000),
    ('Piano_Display_DPU', '/soc/qcom,mdss_mdp@ae00000', 0xae00000, 0x93800, 0x94000),
    ('Piano_Display_DISPCC', '/soc/clock-controller@af00000', 0xaf00000, 0x20000, 0x20000),
    ('Piano_Display_CESTA', '/soc/cesta@af27000', 0xaf27000, 0x3000, 0x3000),
)
CESTA_NAMES = ('SDE_CRMB', 'SDE_CRMB_PT', 'SDE_CRMC', 'SDE_CRMV', 'SDE_CRM_COMMON')
CESTA_REGS = ((0xaf27000, 0x400), (0xaf27400, 0x400), (0xaf27800, 0x2000), (0xaf29800, 0x700), (0xaf29f00, 0x100))
SYMBOLS = {'NoHob': 0, 'AddMem': 1, 'AddDev': 2, 'HobOnlyNoCacheSetting': 3, 'AllocOnly': 4,
           'SYS_MEM': 0, 'MMAP_IO': 1, 'MEM_RES': 5, 'SYS_MEM_CAP': 0x703c07, 'EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE': 0x400,
           'BsData': 4, 'EfiLoaderData': 2, 'EfiConventionalMemory': 7, 'EfiMemoryMappedIO': 11,
           'UNCACHED_UNBUFFERED_XN': 0, 'WRITE_BACK': 1, 'WRITE_BACK_XN': 1,
           'WRITE_THROUGH_XN': 5, 'NS_DEVICE': 6, 'DEVICE': 6, 'ARM_MEMORY_REGION_ATTRIBUTE_DEVICE': 6}
ROW = re.compile(r'\{\s*"([^"\n]+)"\s*,\s*(0x[\da-fA-F]+)\s*,\s*(0x[\da-fA-F]+)\s*,\s*([^,{}]+),\s*([^,{}]+),\s*([^,{}]+),\s*([^,{}]+),\s*([^,{}]+)\}')
MARKER = '\n  // Host-validated display MMIO mapping; register access still requires live proof.\n'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def number(token):
    token = token.strip()
    if token in SYMBOLS:
        return SYMBOLS[token]
    if re.fullmatch(r'0x[\da-fA-F]+|\d+', token):
        return int(token, 0)
    raise ValueError('Unsupported typed descriptor token: ' + token)


def span(base, size):
    if type(base) is not int or type(size) is not int or base < 0 or size <= 0 or base > (1 << 64) - 1 - size:
        raise ValueError('Invalid/overflowing span')
    return base, base + size


def overlaps(a, b):
    return a[0] < b[1] and b[0] < a[1]


def parse_native(data):
    text = data.decode()
    array = re.search(r'STATIC\s+EFI_MEMORY_REGION_DESCRIPTOR\s+gMemoryDescriptor\[\]\s*=\s*\{(.*?)\n\};', text, re.S)
    if not array:
        raise ValueError('Native typed array boundary changed')
    rows = []
    for match in ROW.finditer(array[1]):
        name, base, size, *tokens = match.groups()
        values = [number(token) for token in tokens]
        bounds = span(int(base, 16), int(size, 16))
        if len(name) >= 32 or (bounds[0] | bounds[1]) & 4095:
            raise ValueError('Native name/page bounds invalid')
        rows.append({'name': name, 'base': bounds[0], 'end': bounds[1], 'size': bounds[1] - bounds[0],
                     'hob': values[0], 'resource': values[1], 'resource_attrs': values[2],
                     'memory_type': values[3], 'arm_attrs': values[4], 'source_text': match[0]})
    if len(rows) != array[1].count('{') or len({r['name'] for r in rows}) != len(rows):
        raise ValueError('Skipped/duplicate native descriptors')
    for i, a in enumerate(rows):
        for b in rows[i + 1:]:
            if not overlaps((a['base'], a['end']), (b['base'], b['end'])):
                continue
            role = all(r['hob'] == 2 and r['resource'] == 1 and r['memory_type'] == 11 and r['arm_attrs'] == 6 for r in (a, b))
            contains = (a['base'] <= b['base'] and b['end'] <= a['end']) or (b['base'] <= a['base'] and a['end'] <= b['end'])
            if not role or not contains or a['resource_attrs'] != b['resource_attrs']:
                raise ValueError('Conflicting existing native descriptors')
    return text, array, rows


def validate(inputs, native_data, owners=()):
    tree = read_fdt(inputs[DTB])['tree']
    native_tree = read_fdt(inputs[NATIVE_DTB])['tree']
    _, _, native = parse_native(native_data)
    if len(native) != 49:
        raise ValueError('Expected the actual 49-row product table')
    dts = inputs[DTS].decode()
    required = ('reg = <0x0 0x00100000 0x0 0x1f4200>;',
                'reg = <0x0 0x0af00000 0x0 0x20000>;',
                'reg = <0x0 0x0ae00000 0x0 0x1000>;',
                'reg = <0x0 0x0ae01000 0x0 0x93000>,')
    if any(dts.count(value) != 1 for value in required):
        raise ValueError('Kernel resource bounds changed')
    protected = [(r['name'], (r['base'], r['end'])) for r in native]
    if len(owners) > 64:
        raise ValueError('Too many explicit owners')
    for owner in owners:
        if set(owner) != {'name', 'base', 'size'} or not isinstance(owner['name'], str):
            raise ValueError('Malformed owner record')
        protected.append((owner['name'], span(owner['base'], owner['size'])))
    windows = []
    for name, path, base, raw_size, page_size in WINDOWS:
        cesta = name == 'Piano_Display_CESTA'
        props = (native_tree if cesta else tree).get(path)
        if cesta:
            expected_reg = b''.join(struct.pack('>II', *r) for r in CESTA_REGS)
            expected_names = '\0'.join(CESTA_NAMES).encode() + b'\0'
            if not props or props.get('reg') != expected_reg or props.get('reg-names') != expected_names:
                raise ValueError('Native CESTA exact reg/name changed')
            cursor = base
            for start, length in CESTA_REGS:
                if start != cursor:
                    raise ValueError('CESTA role has a gap or overlap')
                cursor = span(start, length)[1]
            if cursor != base + raw_size:
                raise ValueError('CESTA role union changed')
            # Independent Android ROM corroboration, not the source of a
            # guessed FAR envelope. Preserve the unrelated CRM base/RSC roles.
            crm = tree.get('/soc/crm@af21000', {})
            corroborated = b''.join(struct.pack('>II', *r) for r in ((0xaf21000, 0x6000), *CESTA_REGS))
            if crm.get('reg') != corroborated or tree.get('/soc/syscon@af27800', {}).get('reg') != struct.pack('>II', 0xaf27800, 0x2000):
                raise ValueError('Android CRM/CRMC corroboration changed')
        elif not props or props.get('reg', b'')[:8] != struct.pack('>II', base, raw_size):
            raise ValueError('ROM exact reg changed: ' + path)
        if path.endswith('mdss_mdp@ae00000') and props.get('reg-names', b'').split(b'\0')[0] != b'mdp_phys':
            raise ValueError('ROM DPU resource identity changed')
        if not cesta and path != WINDOWS[1][1] and len(props['reg']) != 8:
            raise ValueError('Clock-controller reg tuple changed')
        bounds = span(base, page_size)
        if base & 4095 or page_size != (raw_size + 4095) & ~4095 or bounds[1] > 0x80000000:
            raise ValueError('Display envelope/page boundary invalid')
        for owner, occupied in protected:
            if overlaps(bounds, occupied):
                raise ValueError('Display candidate conflicts with ' + owner)
        for previous in windows:
            if overlaps(bounds, (previous['base'], previous['end'])):
                raise ValueError('Display candidate windows overlap')
        windows.append({'name': name, 'path': path, 'base': base, 'size': page_size, 'end': bounds[1],
                        'raw_size': raw_size, 'padding_bytes': page_size - raw_size,
                        'hob': 'AddDev', 'resource_type': 'MMAP_IO', 'resource_attrs': 'EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE',
                        'memory_type': 'EfiMemoryMappedIO', 'arm_attrs': 'ARM_MEMORY_REGION_ATTRIBUTE_DEVICE'})
    return windows


def render(native_data, windows):
    text, array, before = parse_native(native_data)
    added = MARKER
    added += '\n'.join('  {"%s", 0x%X, 0x%X, AddDev, MMAP_IO, EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE, EfiMemoryMappedIO, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},' %
                       (r['name'], r['base'], r['size']) for r in windows)
    body = array[1].rstrip()
    if not body.endswith(','):
        body += ','
    generated = (text[:array.start(1)] + body + added + text[array.end(1):]).encode()
    _, _, after = parse_native(generated)
    if after[:len(before)] != before or len(after) != len(before) + len(windows) or len(after) >= 128:
        raise ValueError('Generated typed rows altered the native table')
    if any(r['hob'] != 2 or r['resource'] != 1 or r['resource_attrs'] != 0x400 or r['memory_type'] != 11 or r['arm_attrs'] != 6 for r in after[len(before):]):
        raise ValueError('Generated MMIO descriptor semantics differ')
    return generated


def source_files(root=ROOT):
    root = Path(root)
    return (root / 'tools/piano_display_mapping.py', *(root / p for p in PINS if p != NATIVE))


def prepare(root, input_native_text, owners=()):
    root = Path(root)
    generator = root / 'tools/piano_display_mapping.py'
    if generator.is_symlink():
        raise ValueError('Display mapping generator is a symlink')
    generator_bytes = generator.read_bytes()
    native = input_native_text.encode()
    if digest(native) != PINS[NATIVE]:
        raise ValueError('Display mapping original 49-row SHA drift')
    inputs = {}
    for relative, expected in PINS.items():
        if relative == NATIVE:
            continue
        path = root / relative
        if path.is_symlink():
            raise ValueError('Pinned input is a symlink: ' + relative)
        data = path.read_bytes()
        if digest(data) != expected:
            raise ValueError('Display mapping source SHA drift: ' + relative)
        inputs[relative] = data
    windows = validate(inputs, native, owners)
    source = render(native, windows)
    metadata = {'status': 'HOST_DISPLAY_MMIO_CANDIDATE_REQUIRES_LIVE_AT_GCD',
                'original_rows': 49, 'candidate_rows': 53, 'windows': windows,
                'original_native_sha256': digest(native),
                'generator_sha256': digest(generator_bytes),
                'source_inputs': {p: {'sha256': digest(b), 'bytes': len(b)} for p, b in inputs.items()},
                'candidate_sha256': digest(source), 'owners': list(owners),
                'conventional_bytes_added': 0, 'framebuffer_changed': False, 'high_ddr_changed': False,
                'applied_to_product': False, 'hardware_verified': False, 'register_access_authorized': False,
                'limits': ['UC resource HOB capabilities are not live GCD attributes.',
                           'Mapped page padding and DISPCC TZ offsets are not a register-read whitelist.',
                           'Actual AT/PTE/GCD and guarded-register admission remain required.']}
    if any((root / p).read_bytes() != b for p, b in inputs.items()):
        raise ValueError('Display mapping source changed during generation')
    if generator.read_bytes() != generator_bytes:
        raise ValueError('Display mapping generator changed during generation')
    return source.decode(), metadata


def recover_original(expanded_text):
    text, array, rows = parse_native(expanded_text.encode())
    if len(rows) not in (52, 53) or array[1].count(MARKER) != 1:
        raise ValueError('Expanded display table block missing/duplicated')
    original_body = array[1].split(MARKER)[0]
    if not original_body.endswith(','):
        raise ValueError('Expanded display table separator changed')
    original = text[:array.start(1)] + original_body[:-1] + text[array.end(1):]
    if digest(original.encode()) != PINS[NATIVE]:
        raise ValueError('Expanded display source original 49-row SHA drift')
    return original


def original_native(root=ROOT):
    root = Path(root)
    path = root / NATIVE
    if path.is_symlink():
        raise ValueError('Native source is a symlink')
    text = path.read_text()
    if digest(text.encode()) == PINS[NATIVE]:
        return text
    original = recover_original(text)
    expected, _ = prepare(root, original)
    historical = render(original.encode(), validate({p:(root/p).read_bytes() for p in PINS if p!=NATIVE}, original.encode())[:3]).decode()
    if text not in (expected, historical):
        raise ValueError('Native expanded source differs from exact reconstruction')
    return original


def verify(root, expanded_text, record):
    if len(parse_native(expanded_text.encode())[2]) != 53:
        raise ValueError('Current display verification requires 53 rows')
    original = recover_original(expanded_text)
    expected, expected_record = prepare(root, original, record.get('owners', ()))
    if expected != expanded_text or record != expected_record:
        raise ValueError('Expanded display source/record differs from pinned reconstruction')
    return True


def create(root=ROOT, owners=()):
    expanded, record = prepare(root, original_native(root), owners)
    return expanded.encode(), record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--owners', type=Path, help='explicit current owner intervals JSON; never inferred from ready flags')
    args = parser.parse_args()
    try:
        directory = args.output_dir.resolve()
        if any(directory.is_relative_to((ROOT / p).resolve()) for p in ('platforms', 'upstream', 'bootprofiles', 'tools', 'private')):
            raise ValueError('Candidate output cannot overwrite source/staging/evidence')
        owners = json.loads(args.owners.read_text()) if args.owners else []
        source, record = create(owners=owners)
        directory.mkdir(parents=True, exist_ok=True)
        for name in ('MemoryMapLib.c', 'display-mapping.json'):
            if (directory / name).is_symlink():
                raise ValueError('Candidate output is a symlink')
        (directory / 'MemoryMapLib.c').write_bytes(source)
        (directory / 'display-mapping.json').write_text(json.dumps(record, indent=2) + '\n')
    except (ValueError, OSError, KeyError) as error:
        parser.exit(2, 'Display mapping: ' + str(error) + '\n')
    print(json.dumps({'output': str(directory), 'sha256': record['candidate_sha256'], 'status': record['status']}, indent=2))


if __name__ == '__main__':
    main()
