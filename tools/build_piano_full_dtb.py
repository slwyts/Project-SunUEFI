#!/usr/bin/env python3
"""Host-only full Piano overlay folding; explicit kernel headers and ROM layers.

Raw DTBs contain boot metadata and remain in the private output directory.
No device command, source mutation, download, memory authorization or enable gate.
"""
import argparse
import copy
import ctypes
import hashlib
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path
from compose_piano_dtb import read_fdt, write_fdt
from piano_dtb_impact import map_diagnostics

ROOT = Path(__file__).resolve().parents[1]
BASE_SHA = '0f49ab7dad027d7697b1c57bf4a77ffdc607f0fe83effd2473a67eb8f8b1326d'
STOCK_SHA = '06283997e0bfca9b9eac2b3e3328e8dc5df60433e0bb4ed89f96ed385b6ad955'
LIVE_SHA = '8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc'
DEBIAN_COMMIT = 'fd6266d73f3442b23362260c3aa0c86782e0b52c'
KERNEL_COMMIT = '352508459733d3e6d349ea5581a8dd2fd8bb4180'
TRANSIENT = {'linux,initrd-start', 'linux,initrd-end', 'kaslr-seed', 'rng-seed'}
PROVIDERS = {'clocks': '#clock-cells', 'assigned-clocks': '#clock-cells',
             'assigned-clock-parents': '#clock-cells', 'resets': '#reset-cells',
             'iommus': '#iommu-cells', 'dmas': '#dma-cells', 'phys': '#phy-cells',
             'power-domains': '#power-domain-cells', 'interconnects': '#interconnect-cells',
             'interrupts-extended': '#interrupt-cells', 'pwms': '#pwm-cells',
             'sound-dai': '#sound-dai-cells', 'io-channels': '#io-channel-cells',
             'mboxes': '#mbox-cells'}
SINGLES = {'remote-endpoint', 'backlight', 'kinetic,secondary-backlight',
           'interrupt-parent', 'qcom,rproc-handle', 'qcom,wsa-macro-handle'}
LISTS = {'memory-region', 'pinctrl-0', 'pinctrl-1', 'pinctrl-2', 'pinctrl-3'}
# Explicit downstream ABI lists used by the captured stock layer. These are
# phandle identities, not integers heuristically recognized by their values.
LISTS |= {'panel', 'actuator-src', 'eeprom-src', 'led-flash-src', 'connectors',
          'asoc-codec', 'qcom,dsi-ctrl', 'qcom,dsi-phy', 'qcom,mdp',
          'qcom,dsi-default-panel', 'qcom,dp-pll', 'qcom,ext-disp',
          'pll_codes_region', 'qcom,panel-supply-entries', 'qcom,rx-slave',
          'qcom,tx-slave', 'qcom,wcd-rst-gpio-node', 'qcom,spkr-sd-n-node'}
LEGACY_GPIOS = {'mos-ctrl-gpio'}
AUDITED_PREFIX = {
    ('/soc/ssusb@a600000', 'dr_mode'): (b'peripheral\0', 'boot/dtbo-piano-usb-nopd9.dts:3127',
        'Published fragment29 selects USB gadget peripheral role.'),
    ('/reserved-memory/splash_region', 'no-map'): (b'', 'boot/dtbo-piano-usb-nopd9.dts:19104',
        'Published fragment131 avoids simpledrm WC/linear mapping collision at unchanged splash range.')
}


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def execute(argv, log=None):
    result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, timeout=60)
    if log:
        log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise ValueError('command failed: ' + str(argv[0]) + ': ' + result.stderr[-1800:])
    return result


def pinned(path, expected):
    data = path.read_bytes()
    if sha(data) != expected:
        raise ValueError('input hash mismatch: ' + str(path))
    return data


def git_commit(path, expected):
    commit = execute(['git', '-C', path, 'rev-parse', 'HEAD']).stdout.strip()
    if commit != expected:
        raise ValueError('Git source commit mismatch: ' + str(path))
    if execute(['git', '-C', path, 'status', '--porcelain', '--untracked-files=no']).stdout:
        raise ValueError('dirty tracked source: ' + str(path))
    return commit


