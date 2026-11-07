#!/usr/bin/env python3
"""Build an offline Piano bundle with real FAT/ext4 tools; never access a device."""
import argparse
from collections import Counter
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

from make_kernel_initramfs import inspect_newc
from build_piano_ram_bootstrap import guest_resolve
from prepare_boot_files import fdt_info, image_info, initramfs_info
from compose_piano_dtb import read_fdt, write_fdt

ROOT = Path(__file__).resolve().parents[1]
MIB = 1024 * 1024
CLOCK_EPOCH = 'usr/lib/clock-epoch'


def normalize_mainline_bindings(board):
    """Remove downstream-only bindings retained by the folded Android DT."""
    parsed = read_fdt(board.read_bytes())
    tree, changes = parsed['tree'], []
    # A folded Android tree contains debug blocks and suppliers whose binding
    # is not implemented by mainline. Disable those devices in the DT rather
    # than suppressing autoload for every platform/OF device in userspace.
    inactive_compatibles = {b'arm,coresight-static-funnel',
                           b'arm,coresight-static-replicator', b'qcom,spmi-temp-alarm'}
    trace_peripheral_ids = {0xbb908, 0xbb909, 0xbb961, 0xbb962, 0xbb922}

    def enabled(path):
        while path:
            if tree[path].get('status', b'okay\0') not in (b'ok\0', b'okay\0'):
                return False
            path = path.rsplit('/', 1)[0]
        return True
    legacy = (
        '/soc/qcom,sde_cesta@0x0af30000',
        '/soc/qcom,mdss_mdp@ae00000',
        '/soc/qcom,cam-cpas@ac13000',
        '/soc/qcom,cvp@ab00000',
        '/soc/qcom,vidc@aa00000',
        '/soc/spf_core_platform',
    )
    for path, props in tree.items():
        if props.get('iommus') == b'':
            del props['iommus']
            changes.append(path + ':iommus')
        if (path == '/soc/xiaomi_fingerprint' and
                props.get('compatible') == b'xiaomi,piano-goodix-control\0'):
            # The new controller uses the real mainline supply and GPIOs.
            # Retained vendor suppliers would make fw_devlink wait forever
            # for downstream-only pinctrl and regulator drivers.
            for key in ('fp_3v3_vreg-supply', 'pinctrl-1', 'pinctrl-2',
                        'xiaomi,gpio_irq', 'xiaomi,vendor_names', 'netlink-event'):
                if key in props:
                    del props[key]
                    changes.append(path + ':' + key)
        downstream = any(path == root or path.startswith(root + '/') for root in legacy)
        # qcom_q6v5 consumes a standard ICC path during DSP startup. Keep
        # the converted CDSP path when its NSP provider has the mainline
        # binding; discard only the inherited downstream paths. QTB nodes
        # belong to the old qsmmuv500 binding, not the active ARM SMMU.
        mainline_cdsp = (path == '/soc/remoteproc-cdsp@32300000' and
                        tree.get('/soc/interconnect@320c0000', {}).get('compatible') ==
                        b'qcom,sm8750-nsp-noc\0')
        old_icc = downstream or path in (
            '/soc/remoteproc-adsp@03000000', '/soc/remoteproc-cdsp@32300000'
        ) or (path.startswith('/soc/apps-smmu@15000000/') and '_qtb@' in path)
        if old_icc and not mainline_cdsp:
            for key in ('interconnects', 'interconnect-names'):
                if key in props:
                    del props[key]
                    changes.append(path + ':' + key)
        if path == '/soc/spf_core_platform/lpass-cdc/va-macro@7660000':
            # These are an Android flag and pinctrl handle, not GPIO
            # specifiers. The active codec@7660000 uses its own binding.
            for key in ('qcom,is-used-swr-gpio', 'qcom,va-swr-gpios'):
                if key in props:
                    del props[key]
                    changes.append(path + ':' + key)
        if path in legacy and props.get('status') != b'disabled\0':
            props['status'] = b'disabled\0'
            changes.append(path + ':status')
    disabled = (
        # Display/camera hardware voters are not the AP's RPMh controller.
        # The actual AP supplier is rsc@16500000.
        '/soc/rsc@adc8000', '/soc/rsc@af20000',
        # These optional PMICs also fail probe on this board's stock
        # HyperOS 4. None supplies the active Piano mainline peripherals.
        '/soc/qcom,spmi@c42d000/qcom,pmr735d@a',
        '/soc/qcom,spmi@c42d000/qcom,pmd802x@4',
        '/soc/qcom,spmi@c42d000/qcom,pm8010@c',
        '/soc/qcom,spmi@c42d000/qcom,pm8010@d',
    )
    for path in disabled:
        if path in tree and tree[path].get('status') != b'disabled\0':
            tree[path]['status'] = b'disabled\0'
            changes.append(path + ':status')
    # Folded Android trees also retain devices superseded by mainline
    # nodes. Do not let their drivers wait on the old downstream TLMM, or
    # compete for the same GPIOs with the active display/camera supplies.
    replaced = {
        '/soc/display_gpio_regulator_vsp': '/panel-avdd-ml',
        '/soc/display_gpio_regulator_vsn': '/panel-avee-ml',
        '/soc/gpio-regulator@0': '/cam-wide-avdd-ml',
        '/soc/gpio-regulator@1': '/cam-front-avdd-ml',
        '/soc/qcom,cci0@ac7b000': '/soc/cci-ml@ac7b000',
        '/soc/qcom,dsi-display-primary': '/soc/display-subsystem@ae00000',
    }
    unused = (
        '/soc/qcom,cci1@ac7c000', '/soc/qcom,cci2@ac7d000',
        '/soc/qcom,qupv3_1_geni_se@ac0000/qcom,qup_uart@a9c000',
        '/soc/touch_avdd_vreg', '/soc/usb_nop_phy',
    )
    for path in (*replaced, *unused):
        replacement = tree.get(replaced.get(path))
        if path in replaced and (replacement is None or
                replacement.get('status', b'okay\0') not in (b'ok\0', b'okay\0')):
            continue
        if path in tree and tree[path].get('status') != b'disabled\0':
            tree[path]['status'] = b'disabled\0'
            changes.append(path + ':status')
    for path in ('/reserved-memory/demura_region_0',
                 '/reserved-memory/demura_region_1'):
        props = tree.get(path, {})
        # Zero-length placeholders are not reserved memory. Leave any real
        # allocation supplied by the bootloader intact.
        if props.get('reg') == bytes(16) and props.get('status') != b'disabled\0':
            props['status'] = b'disabled\0'
            changes.append(path + ':status')
    path = '/soc/qcom,spmi@c42d000/qcom,pmk8550@0/sdam@7000/sm1510_present@5d'
    props = tree.get(path, {})
    if (props.get('reg') == struct.pack('>II', 0x5d, 1) and
            props.get('bits') == struct.pack('>II', 5, 5)):
        # The captured field exceeds its one-byte cell (5 + 5 > 8).
        # No mainline consumer uses this vendor flag. Keep the raw byte
        # readable without guessing a bit width; otherwise the invalid
        # slice prevents registration of the entire SDAM, including OCP.
        del props['bits']
        changes.append(path + ':bits')
    for path, props in tree.items():
        compatibles = props.get('compatible', b'').split(b'\0')
        peripheral_id = props.get('arm,primecell-periphid', b'')
        trace = (b'arm,primecell' in compatibles and len(peripheral_id) == 4 and
                 struct.unpack('>I', peripheral_id)[0] & 0x000fffff in trace_peripheral_ids)
        if (inactive_compatibles.intersection(compatibles) or trace) and enabled(path):
            props['status'] = b'disabled\0'
            changes.append(path + ':status')
    if changes:
        board.write_bytes(write_fdt(parsed))
    return {'changes': changes, 'sha256': sha(board)}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, capture_output=True, text=True, **kwargs)


def apply_cpu_model_overlay(board, overlay, work, cpu_only=True):
    """Apply declared board properties to a copy of the captured input."""
    parsed = read_fdt(board.read_bytes())
    before = parsed['tree']
    compiled, derived = work / 'cpu-model.dtbo', work / 'board-with-cpu-model.dtb'
    run(['dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o', compiled, overlay])
    overlay_fdt = read_fdt(compiled.read_bytes())
    fragments = overlay_fdt['tree']
    next_phandle = max(parsed['phandles'], default=0) + 1
    expected = {}
    for path, properties in fragments.items():
        if path.count('/') == 1 and path.startswith('/fragment@'):
            target = properties['target-path'].rstrip(b'\0').decode()
            payload = fragments[path + '/__overlay__']
            require(target in before, 'Board overlay must target an existing node')
            if cpu_only:
                require(set(payload) == {'model'} and target.startswith('/cpus/cpu@'),
                        'CPU model overlay must only add CPU model properties')
            else:
                # libfdt treats an addressless path component as a partial
                # match: led-controller can select led-controller@ee00 even
                # when the exact addressless node also exists. Resolve the
                # declared path in our exact node dictionary, then use a
                # base-tree phandle so fdtoverlay cannot pick its sibling.
                handle = before[target].get('phandle', before[target].get('linux,phandle'))
                if handle is None:
                    require(next_phandle < 0xffffffff, 'Board phandle space is exhausted')
                    handle = struct.pack('>I', next_phandle)
                    next_phandle += 1
                    before[target]['phandle'] = handle
                properties.pop('target-path')
                properties['target'] = handle
            expected[target] = payload
    require(expected, 'Board overlay has no targets')
    overlay_input = board
    if not cpu_only:
        overlay_input = work / 'board-with-exact-targets.dtb'
        overlay_input.write_bytes(write_fdt(parsed))
        # Preserve __symbols__ and __local_fixups__: fdtoverlay must still
        # relocate child phandles and references within the overlay normally.
        compiled.write_bytes(write_fdt(overlay_fdt))
    run(['fdtoverlay', '-i', overlay_input, '-o', derived, compiled])
    output_fdt = read_fdt(derived.read_bytes())
    require(output_fdt['reservations'] == parsed['reservations'] and
            output_fdt['boot_cpu'] == parsed['boot_cpu'],
            'Board overlay changed reserved memory or boot CPU')
    after = output_fdt['tree']
    if cpu_only:
        for target, payload in expected.items():
            for key, value in payload.items():
                require(after[target].get(key) == value, 'CPU model overlay did not apply')
                if key in before[target]: after[target][key] = before[target][key]
                else: after[target].pop(key, None)
    # The CPU model helper changes only existing properties. General board
    # overlays also add standard child nodes and relocate their phandles and
    # __symbols__; fdtoverlay performs that merge. Both new child nodes and
    # references such as flash-leds have relocated phandles, so raw property
    # bytes are not comparable with the independently compiled overlay.
    if cpu_only:
        require(after == before, 'CPU model overlay changed undeclared properties')
    fdt_info(derived)
    result = {'input_sha256': sha(board), 'overlay_sha256': sha(overlay),
              'output_sha256': sha(derived), 'cpu_nodes' if cpu_only else 'nodes': sorted(expected)}
    derived.replace(board)
    return result