def libcheck(blob, library):
    lib = ctypes.CDLL(str(library))
    lib.fdt_check_full.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.fdt_check_full.restype = ctypes.c_int
    lib.fdt_strerror.argtypes = [ctypes.c_int]
    lib.fdt_strerror.restype = ctypes.c_char_p
    data = ctypes.create_string_buffer(blob)
    status = lib.fdt_check_full(data, len(blob))
    if status:
        raise ValueError('libfdt full check: ' + lib.fdt_strerror(status).decode())
    return read_fdt(blob)


def fragments(parsed):
    return sorted(p for p in parsed['tree'] if re.fullmatch(r'/fragment@[0-9]+', p))


def subtree(tree, path):
    return {p: v for p, v in tree.items() if p == path or p.startswith(path + '/')}


def reference_offsets(parsed, path, key, raw):
    """Known binding positions only; never guess phandles from integer values."""
    if not (key in SINGLES or key.endswith('-supply') or key in LISTS or
            key in PROVIDERS or key in ('gpios', 'gpio') or key.endswith('-gpios') or
            key in LEGACY_GPIOS or key == 'iommu-addresses'):
        return None
    if len(raw) % 4:
        raise ValueError('unaligned reference')
    cells = list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))
    tree, handles = parsed['tree'], parsed['phandles']
    if key in SINGLES or key.endswith('-supply'):
        if len(cells) != 1:
            raise ValueError('single phandle cell count')
        positions = [0]
    elif key in LISTS:
        positions = list(range(len(cells)))
    else:
        provider = '#gpio-cells' if key in ('gpios', 'gpio') or key.endswith('-gpios') or key in LEGACY_GPIOS else PROVIDERS.get(key)
        if key == 'iommu-addresses':
            positions = []
            index = 0
            while index < len(cells):
                handle = cells[index]
                if handle not in handles:
                    raise ValueError('unresolved iommu-addresses device')
                parent = handles[handle].rsplit('/', 1)[0] or '/'
                widths = [tree[parent].get(n) for n in ('#address-cells', '#size-cells')]
                if any(x is None or len(x) != 4 for x in widths):
                    raise ValueError('missing iommu-addresses parent cells')
                width = sum(int.from_bytes(x, 'big') for x in widths)
                if width < 1 or width > 8 or index + 1 + width > len(cells):
                    raise ValueError('iommu-addresses tuple bounds')
                positions.append(index)
                index += 1 + width
            return positions
        if provider is None:
            return None
        positions = []
        index = 0
        while index < len(cells):
            handle = cells[index]
            # OF uses a zero cell as an empty assigned-clock slot.
            if handle == 0 and key in ('clocks', 'assigned-clocks', 'assigned-clock-parents'):
                index += 1
                continue
            if handle not in handles:
                raise ValueError('unresolved phandle')
            count = tree[handles[handle]].get(provider)
            if count is None or len(count) != 4:
                raise ValueError('missing provider ' + provider)
            count = int.from_bytes(count, 'big')
            if count > 32 or index + 1 + count > len(cells):
                raise ValueError('provider cell count mismatch')
            positions.append(index)
            index += 1 + count
    for position in positions:
        if cells[position] not in handles:
            raise ValueError('unresolved phandle')
    return positions


def resources(parsed):
    errors, count = [], 0
    for path, props in parsed['tree'].items():
        if path.startswith('/__'):
            continue
        for key, raw in props.items():
            try:
                positions = reference_offsets(parsed, path, key, raw)
                if positions is not None:
                    count += len(positions)
                if key == 'remote-endpoint' and positions:
                    target = parsed['phandles'][int.from_bytes(raw, 'big')]
                    own = props.get('phandle', props.get('linux,phandle'))
                    if own is None or parsed['tree'][target].get('remote-endpoint') != own:
                        raise ValueError('non-reciprocal endpoint')
            except ValueError as error:
                errors.append({'path': path, 'property': key, 'reason': str(error)})
    return {'resolved_reference_cells': count, 'errors': errors}