def select_panel_vendor(board, requested='auto'):
    parsed = read_fdt(board.read_bytes())
    panels = [(name, props) for name, props in parsed['tree'].items()
              if any(value in props.get('compatible', b'').split(b'\0')
                     for value in (b'xiaomi,piano-boe-nt36532', b'xiaomi,piano-csot-nt36532'))]
    require(len(panels) == 1, 'Input DTB must identify one supported Piano panel; supply the device-specific board DTB')
    node, properties = panels[0]
    original = 'boe' if b'xiaomi,piano-boe-nt36532' in properties['compatible'] else 'csot'
    # A factory capture identifies its donor tablet, not every future device.
    # The shared firmware selects the actual panel from ABL at Linux handoff.
    selected = original if requested == 'auto' else requested
    if selected != original:
        properties['compatible'] = properties['compatible'].replace(
            ('xiaomi,piano-' + original + '-nt36532').encode(),
            ('xiaomi,piano-' + selected + '-nt36532').encode())
        board.write_bytes(write_fdt(parsed))
    return {'requested': requested, 'input_vendor': original, 'selected': selected,
            'panel_node': node, 'selection_source': 'runtime-ABL-selection' if requested == 'auto' else 'explicit-owner-selection',
            'fallback_vendor': original,
            'hardware_tested': False, 'csot_status': 'DRIVER_IMPLEMENTED_DEVICE_TEST_PENDING'}


def record(path):
    data = json.loads(path.read_text()) if path.is_file() else {}
    require(isinstance(data, dict), 'Expected manifest object: ' + str(path))
    return data


def component(path, metadata, explicit, kind):
    require(path.is_file() and not path.is_symlink(), 'Expected regular component: ' + str(path))
    expected = metadata.get('files', {}).get(path.name, {}).get('sha256')
    entry = metadata.get(kind, {})
    expected = expected or (entry.get('sha256') if isinstance(entry, dict) else None) or metadata.get(kind + '_sha256')
    if explicit and expected:
        require(explicit == expected, 'Explicit hash disagrees with manifest: ' + path.name)
    expected = explicit or expected
    require(isinstance(expected, str) and re.fullmatch('[0-9a-f]{64}', expected), 'Missing component hash: ' + path.name)
    require(sha(path) == expected, 'Component hash mismatch: ' + path.name)
    return expected


def scan_root(root, release):
    """Inspect names/stat only for credentials; never open their contents."""
    require(root.is_dir() and not root.is_symlink(), 'Expected a clean rootfs directory')
    rows, links = {}, Counter()
    for path in [root, *sorted(root.rglob('*'))]:
        name = path.relative_to(root).as_posix()
        require(not any(c in name for c in '\n\r'), 'Unsupported rootfs filename')
        info = path.lstat()
        require(stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode), 'Special rootfs file: ' + name)
        if name.split('/')[0] in ('dev', 'proc', 'sys', 'run'):
            require(name in ('dev', 'proc', 'sys', 'run') and stat.S_ISDIR(info.st_mode), 'Runtime tree is not empty: ' + name)
        secret = '.ssh' in path.relative_to(root).parts or name.startswith('etc/ssh/ssh_host_')
        machine = name in ('etc/machine-id', 'var/lib/dbus/machine-id')
        identity = guest_resolve(root, '/' + name).lstat() if machine and stat.S_ISLNK(info.st_mode) else info
        require(not (secret or machine) or (stat.S_ISREG(identity.st_mode) and identity.st_size == 0) or stat.S_ISDIR(identity.st_mode),
                'Nonempty identity/SSH material: ' + name)
        if stat.S_ISLNK(info.st_mode):
            target = os.readlink(path)
            parts = [] if target.startswith('/') else list(path.relative_to(root).parent.parts)
            for part in PurePosixPath(target).parts:
                if part in ('/', '.'):
                    continue
                if part == '..':
                    require(parts, 'Symlink escapes rootfs: ' + name)
                    parts.pop()
                else:
                    parts.append(part)
            # Resolve chains in the guest namespace to catch escapes/loops.
            # Optional software can supply a link target after installation
            # (SteamOS libopenvr_api.so -> /opt/steamvr/...). tar and mke2fs
            # preserve the link itself; absence does not imply a host link.
            guest_resolve(root, '/' + name)
        if stat.S_ISREG(info.st_mode):
            links[(info.st_dev, info.st_ino)] += 1
        rows[name] = info
    for name, info in rows.items():
        if stat.S_ISREG(info.st_mode):
            require(links[(info.st_dev, info.st_ino)] == info.st_nlink, 'Hardlink outside rootfs: ' + name)
    modules = guest_resolve(root, '/lib/modules')
    if not modules.is_dir():
        modules = guest_resolve(root, '/usr/lib/modules')
    require(modules.is_dir() and sorted(p.name for p in modules.iterdir()) == [release], 'Rootfs kernel modules/release mismatch')
    return rows