def metadata_identity(parsed):
    """Canonical local/external fixups, so different phandle numbering is harmless."""
    tree = parsed['tree']
    markers = {}
    for label, raw in tree.get('/__fixups__', {}).items():
        for item in raw.rstrip(b'\0').split(b'\0'):
            path, key, offset = item.decode().rsplit(':', 2)
            markers[(path, key, int(offset))] = ('external', label)
    for fixpath, props in tree.items():
        if not fixpath.startswith('/__local_fixups__/'):
            continue
        path = fixpath[len('/__local_fixups__'):]
        for key, offsets in props.items():
            for offset in struct.unpack('>' + 'I' * (len(offsets) // 4), offsets):
                raw = tree[path][key]
                handle = int.from_bytes(raw[offset:offset + 4], 'big')
                markers[(path, key, offset)] = ('local', parsed['phandles'].get(handle, 'unresolved'))
    by_property = {}
    for (path, key, offset), marker in markers.items():
        by_property.setdefault((path, key), []).append((offset, marker))
    result = {}
    for path, props in tree.items():
        if path.startswith('/__'):
            continue
        values = {}
        for key, raw in props.items():
            if key in ('phandle', 'linux,phandle'):
                values[key] = ('node', path)
                continue
            replaced = by_property.get((path, key), [])
            if replaced:
                value = bytearray(raw)
                for offset, _ in replaced:
                    value[offset:offset + 4] = b'\0' * 4
                values[key] = (bytes(value), tuple(sorted(replaced)))
            else:
                values[key] = raw
        result[path] = values
    return result


def stock_evidence(full, stock):
    a, b = metadata_identity(full), metadata_identity(stock)
    missing, different, equal = [], [], []
    for fragment in fragments(stock):
        if fragment not in full['tree']:
            missing.append(fragment)
        elif subtree(a, fragment) == subtree(b, fragment):
            equal.append(fragment)
        else:
            different.append(fragment)
    return {'stock_fragment_count': len(fragments(stock)), 'full_fragment_count': len(fragments(full)),
            'embedded_stock_fragment_paths': not missing, 'canonical_equal': equal,
            'canonical_different': different, 'missing': missing,
            'captured_stock_entry_exact': not missing and not different,
            'decision': 'apply complete chain once to selected ROM base; never replay onto live'}


def runtime_backfill(rom, live, folded, blocked=frozenset()):
    """Keep genuine ABL differences; map reference identities by node path.

    Numeric phandle renumbering is not hardware state and is never replayed.
    Android initrd/random seeds are removed. No guessed integer-cell remap.
    """
    result = copy.deepcopy(folded)
    changes = []
    absent = {path for path in live['tree'] if not path.startswith('/__') and path not in result['tree']}
    next_handle = max(folded['phandles'], default=0)
    for path in live['tree']:
        if path in absent:
            result['tree'][path] = {}
            if 'phandle' in live['tree'][path] or 'linux,phandle' in live['tree'][path]:
                next_handle += 1
                if next_handle >= 0xffffffff:
                    raise ValueError('phandle range exhausted')
                result['tree'][path]['phandle'] = struct.pack('>I', next_handle)
    for path, props in live['tree'].items():
        if path.startswith('/__'):
            continue
        if path not in result['tree']:
            result['tree'][path] = {}
        old = rom['tree'].get(path, {})
        for key in sorted(set(props) | set(old)):
            value = props.get(key)
            if (path, key) in blocked or (path not in absent and old.get(key) == value) or key in ('phandle', 'linux,phandle'):
                continue
            if path == '/chosen' and key in TRANSIENT:
                continue
            if value is not None:
                positions = reference_offsets(live, path, key, value)
                if positions:
                    cells = list(struct.unpack('>' + 'I' * (len(value) // 4), value))
                    for position in positions:
                        target = live['phandles'][cells[position]]
                        if target not in result['tree']:
                            raise ValueError('runtime target absent from folded tree: ' + target)
                        handle = result['tree'][target].get('phandle', result['tree'][target].get('linux,phandle'))
                        if handle is None:
                            raise ValueError('runtime target has no folded phandle: ' + target)
                        cells[position] = int.from_bytes(handle, 'big')
                    value = struct.pack('>' + 'I' * len(cells), *cells)
                result['tree'][path][key] = value
            else:
                result['tree'][path].pop(key, None)
            changes.append({'path': path, 'property': key, 'operation': 'delete' if value is None else 'preserve-live',
                            'bytes': None if value is None else len(value)})
    for path, props in result['tree'].items():
        if path == '/chosen' or path.startswith('/chosen/'):
            for key in TRANSIENT:
                props.pop(key, None)
    result['reservations'] = list(live['reservations'])
    result['boot_cpu'] = live['boot_cpu']
    return read_fdt(write_fdt(result)), changes


def stock_prefix(full):
    """Compiled once: retain embedded stock fragment IDs below published 200."""
    result = copy.deepcopy(full)
    removed = [p for p in fragments(full) if int(p.rsplit('@', 1)[1]) >= 200]
    for path in list(result['tree']):
        if any(path == p or path.startswith(p + '/') or
               path == '/__local_fixups__' + p or path.startswith('/__local_fixups__' + p + '/') for p in removed):
            del result['tree'][path]
    for label, raw in list(result['tree'].get('/__symbols__', {}).items()):
        path = raw.rstrip(b'\0').decode()
        if any(path == p or path.startswith(p + '/') for p in removed):
            del result['tree']['/__symbols__'][label]
    for label, raw in list(result['tree'].get('/__fixups__', {}).items()):
        entries = [entry for entry in raw.rstrip(b'\0').split(b'\0')
                   if not any(entry.decode().startswith(p + ':') or entry.decode().startswith(p + '/') for p in removed)]
        if entries:
            result['tree']['/__fixups__'][label] = b'\0'.join(entries) + b'\0'
        else:
            del result['tree']['/__fixups__'][label]
    return read_fdt(write_fdt(result))


def additional_operations(full, rom, folded):
    """Resolve actual public fragment targets, including external/local fixups."""
    tree = full['tree']
    external = {}
    for label, raw in tree.get('/__fixups__', {}).items():
        for entry in raw.rstrip(b'\0').split(b'\0'):
            external[entry.decode()] = label
    definitions = {}
    for label, raw in tree.get('/__symbols__', {}).items():
        definitions.setdefault(raw.rstrip(b'\0').decode(), []).append(label)
    operations = set()
    for fragment in fragments(full):
        if int(fragment.rsplit('@', 1)[1]) < 200:
            continue
        props = tree[fragment]
        if 'target-path' in props:
            target = props['target-path'].rstrip(b'\0').decode()
        elif fragment + ':target:0' in external:
            label = external[fragment + ':target:0']
            raw = rom['tree'].get('/__symbols__', {}).get(label)
            if raw is None:
                raise ValueError('public fragment external target missing: ' + label)
            target = raw.rstrip(b'\0').decode()
        else:
            handle = int.from_bytes(props.get('target', b''), 'big')
            definition = full['phandles'].get(handle)
            targets = {folded['tree'].get('/__symbols__', {}).get(label) for label in definitions.get(definition, [])}
            targets.discard(None)
            if len(targets) != 1:
                raise ValueError('public local fragment target unresolved: ' + fragment)
            target = targets.pop().rstrip(b'\0').decode()
        overlay = fragment + '/__overlay__'
        for path, values in subtree(tree, overlay).items():
            actual = target.rstrip('/') + path[len(overlay):] or '/'
            for key in values:
                if key not in ('phandle', 'linux,phandle'):
                    operations.add((actual, key))
    return operations


def semantic_value(parsed, path, key, raw):
    if raw is None:
        return None
    try:
        positions = reference_offsets(parsed, path, key, raw)
    except ValueError:
        positions = None  # Report unresolved shape separately; don't guess.
    if positions:
        cells = list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))
        for position in positions:
            cells[position] = parsed['phandles'][cells[position]]
        return tuple(cells)
    return raw


def classify_property(key):
    if key == 'reg' or key == 'ranges':
        return 'reg-address-layout'
    if 'interrupt' in key or 'irq' in key:
        return 'IRQ'
    if 'gpio' in key or key.startswith('pinctrl'):
        return 'GPIO-pinctrl'
    if key in ('panel', 'connectors', 'qcom,dsi-ctrl', 'qcom,dsi-phy', 'qcom,mdp', 'qcom,dsi-default-panel'):
        return 'display-phandle-identity'
    if 'supply' in key or 'regulator' in key or 'voltage' in key or 'microvolt' in key:
        return 'supply-voltage'
    if 'clock' in key or key in ('resets', 'reset-names'):
        return 'clock-reset-IDs'
    if 'interconnect' in key:
        return 'ICC-IDs'
    if 'iommu' in key or 'dma' in key:
        return 'IOMMU-DMA'
    if 'memory' in key or key in ('no-map', 'reusable'):
        return 'memory-reservation'
    if key in ('compatible', 'status'):
        return 'binding-status'
    return 'other-raw-property'


def stock_drift(rom, prefix, operations):
    rows = []
    for path in sorted(set(rom['tree']) & set(prefix['tree'])):
        if path.startswith('/__'):
            continue
        a, b = rom['tree'][path], prefix['tree'][path]
        for key in sorted(set(a) | set(b)):
            if key in ('phandle', 'linux,phandle'):
                continue
            old, new = a.get(key), b.get(key)
            if semantic_value(rom, path, key, old) == semantic_value(prefix, path, key, new):
                continue
            rows.append({'path': path, 'property': key, 'category': classify_property(key),
                         'action': 'retain-audited-public-prefix-modification' if (path, key) in AUDITED_PREFIX else
                         'retain-explicit-public-functional-operation' if (path, key) in operations else 'restore-current-ROM',
                         'current_rom_hex': None if old is None else old.hex() if len(old) <= 64 else 'sha256:' + sha(old),
                         'published_prefix_hex': None if new is None else new.hex() if len(new) <= 64 else 'sha256:' + sha(new)})
    return rows


def physical_delta(before, after):
    changes = []
    for path in sorted(set(before['tree']) | set(after['tree'])):
        if path == '/memory' or path.startswith('/reserved-memory') or path.startswith('/ddr-regions'):
            for key in ('reg', 'no-map', 'reusable', 'status'):
                a = before['tree'].get(path, {}).get(key)
                b = after['tree'].get(path, {}).get(key)
                if a != b:
                    changes.append({'path': path, 'property': key,
                                    'before_hex': None if a is None else a.hex(),
                                    'after_hex': None if b is None else b.hex()})
    return changes


def source_inputs(debian, kernel, cpp, source):
    dependency = execute([cpp, '-nostdinc', '-undef', '-x', 'assembler-with-cpp',
                          '-I', kernel / 'include', '-I', source.parent, '-M', source]).stdout
    paths = [Path(x).resolve() for x in dependency.replace('\\\n', ' ').split(':', 1)[1].split()]
    result = []
    for path in sorted(set(paths)):
        owner = 'debian' if path.is_relative_to(debian) else 'kernel' if path.is_relative_to(kernel) else None
        if owner is None:
            raise ValueError('unexpected preprocessing dependency: ' + str(path))
        result.append({'owner': owner, 'path': str(path.relative_to(debian if owner == 'debian' else kernel)),
                       'sha256': sha(path.read_bytes())})
    return result


def risk_inventory(debian, inputs):
    records = []
    patterns = ('bypass', 'devmem', 'stand-in', 'dummy', 'fixed-clock', 'regulator-fixed', 'driverless')
    for row in inputs:
        if row['owner'] != 'debian':
            continue
        for line, text in enumerate((debian / row['path']).read_text().splitlines(), 1):
            found = [word for word in patterns if word in text.lower()]
            if found:
                records.append({'source': row['path'], 'line': line, 'kinds': found, 'text': text.strip()[:240]})
    return records


def runtime_prerequisites(debian):
    records = []
    paths = ('initramfs/rootfs-init', 'initramfs/tests/piano-qup-smmu',
             'rootfs/overlay/usr/lib/piano/display-start')
    for relative in paths:
        path = debian / relative
        if not path.is_file():
            raise ValueError('published runtime prerequisite source missing: ' + relative)
        lines = []
        for number, text in enumerate(path.read_text().splitlines(), 1):
            if any(word in text.lower() for word in ('devmem', 'bypass', 'clkref', 'partlabel=userdata', 'smmu')):
                lines.append({'line': number, 'text': text.strip()[:240]})
        records.append({'source': relative, 'sha256': sha(path.read_bytes()),
                        'observed_dependencies': lines, 'executed': False,
                        'satisfied_in_this_candidate': False})
    return records


def correct_uart_icc(args, stage, candidate, output):
    """One audited resource correction; physical/enable/provider graph unchanged."""
    source = args.resource_overlay.resolve()
    target = '/soc/qcom,qupv3_1_geni_se@ac0000/qcom,qup_uart@a9c000'
    props = candidate['tree'].get(target, {})
    if b'qcom,geni-debug-uart\0' not in props.get('compatible', b'') or props.get('reg') != struct.pack('>II', 0xa9c000, 0x4000):
        raise ValueError('UART7 resource correction target identity mismatch')
    base, pp, overlay, result = (stage / x for x in ('resource-base.dtb', 'resource-fix.pp.dts', 'resource-fix.dtbo', 'resource-fixed.dtb'))
    base.write_bytes(write_fdt(candidate))
    execute([args.cpp, '-nostdinc', '-undef', '-x', 'assembler-with-cpp', '-I', args.kernel_tree.resolve() / 'include',
             '-o', pp, source], output / 'resource-cpp.log')
    execute([args.dtc, '-@', '-I', 'dts', '-O', 'dtb', '-o', overlay, pp], output / 'resource-dtc.log')
    execute([args.fdtoverlay, '-i', base, '-o', result, overlay], output / 'resource-fold.log')
    parsed = libcheck(result.read_bytes(), args.libfdt)
    changes = []
    for path in set(candidate['tree']) | set(parsed['tree']):
        for key in set(candidate['tree'].get(path, {})) | set(parsed['tree'].get(path, {})):
            if candidate['tree'].get(path, {}).get(key) != parsed['tree'].get(path, {}).get(key):
                if path != target or key not in ('interconnects', 'interconnect-names'):
                    raise ValueError('resource correction changed unreviewed property: ' + path + ':' + key)
                changes.append({'path': path, 'property': key})
    positions = reference_offsets(parsed, target, 'interconnects', parsed['tree'][target]['interconnects'])
    if len(positions) != 4 or parsed['tree'][target]['interconnect-names'] != b'qup-core\0qup-config\0':
        raise ValueError('UART ICC correction does not have exact two bound paths')
    return parsed, {'source': str(source.relative_to(ROOT)), 'sha256': sha(source.read_bytes()),
                    'overlay_sha256': sha(overlay.read_bytes()), 'property_changes': changes,
                    'kernel_evidence': 'arch/arm64/boot/dts/qcom/sm8750.dtsi:2014-2028',
                    'scope': 'Only UART7 ICC, no pin/clock/register/provider/status changes.'}


def build(args):
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    debian, kernel = args.debian_tree.resolve(), args.kernel_tree.resolve()
    source = debian / 'boot/dtbo-piano-camera.dts'
    manifest = {'schema_version': 1, 'status': 'INCOMPLETE', 'hardware_verified': False,
                'memory_ownership_authorized': False, 'device_operations': False}
    try:
        manifest['source_commits'] = {'debian': git_commit(debian, args.debian_commit),
                                      'kernel': git_commit(kernel, args.kernel_commit)}
        base = pinned(args.base, args.base_sha256)
        stock = pinned(args.stock_overlay, args.stock_sha256)
        live = pinned(args.live, args.live_sha256)
        for name, data in [('base', base), ('stock_overlay', stock), ('live', live)]:
            libcheck(data, args.libfdt)
            manifest[name] = {'sha256': sha(data), 'bytes': len(data)}
        manifest['selected_rom'] = {'dtb_index': 4, 'dtbo_index': 0}
        manifest['inputs'] = source_inputs(debian, kernel, args.cpp, source)
        manifest['retained_risks'] = risk_inventory(debian, manifest['inputs'])
        manifest['published_runtime_prerequisites'] = runtime_prerequisites(debian)
        with tempfile.TemporaryDirectory(prefix='piano-full-fold-') as temporary:
            stage = Path(temporary)
            pp, overlay = stage / 'camera.pp.dts', stage / 'camera.dtbo'
            execute([args.cpp, '-nostdinc', '-undef', '-x', 'assembler-with-cpp', '-I', kernel / 'include',
                     '-I', source.parent, '-o', pp, source], output / 'cpp.log')
            compile_result = execute([args.dtc, '-@', '-I', 'dts', '-O', 'dtb', '-o', overlay, pp], output / 'dtc.log')
            full = libcheck(overlay.read_bytes(), args.libfdt)
            manifest['overlay'] = {'sha256': sha(overlay.read_bytes()), 'bytes': overlay.stat().st_size,
                                    'warning_lines': len(compile_result.stderr.splitlines())}
            manifest['stock_embedding'] = stock_evidence(full, read_fdt(stock))
            if not manifest['stock_embedding']['canonical_equal']:
                raise ValueError('no canonical stock fragment evidence in published full chain')
            rom_path, folded_path = stage / 'rom-merged.dtb', stage / 'folded.dtb'
            execute([args.fdtoverlay, '-i', args.base, '-o', rom_path, args.stock_overlay], output / 'rom-fold.log')
            execute([args.fdtoverlay, '-i', args.base, '-o', folded_path, overlay], output / 'full-fold.log')
            rom = libcheck(rom_path.read_bytes(), args.libfdt)
            raw_fold = libcheck(folded_path.read_bytes(), args.libfdt)
            prefix = stock_prefix(full)
            prefix_path, prefix_folded = stage / 'published-stock-prefix.dtbo', stage / 'published-stock-prefix.dtb'
            prefix_path.write_bytes(write_fdt(prefix))
            execute([args.fdtoverlay, '-i', args.base, '-o', prefix_folded, prefix_path], output / 'prefix-fold.log')
            prefix_merged = libcheck(prefix_folded.read_bytes(), args.libfdt)
            operations = additional_operations(full, rom, raw_fold)
            intents = []
            for (path, key), (value, source_location, reason) in AUDITED_PREFIX.items():
                if prefix_merged['tree'].get(path, {}).get(key) != value:
                    raise ValueError('audited public prefix value changed: ' + path + ':' + key)
                if path.startswith('/reserved-memory') and prefix_merged['tree'][path].get('reg') != rom['tree'][path].get('reg'):
                    raise ValueError('audited splash physical range changed')
                operations.add((path, key))
                intents.append({'path': path, 'property': key, 'source_location': source_location, 'reason': reason})
            drift = stock_drift(rom, prefix_merged, operations)
            reconciled, stock_patches = runtime_backfill(prefix_merged, rom, raw_fold, operations)
            candidate, patches = runtime_backfill(rom, read_fdt(live), reconciled)
            before_correction = resources(candidate)
            candidate, correction = correct_uart_icc(args, stage, candidate, output)
            final = write_fdt(candidate)
            libcheck(final, args.libfdt)
            final_path = stage / 'Piano-full-camera.dtb'
            final_path.write_bytes(final)
            execute([args.dtc, '-I', 'dtb', '-O', 'dtb', '-o', stage / 'roundtrip.dtb', final_path], output / 'roundtrip.log')
            roundtrip = libcheck((stage / 'roundtrip.dtb').read_bytes(), args.libfdt)
            if roundtrip['tree'] != candidate['tree'] or roundtrip['reservations'] != candidate['reservations']:
                raise ValueError('dtc roundtrip semantic drift')
            baseline_resources, candidate_resources = resources(read_fdt(live)), resources(candidate)
            known = {(x['path'], x['property'], x['reason']) for x in baseline_resources['errors']}
            new_errors = [x for x in candidate_resources['errors'] if (x['path'], x['property'], x['reason']) not in known]
            for row in new_errors:
                props = candidate['tree'][row['path']]
                row['empty_property'] = props.get(row['property']) == b''
                row['explicit_public_operation'] = (row['path'], row['property']) in operations
                row['declared_status'] = props.get('status', b'okay\0').rstrip(b'\0').decode(errors='replace')
            before_errors = [x for x in before_correction['errors'] if (x['path'], x['property'], x['reason']) not in known]
            impact = map_diagnostics(kernel, candidate, before_errors, new_errors)
            manifest.update(runtime_patches=patches, stock_reconciliation_patches=stock_patches,
                            stock_property_drift=drift, explicit_public_operations=len(operations), live_reserve_map_preserved=True,
                            audited_public_prefix_intents=intents,
                            applied_resource_correction=correction, diagnostic_impact=impact,
                            memory_reserved_delta=physical_delta(read_fdt(live), candidate),
                            baseline_resources=baseline_resources, candidate_resources=candidate_resources,
                            new_resource_errors=new_errors, nodes=len(candidate['tree']),
                            phandles=len(candidate['phandles']), removed_android_transients=sorted(TRANSIENT),
                            output={'file': 'Piano-full-camera.dtb', 'sha256': sha(final), 'bytes': len(final)},
                            status='OFFLINE_FOLDED_CANDIDATE_WITH_UNRESOLVED_GRAPH' if new_errors else 'OFFLINE_FOLDED_CANDIDATE')
            if new_errors and not impact['remaining_consumed_blockers'] and not impact['review_required']:
                manifest['status'] = 'OFFLINE_FOLDED_CANDIDATE_WITH_LEGACY_DIAGNOSTICS'
            for name, blob in [('camera.dtbo', overlay.read_bytes()), ('Piano-full-camera.dtb', final)]:
                path = output / name
                if path.exists() and path.read_bytes() != blob:
                    raise ValueError('refuse to replace a different existing candidate: ' + str(path))
                path.write_bytes(blob)
    except Exception as error:
        manifest['status'] = 'FAILED'
        manifest['error'] = str(error)
        raise
    finally:
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel-tree', type=Path, required=True)
    ap.add_argument('--debian-tree', type=Path, default=ROOT / 'upstream/debian-piano-current')
    ap.add_argument('--base', type=Path, default=ROOT / 'private/analysis/vendor_boot_a-0x21b5430.dtb')
    ap.add_argument('--stock-overlay', type=Path, default=ROOT / 'private/analysis/dtbo_a-0x40.dtb')
    ap.add_argument('--live', type=Path, default=ROOT / 'private/analysis/android-board-runtime-2026-10-05/live.dtb')
    ap.add_argument('--base-sha256', default=BASE_SHA)
    ap.add_argument('--stock-sha256', default=STOCK_SHA)
    ap.add_argument('--live-sha256', default=LIVE_SHA)
    ap.add_argument('--debian-commit', default=DEBIAN_COMMIT)
    ap.add_argument('--kernel-commit', default=KERNEL_COMMIT)
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--cpp', default='cpp')
    ap.add_argument('--dtc', type=Path, default=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/dtc')
    ap.add_argument('--fdtoverlay', type=Path, default=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/fdtoverlay')
    ap.add_argument('--libfdt', type=Path, default=ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1')
    ap.add_argument('--resource-overlay', type=Path, default=ROOT / 'configs/linux/dtb/piano-uart7-icc.dtso')
    args = ap.parse_args()
    try:
        result = build(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        ap.exit(1, str(error) + '\n')
    print(json.dumps({key: result[key] for key in ('status', 'nodes', 'phandles', 'output')}))


if __name__ == '__main__':
    main()