def pinned_mkbootimg():
    pins = record(ROOT / 'sources.lock.json')
    for name, relative in (('debian-piano-current', 'mkbootimg/mkbootimg.py'), ('Mu-Silicium', 'Resources/Scripts/mkbootimg.py')):
        checkout = ROOT / 'upstream' / name
        script = checkout / relative
        if not script.is_file():
            continue
        pin = pins[name]['commit']
        registered = run(['git', '-C', ROOT, 'ls-files', '--stage', '--', checkout.relative_to(ROOT)]).stdout.split()
        require(len(registered) >= 2 and registered[0] == '160000' and registered[1] == pin, 'mkbootimg source is not the registered pin')
        require(run(['git', '-C', checkout, 'rev-parse', 'HEAD']).stdout.strip() == pin, 'mkbootimg checkout differs from source pin')
        folder = str(Path(relative).parent)
        tracked = run(['git', '-C', checkout, 'ls-tree', '-r', '--name-only', pin, '--', folder]).stdout.splitlines()
        for item in tracked:
            if item.endswith('.py'):
                original = subprocess.check_output(['git', '-C', str(checkout), 'show', pin + ':' + item])
                require((checkout / item).read_bytes() == original, 'Modified mkbootimg Python source: ' + item)
        require(not run(['git', '-C', checkout, 'status', '--porcelain', '--untracked-files=all', '--', folder]).stdout.strip(), 'Dirty mkbootimg source directory')
        return script, {'source': name, 'commit': pin, 'script_sha256': sha(script)}
    raise ValueError('No pinned local mkbootimg source; initialize the registered submodule offline first')


def verify_boot(path, inputs):
    require(path.stat().st_size <= 128 * MIB, 'Linux boot container exceeds the 128 MiB ESP load budget')
    data = path.read_bytes()
    require(len(data) >= 1660 and data[:8] == b'ANDROID!', 'Invalid boot container')
    ksize, _, rsize, _, second, _, _, page, version = struct.unpack_from('<9I', data, 8)
    recovery = struct.unpack_from('<I', data, 1632)[0]
    header, dsize = struct.unpack_from('<II', data, 1644)
    require((page, version, header, second, recovery) == (4096, 2, 1660, 0, 0), 'Expected Android v2/4096 without extra stages')
    require(not any(data[64:576]) and not any(data[608:1632]), 'Unexpected wrapper command line')
    offset = page
    for size, source in zip((ksize, rsize, dsize), inputs):
        require(size == source.stat().st_size and data[offset:offset + size] == source.read_bytes(), 'Boot wrapper component mismatch')
        offset += (size + page - 1) // page * page
    require(len(data) == offset, 'Unexpected boot wrapper trailing data')


def archive_root(root, target, epoch):
    clock = root / CLOCK_EPOCH
    if clock.is_file() and not clock.is_symlink():
        # --mtime is global in GNU tar. Append this one semantic timestamp
        # without that option rather than normalizing systemd's clock floor.
        with tempfile.TemporaryDirectory(dir=target.parent) as temporary:
            archive = Path(temporary) / 'root.tar'
            common = ['tar', '--numeric-owner', '--xattrs', '--acls', '--format=pax',
                      '--pax-option=delete=atime,delete=ctime', '-C', str(root)]
            run([*common, '--sort=name', '--mtime=@' + str(epoch),
                 '--exclude=./' + CLOCK_EPOCH, '-cf', archive, '.'])
            run([*common, '-rf', archive, './' + CLOCK_EPOCH])
            run(['zstd', '-q', '-T0', '-6', '-o', target, archive])
        return
    with tempfile.TemporaryFile() as errors:
        tar = subprocess.Popen(['tar', '--numeric-owner', '--xattrs', '--acls', '--sort=name', '--format=pax',
                                '--pax-option=delete=atime,delete=ctime', '--mtime=@' + str(epoch), '-C', str(root), '-cf', '-', '.'],
                               stdout=subprocess.PIPE, stderr=errors)
        try:
            subprocess.run(['zstd', '-q', '-T0', '-6', '-o', str(target)], stdin=tar.stdout, check=True)
            tar.stdout.close()
            require(tar.wait() == 0, 'tar failed; rootfs may have changed during packing')
        finally:
            if tar.poll() is None:
                tar.terminate()
                tar.wait()


def quote(name):
    path = '/' if name == '.' else '/' + name
    return '"' + path.replace('\\', '\\\\').replace('"', '\\"') + '"'


def xattr_equal(name, left, right):
    if name in ('system.posix_acl_access', 'system.posix_acl_default'):
        # POSIX ACL object/mask entries have no numeric identity. libext2fs
        # emits zero there, while the kernel exposes ACL_UNDEFINED_ID.
        def acl(data):
            require(len(data) >= 4 and (len(data) - 4) % 8 == 0 and data[:4] == b'\2\0\0\0', 'Invalid POSIX ACL format')
            return [(tag, perm, ident if tag in (2, 8) else None) for tag, perm, ident in struct.iter_unpack('<HHI', data[4:])]
        return acl(left) == acl(right)
    return left == right


def inode_mtime(name, info, epoch):
    if name == CLOCK_EPOCH and stat.S_ISREG(info.st_mode):
        timestamp = int(info.st_mtime)
        require(0 <= timestamp <= 0x7fffffff, 'Invalid systemd clock epoch timestamp')
        return timestamp
    return epoch


def ext4(root, target, size, label, epoch, rows, work):
    """No mount: verify every inode's mode/owner/mtime and source xattrs."""
    with target.open('xb') as stream:
        stream.truncate(size * MIB)
    owner = rows['.']
    run(['mke2fs', '-q', '-t', 'ext4', '-F', '-L', label, '-E', f'root_owner={owner.st_uid}:{owner.st_gid}', '-d', root, target], env={**os.environ, 'E2FSPROGS_FAKE_TIME': str(epoch)})
    # debugfs does not reliably unescape embedded quotes in filenames. Use
    # inode references for these paths; directory listing names stay literal.
    inodes = {'.': 2}
    children = {}
    def inode_reference(name):
        parent = '.'
        for part in PurePosixPath(name).parts:
            child = part if parent == '.' else parent + '/' + part
            if child not in inodes:
                if parent not in children:
                    listing = run(['debugfs', '-R', f'ls -p <{inodes[parent]}>', target]).stdout
                    entries = {}
                    for line in listing.splitlines():
                        fields = line.split('/')
                        if len(fields) >= 8 and fields[1].isdigit():
                            entries[fields[5]] = int(fields[1])
                    children[parent] = entries
                require(part in children[parent], 'ext4 directory entry missing: ' + child)
                inodes[child] = children[parent][part]
            parent = child
        return f'<{inodes[parent]}>'
    references = {n: inode_reference(n) if any(c in n for c in '"\\') else quote(n) for n in rows}
    script = work / 'inode-checks'
    script.write_text(f'set_inode_field "/" mode 0{owner.st_mode:o}\n' + ''.join(f'set_inode_field {references[n]} mtime @{inode_mtime(n, info, epoch)}\nset_inode_field {references[n]} ctime @{epoch}\nstat {references[n]}\n' for n, info in rows.items()))
    output = run(['debugfs', '-w', '-f', script, target]).stdout
    sections = re.split(r'debugfs:\s+stat ', output)[1:]
    require(len(sections) == len(rows), 'ext4 inode checks incomplete')
    for (name, info), text in zip(rows.items(), sections):
        mode = re.search(r'Mode:\s+([0-7]+)', text)
        owner = re.search(r'User:\s*(\d+)\s+Group:\s*(\d+)', text)
        mtime = re.search(r'mtime:\s+0x([0-9a-f]+)', text)
        ctime = re.search(r'ctime:\s+0x([0-9a-f]+)', text)
        require(mode and owner and mtime and int(mode[1], 8) == stat.S_IMODE(info.st_mode)
                and ctime and tuple(map(int, owner.groups())) == (info.st_uid, info.st_gid)
                and int(mtime[1], 16) == inode_mtime(name, info, epoch)
                and int(ctime[1], 16) == epoch,
                'ext4 metadata mismatch: ' + name)
        for attr in os.listxattr(root / name, follow_symlinks=False):
            require(re.fullmatch('[A-Za-z0-9_.-]+', attr), 'Unsupported xattr name')
            saved = work / 'xattr-value'
            saved.unlink(missing_ok=True)
            run(['debugfs', '-R', f'ea_get -f "{saved}" {references[name]} {attr}', target])
            require(saved.is_file() and xattr_equal(attr, saved.read_bytes(), os.getxattr(root / name, attr, follow_symlinks=False)), 'ext4 xattr/ACL mismatch: ' + name)


def package(args):
    uefi, kernel, dtb, initrd, root = [Path(getattr(args, n)).absolute() for n in ('uefi', 'kernel', 'dtb', 'initramfs', 'rootfs')]
    require(not root.is_symlink(), 'Rootfs must not be a host symlink')
    require(not args.output.is_symlink(), 'Output must not be a host symlink')
    root, output = root.resolve(), args.output.resolve()
    require(not output.exists() and not output.is_relative_to(root), 'Output must be fresh and outside rootfs')
    require(64 <= args.esp_size_mib <= 2048 and (args.root_size_mib is None or 64 <= args.root_size_mib <= 65536), 'Invalid image capacity')
    require(0 <= args.epoch <= 0x7fffffff, 'Invalid SOURCE_DATE_EPOCH')
    # Qt treats an exact zero file timestamp as invalid. SDDM then skips all
    # configuration files on a fresh image. Keep deterministic, positive times.
    args.epoch = max(1, args.epoch)
    require(re.fullmatch('LABEL=[A-Za-z0-9_-]{1,16}', args.root_selector), 'Portable bundle requires a filesystem LABEL root selector')
    metas = [record(uefi.parent / 'manifest.json'), record(kernel / 'manifest.json'), record(dtb.parent / 'manifest.json'), record(initrd.parent / 'manifest.json')]
    image = kernel / 'Image'
    hashes = [component(p, m, getattr(args, key + '_sha256'), kind) for p, m, key, kind in zip((uefi, image, dtb, initrd), metas,
              ('uefi', 'kernel', 'dtb', 'initramfs'), ('uefi', 'image', 'output', 'initramfs'))]
    require(metas[1].get('source_dirty') is not True and metas[1].get('source_clean') is not False
            and not any(word in metas[1].get('status', '').upper() for word in ('FAILED', 'INCOMPLETE')), 'Kernel manifest is not a completed clean build')
    release = args.kernel_release or metas[1].get('kernel_release')
    require(isinstance(release, str) and re.fullmatch('[A-Za-z0-9._+-]{1,128}', release), 'Missing/invalid kernel release')
    for meta in metas[1:]:
        require(not meta.get('kernel_release') or meta['kernel_release'] == release, 'Component kernel release mismatch')
    require(args.rootfs_manifest is None or args.rootfs_manifest.is_file(), 'Rootfs manifest does not exist')
    root_meta = record(args.rootfs_manifest or root.parent / 'manifest.json')
    require(not root_meta.get('kernel_release') or root_meta['kernel_release'] == release, 'Rootfs manifest release mismatch')
    for meta in [*metas[1:], root_meta]:
        policy = meta.get('root_policy') or meta.get('root_selector')
        require(not policy or policy == args.root_selector, 'Component root selector mismatch')
    for meta in (metas[3], root_meta):
        if meta.get('kernel_manifest_sha256'):
            require(sha(kernel / 'manifest.json') == meta['kernel_manifest_sha256'], 'Component refers to another kernel manifest')
        if meta.get('kernel_commit'):
            require(meta['kernel_commit'] == metas[1].get('source_commit'), 'Rootfs kernel commit mismatch')
    command_line = metas[1].get('command_line', '').split()
    roots = [v.split('=', 1)[1] for v in command_line if v.startswith(('root=', 'piano.root='))]
    require(not roots or roots == [args.root_selector], 'Kernel command line selects another root')
    if metas[1].get('config_sha256'):
        require(sha(kernel / 'config') == metas[1]['config_sha256'], 'Kernel config hash mismatch')
    require(uefi.read_bytes()[:8] == b'ANDROID!', 'UEFI must be an Android boot container')
    image_info(image)
    fdt_info(dtb)
    initramfs_info(initrd)
    with gzip.open(initrd, 'rb') as stream:
        ram = stream.read(128 * MIB + 1)
    require(len(ram) <= 128 * MIB, 'Initramfs exceeds the bounded bootstrap budget')
    entries = inspect_newc(ram)
    embedded = {r['name']: r['data'] for r in entries}
    require('init' in embedded or 'pianoinit' in embedded, 'Initramfs has no init')
    if 'etc/piano/kernel-release' in embedded:
        require(embedded['etc/piano/kernel-release'].decode().strip() == release, 'Initramfs embedded release mismatch')
    require(all(n.split('/')[2] == release for n in embedded if n.startswith('lib/modules/') and len(n.split('/')) > 2), 'Initramfs module release mismatch')
    rows = scan_root(root, release)
    systemd_boot = (Path(args.systemd_boot).absolute() if getattr(args, 'systemd_boot', None) else
                    guest_resolve(root, '/usr/lib/systemd/boot/efi/systemd-bootaa64.efi'))
    linux_loader = (Path(args.linux_loader).absolute() if getattr(args, 'linux_loader', None) else
                    uefi.parent / 'PianoLinuxBoot.efi')
    efi_sources = {}
    for name, source in (('systemd_boot', systemd_boot), ('linux_loader', linux_loader)):
        require(source.is_file() and not source.is_symlink(), 'Expected regular EFI application: ' + str(source))
        info = image_info(source, kernel=False)
        if name == 'linux_loader':
            component(source, metas[0], info['sha256'], name)
        efi_sources[name] = {'source': str(source), 'bytes': info['bytes'], 'sha256': info['sha256']}
    esp_only = getattr(args, 'esp_only', False)
    build_root = bool(args.root_size_mib) and not esp_only
    build_archive = not esp_only and (not args.root_size_mib or args.root_archive)
    tools = ['mkfs.vfat', 'mmd', 'mcopy']
    if build_archive:
        tools += ['tar', 'zstd']
    if build_root:
        tools += ['mke2fs', 'debugfs']
    require(all(shutil.which(t) for t in tools), 'Missing host tools: ' + ', '.join(t for t in tools if not shutil.which(t)))
    maker, maker_source = pinned_mkbootimg()
    output.mkdir(parents=True)
    incomplete = output / '.incomplete'
    incomplete.write_text('Packaging has not completed.\n')
    work = output / '.work'
    work.mkdir()
    sources = (uefi, image, dtb, initrd)
    names = ('PianoUEFI-product.img', 'Image', 'board.dtb', 'initramfs')
    for source, name, expected in zip(sources, names, hashes):
        shutil.copyfile(source, work / name)
        require(sha(work / name) == expected, 'Source changed while copying: ' + name)
    for name, source, filename in (('systemd_boot', systemd_boot, 'BOOTAA64.EFI'),
                                   ('linux_loader', linux_loader, 'LinuxBoot.efi')):
        shutil.copyfile(source, work / filename)
        require(sha(work / filename) == efi_sources[name]['sha256'], 'Source changed while copying: ' + filename)
    (work / 'loader.conf').write_text('default debian.conf\ntimeout 0\neditor no\nauto-entries no\nauto-firmware no\nconsole-mode keep\n')
    (work / 'debian.conf').write_text('title Debian\nefi /EFI/Piano/LinuxBoot.efi\n')
    dtb_derivation = None
    if args.cpu_model_overlay:
        require(shutil.which('dtc') and shutil.which('fdtoverlay'), 'CPU model overlay needs dtc and fdtoverlay')
        dtb_derivation = apply_cpu_model_overlay(work / 'board.dtb', args.cpu_model_overlay, work)
    extra_dtb_overlays = []
    for overlay in args.dtb_overlay:
        extra_dtb_overlays.append(apply_cpu_model_overlay(work / 'board.dtb', overlay, work, cpu_only=False))
    mainline_bindings = normalize_mainline_bindings(work / 'board.dtb')
    panel_selection = None
    if args.panel_vendor:
        panel_selection = select_panel_vendor(work / 'board.dtb', args.panel_vendor)
    boot = output / 'boot.img'
    run([sys.executable, maker, '--header_version', '2', '--pagesize', '4096', '--kernel', work / 'Image', '--ramdisk', work / 'initramfs', '--dtb', work / 'board.dtb', '-o', boot],
        env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
    verify_boot(boot, (work / 'Image', work / 'initramfs', work / 'board.dtb'))
    esp_files = { '/EFI/Piano/stable/' + path.name: path
                  for path in (work / 'Image', work / 'board.dtb', work / 'initramfs', boot) }
    esp_files.update({'/EFI/BOOT/BOOTAA64.EFI': work / 'BOOTAA64.EFI',
                      '/EFI/Piano/LinuxBoot.efi': work / 'LinuxBoot.efi',
                      '/loader/loader.conf': work / 'loader.conf',
                      '/loader/entries/debian.conf': work / 'debian.conf'})
    for path in esp_files.values():
        os.utime(path, (max(args.epoch, 315532800),) * 2)  # FAT dates start in 1980.
    esp = output / 'esp.img'
    require(sum(path.stat().st_size for path in esp_files.values()) + 2 * MIB < args.esp_size_mib * MIB, 'Payload exceeds ESP capacity')
    with esp.open('xb') as stream:
        stream.truncate(args.esp_size_mib * MIB)
    # Piano UFS exposes 4 KiB logical sectors; Linux rejects a smaller BPB
    # sector size even when firmware can read the filesystem.
    run(['mkfs.vfat', '--invariant', '-S', '4096', '-F', '32', '-n', 'SUNUEFI_ESP', esp])
    run(['mmd', '-i', esp, '::/EFI', '::/EFI/BOOT', '::/EFI/Piano', '::/EFI/Piano/stable',
         '::/loader', '::/loader/entries'])
    for destination, path in esp_files.items():
        run(['mcopy', '-m', '-i', esp, path, '::' + destination])
        verify = work / ('readback-' + path.name)
        run(['mcopy', '-i', esp, '::' + destination, verify])
        require(sha(verify) == sha(path), 'ESP file readback differs: ' + path.name)
    if build_archive:
        archive_root(root, output / 'root.tar.zst', args.epoch)
    if build_root:
        ext4(root, output / 'root.ext4.img', args.root_size_mib, args.root_selector.split('=', 1)[1], args.epoch, rows, work)
    require({n: (s.st_mode, s.st_uid, s.st_gid, s.st_size, s.st_mtime_ns, s.st_ctime_ns) for n, s in rows.items()} ==
            {n: (s.st_mode, s.st_uid, s.st_gid, s.st_size, s.st_mtime_ns, s.st_ctime_ns) for n, s in scan_root(root, release).items()}, 'Rootfs changed while packaging')
    shutil.move(work / names[0], output / names[0])
    result = {'schema_version': 1, 'status': 'HOST_BUILT_NOT_DEVICE_READY', 'kernel_release': release, 'root_policy': args.root_selector,
              'epoch': args.epoch, 'mkbootimg': maker_source, 'device_operation_performed': False, 'device_ready': False,
              'components': {**{name: {'sha256': value} for name, value in zip(('uefi', 'kernel', 'dtb', 'initramfs'), hashes)}, **efi_sources},
              'kernel_manifest_sha256': sha(kernel / 'manifest.json') if (kernel / 'manifest.json').is_file() else None,
              'partitions': {'esp': {'partlabel': 'sunuefi_esp', 'label': 'SUNUEFI_ESP', 'sector_bytes': 4096, 'capacity_bytes': esp.stat().st_size},
                             'root': {'partlabel': 'sunuefi_root', 'label': args.root_selector.split('=', 1)[1], 'capacity_bytes': args.root_size_mib * MIB if build_root else None}},
              'esp_files': list(esp_files),
              'metadata': {'archive': 'numeric uid/gid, modes, symlinks, hardlinks, xattrs and ACLs; mtime normalized except /usr/lib/clock-epoch' if build_archive else None,
                           'ext4': 'all inode uid/gid/mode/mtime and source xattrs/ACLs checked; ctime normalized; sparse source allocation not preserved' if build_root else None},
              'files': {}}
    if CLOCK_EPOCH in rows and stat.S_ISREG(rows[CLOCK_EPOCH].st_mode):
        result['clock_epoch'] = {'path': '/' + CLOCK_EPOCH,
                                 'mtime': inode_mtime(CLOCK_EPOCH, rows[CLOCK_EPOCH], args.epoch)}
    if dtb_derivation: result['dtb_derivation'] = dtb_derivation
    result['mainline_bindings'] = mainline_bindings
    if extra_dtb_overlays: result['additional_dtb_overlays'] = extra_dtb_overlays
    if panel_selection: result['panel_selection'] = panel_selection
    shutil.rmtree(work)
    for path in sorted(output.iterdir()):
        if path.name == '.incomplete':
            continue
        result['files'][path.name] = {'bytes': path.stat().st_size, 'sha256': sha(path), 'format': 'raw-fat32' if path == esp else 'raw-ext4' if path.name == 'root.ext4.img' else 'tar-zstd' if path.name == 'root.tar.zst' else 'android-boot', 'expanded_bytes': path.stat().st_size}
    manifest = output / 'manifest.json'
    manifest.write_text(json.dumps(result, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.name}\n' for p in sorted(output.iterdir()) if p.name not in ('.incomplete', 'SHA256SUMS')))
    incomplete.unlink()
    return result


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cpu-model-overlay', type=Path)
    ap.add_argument('--dtb-overlay', type=Path, action='append', default=[])
    ap.add_argument('--panel-vendor', choices=('auto', 'boe', 'csot'))
    for name in ('uefi', 'kernel', 'dtb', 'initramfs', 'rootfs', 'output'):
        ap.add_argument('--' + name, type=Path, required=True)
    for name in ('uefi', 'kernel', 'dtb', 'initramfs'):
        ap.add_argument('--' + name + '-sha256')
    ap.add_argument('--kernel-release')
    ap.add_argument('--rootfs-manifest', type=Path)
    ap.add_argument('--systemd-boot', type=Path,
                    help='ARM64 systemd-boot EFI application (default: from rootfs)')
    ap.add_argument('--linux-loader', type=Path,
                    help='PianoLinuxBoot EFI application (default: beside UEFI image)')
    ap.add_argument('--root-selector', default='LABEL=PIANOROOT')
    ap.add_argument('--esp-size-mib', type=int, default=512)
    ap.add_argument('--esp-only', action='store_true', help='Export the startup images without rebuilding a root image or archive')
    ap.add_argument('--root-size-mib', type=int)
    ap.add_argument('--root-archive', action='store_true',
                    help='Also export root.tar.zst when building a root.ext4.img')
    epoch = os.environ.get('SOURCE_DATE_EPOCH')
    if epoch is None:
        epoch = subprocess.check_output(
            ['git', '-C', ROOT, 'show', '-s', '--format=%ct', 'HEAD'], text=True).strip()
    ap.add_argument('--epoch', type=int, default=int(epoch))
    return ap


if __name__ == '__main__':
    try:
        manifest = package(parser().parse_args())
        print(json.dumps({'status': manifest['status'], 'files': manifest['files']}, indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
