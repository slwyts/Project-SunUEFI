#!/usr/bin/env python3
"""Host-only combined stock-GKI/product entry. Never flash or access a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
BOOT_BYTES = 96 * 1024**2
PAGE = 4096


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def align(size, unit=PAGE):
    return (size + unit - 1) // unit * unit


def stock_kernel(data):
    require(len(data) >= PAGE and data[:8] == b'ANDROID!', 'Not an Android boot image')
    size, ramdisk, version = struct.unpack_from('<I', data, 8)[0], struct.unpack_from('<I', data, 12)[0], struct.unpack_from('<I', data, 40)[0]
    require(version == 4 and struct.unpack_from('<I', data, 20)[0] == 1584 and
            not ramdisk and not struct.unpack_from('<I', data, 1580)[0], 'Expected stock BOOT4 with init_boot and no GKI signature section')
    require(4096 <= size <= len(data) - PAGE, 'Kernel extent exceeds input')
    kernel = data[PAGE:PAGE + size]
    require(kernel[:2] == b'MZ' and kernel[56:60] == b'ARMd', 'Expected raw AArch64 GKI, not a compressed or already wrapped input')
    code0, code1, offset, span, flags = struct.unpack_from('<IIQQQ', kernel)
    require(offset % (2 * 1024**2) == 0 and size <= span < BOOT_BYTES and not flags & 1,
            'Unsupported GKI placement/span/endianness')
    displacement = code1 & 0x3ffffff
    if displacement & 0x2000000:
        displacement -= 0x4000000
    require(code1 >> 26 == 5 and 64 <= 4 + displacement * 4 < len(kernel), 'Unsupported original GKI entry branch')
    require(b'SUNUEFI-SPLITv1\0' not in kernel, 'Input is already wrapped; restore/extract its original kernel first')
    return kernel, {'kernel_bytes': size, 'image_size': span, 'text_offset': offset,
                    'flags': flags, 'code0': code0, 'code1': code1,
                    'kernel_sha256': digest(kernel)}


def build_selector(output):
    toolroot = Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT', ROOT / 'build/host-tools/usr'))
    cc, ld, objcopy, nm = (toolroot / 'bin' / name for name in ('clang', 'ld.lld', 'llvm-objcopy', 'llvm-nm'))
    environment = {**os.environ, 'LD_LIBRARY_PATH': str(toolroot / 'lib') +
                   (':' + os.environ['LD_LIBRARY_PATH'] if os.environ.get('LD_LIBRARY_PATH') else '')}
    inc = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
    sources = [ROOT / 'uefi/handoff/bootselect' / name for name in ('Entry.S', 'BootSelect.c', 'BootRequest.c', 'EarlyTrace.c', 'EarlySplash.c')]
    sources.append(ROOT / 'uefi/components/product-support/Library/ProductBootManagerLib/ProductSplash.c')
    trace_id=digest(b''.join(path.read_bytes() for path in sources))[:16]
    objects = []
    flags = ['--target=aarch64-linux-gnu', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
             '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-mgeneral-regs-only', '-mstrict-align', '-fshort-wchar',
             '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-DPIANO_EARLY_SPLASH',
             '-DPIANO_BOOTSELECT_TRACE_ID="'+trace_id+'"',
             '-I' + str(inc), '-I' + str(inc / 'AArch64')]
    for index, source in enumerate(sources):
        obj = output / f'entry-{index}.o'
        subprocess.run([str(cc), *flags, '-c', str(source), '-o', str(obj)], check=True, env=environment)
        objects.append(obj)
    elf = output / 'selector.elf'
    subprocess.run([str(ld), '-T', str(ROOT / 'uefi/handoff/bootselect/BootSelect.ld'),
                    *map(str, objects), '-o', str(elf)], check=True, env=environment)
    binary = output / 'selector.bin'
    subprocess.run([str(objcopy), '-O', 'binary', str(elf), str(binary)], check=True, env=environment)
    text = subprocess.check_output([str(nm), '-n', str(elf)], text=True, env=environment)
    labels = {row.split()[2]: int(row.split()[0], 16) for row in text.splitlines() if len(row.split()) == 3}
    require(labels.get('_start') == 0 and labels['__image_end'] == labels['__stack_end'] and
            8192 < labels['__image_end'] < 65536 and labels['bootselect_metadata'] + 128 <= binary.stat().st_size,
            'Selector entry/metadata/private-stack layout changed')
    related = [*sources, ROOT / 'uefi/handoff/bootselect/BootSelect.h', ROOT / 'uefi/handoff/bootselect/BootRequest.h', ROOT / 'uefi/handoff/bootselect/EarlyTrace.h', ROOT / 'uefi/handoff/bootselect/BootSelect.ld',
               ROOT / 'uefi/components/product-support/Library/ProductBootManagerLib/ProductSplashAssets.h']
    return binary.read_bytes(), labels, {str(p.relative_to(ROOT)): digest(p.read_bytes()) for p in related}


def combine(original, selector, symbols, shim, fd, app):
    require(len(fd) == 0x300000 and len(shim) >= 64 and shim[56:60] == b'ARMd', 'Unexpected product FD/BootShim')
    require(app[:16] == b'SUNUEFI-APPv1\0'.ljust(16, b'\0') and len(app) >= 64, 'Missing APPv1 payload')
    version, header, count = struct.unpack_from('<IIQ', app, 16)
    require((version, header, count) == (1, 64, len(app) - 64) and digest(app[64:]) == app[32:64].hex(), 'APPv1 digest/size differs')
    span = struct.unpack_from('<Q', original, 16)[0]
    request_offset = align(span)
    selector_offset = request_offset + 2*PAGE
    shim_offset = align(selector_offset + symbols['__image_end'], 16)
    app_offset = align(shim_offset + len(shim) + len(fd), 16)
    total = align(app_offset + len(app))
    require(total + PAGE + 69632 < BOOT_BYTES and total < 0x08000000, 'Combined payload exceeds supported BOOT capacity/branch range')
    delta = selector_offset - 4
    require(delta % 4 == 0 and 0 < delta < 0x08000000, 'Selector cannot be reached by the ARM64 entry branch')
    code0, code1 = struct.unpack_from('<II', original)
    meta = struct.pack('<16sIIQQII6Q32s', b'SUNUEFI-SPLITv1\0', 1, 128,
                       selector_offset, span, code0, code1, shim_offset, len(shim), len(fd),
                       app_offset, len(app), total, hashlib.sha256(app).digest())
    require(len(meta) == 128, 'Selector metadata ABI changed')
    result = bytearray(total)
    result[:len(original)] = original
    empty = bytearray(struct.pack('<16sIIQII16s8s', b'SUNUEFI-NEXTv1\0', 1, 64, 0, 0, 0,
                                  hashlib.sha256(app).digest()[:16], bytes(8)))
    struct.pack_into('<I', empty, 36, zlib.crc32(empty))
    for page in range(2):
        result[request_offset+page*PAGE:request_offset+page*PAGE+64] = empty
    result[selector_offset:selector_offset + len(selector)] = selector
    offset = selector_offset + symbols['bootselect_metadata']
    result[offset:offset + 128] = meta
    result[shim_offset:shim_offset + len(shim)] = shim
    result[shim_offset + len(shim):shim_offset + len(shim) + len(fd)] = fd
    result[app_offset:app_offset + len(app)] = app
    struct.pack_into('<I', result, 4, 0x14000000 | (delta // 4))
    struct.pack_into('<Q', result, 16, total)
    # Byte-for-byte restoration of the original stock kernel is a required
    # host check, not a claim that physical Android handoff has been verified.
    restored = bytearray(result[:len(original)])
    struct.pack_into('<IIQ', restored, 0, code0, code1, struct.unpack_from('<Q', original, 8)[0])
    struct.pack_into('<Q', restored, 16, span)
    require(restored == original, 'Normal-path header restoration changes original kernel bytes')
    return bytes(result), {'selector_offset': selector_offset, 'selector_memory_bytes': symbols['__image_end'],
                           'request_pages_offset': request_offset, 'request_pages_bytes': 2*PAGE,
                           'request_record_bytes': 64, 'request_writer_verified': False,
                           'metadata_offset': offset, 'bootshim_offset': shim_offset,
                           'fd_offset': shim_offset + len(shim), 'app_offset': app_offset,
                           'app_bytes': len(app), 'container_bytes': total,
                           'original_kernel_reconstructable_byte_equal': True,
                           'normal_header_writes': False, 'normal_splash_enabled': False}


def package(stock, product, output, uefi_request=False):
    stock, product, output = Path(stock).resolve(), Path(product).resolve(), Path(output).resolve()
    require(not output.exists(), 'Output must be a fresh directory')
    source = stock.read_bytes()
    kernel, original = stock_kernel(source)
    manifest = json.loads((product / 'manifest.json').read_text())
    require(manifest.get('target') == 'product', 'Expected a product build')
    sys.path.insert(0, str(ROOT / 'tools'))
    from build_integrity import validate
    validate(ROOT, 'product')
    components = {}
    for name in ('PianoUEFI-product.fd', 'BootShim.bin'):
        data = (product / name).read_bytes()
        require(manifest['files'][name] == {'bytes': len(data), 'sha256': digest(data)}, 'Product component changed: ' + name)
        components[name] = data
    app = (ROOT / 'artifacts/simpleinit/product/app-payload.bin').read_bytes()
    output.mkdir(parents=True)
    (output / '.incomplete').write_text('Host packaging has not completed.\n')
    work = output / '.work';work.mkdir()
    selector, symbols, inputs = build_selector(work)
    combined, layout = combine(kernel, selector, symbols, components['BootShim.bin'], components['PianoUEFI-product.fd'], app)
    header = bytearray(source[:PAGE]);struct.pack_into('<I', header, 8, len(combined))
    if uefi_request:
        existing = bytes(header[44:1580]).split(b'\0', 1)[0]
        require(b'sunuefi.boot=' not in existing, 'Stock input already has a SunUEFI request')
        command_line = existing + (b' ' if existing else b'') + b'sunuefi.boot=uefi'
        require(len(command_line) < 1536, 'Request exceeds BOOT cmdline capacity')
        header[44:1580] = command_line.ljust(1536, b'\0')
    image = output / 'PianoUEFI-product.img'
    image.write_bytes(header + combined)
    # Generate valid metadata, retaining the stock OS properties and rollback
    # index. This does not alter the device's vbmeta or provide an OEM signature.
    from package_piano_recovery_avb import AVBTOOL, AVBTOOL_SHA256
    require(digest(AVBTOOL.read_bytes()) == AVBTOOL_SHA256, 'AVB tool source differs')
    command = [sys.executable, str(AVBTOOL)]
    info = subprocess.check_output(command + ['info_image', '--image', str(stock)], text=True)
    rollback = int(re.search(r'^Rollback Index:\s+(\d+)', info, re.M).group(1))
    props = re.findall(r"^    Prop: ([^ ]+) -> '([^']*)'$", info, re.M)
    require(any(k == 'com.android.build.boot.fingerprint' and v.startswith('Xiaomi/piano/') for k, v in props), 'Stock input is not the supported Piano ROM family')
    args = command + ['add_hash_footer', '--image', str(image), '--dynamic_partition_size', '--partition_name', 'boot',
                      '--algorithm', 'NONE', '--flags', '0', '--rollback_index', str(rollback), '--salt', '']
    for key, value in props:
        args += ['--prop', key + ':' + value]
    subprocess.run(args, check=True)
    # Hash-descriptor verification resolves the partition name to boot.img.
    alias = work / 'boot.img';os.link(image, alias)
    subprocess.run(command + ['verify_image', '--image', str(alias)], check=True, stdout=subprocess.DEVNULL)
    # ABL reads AVB at the actual partition end. Keep an explicit complete
    # install container so a prefix-only flash cannot leave a stale footer.
    small = image.read_bytes()
    require(len(small) < BOOT_BYTES and small[-64:-60] == b'AVBf', 'Missing bounded BOOT AVB footer')
    with (output / 'install-container.bin').open('xb') as stream:
        stream.write(small);stream.seek(BOOT_BYTES-64);stream.write(small[-64:])
    for name, data in {**components, 'app-payload.bin': app, 'selector.bin': selector}.items():
        (output / name).write_bytes(data)
    selector_meta = {'schema_version': 1, 'interface_version': 1, 'supported_boot_headers': [4],
                     'strict_alignment': True, 'early_trace_enabled': False,
                     'embedded_handoff_version': 2,
                     'request_targets': {'1': 'menu', '2': 'linux-stable', '3': 'setup'},
                     'trace_id': digest(b''.join((ROOT / path).read_bytes() for path in inputs if path.endswith(('.S','.c'))))[:16],
                     'selector_sha256': digest(selector), 'selector_bytes': len(selector),
                     'app_abi': 1, 'wrapper_version': 1, 'selector_memory_bytes': symbols['__image_end'],
                     'metadata_offset': symbols['bootselect_metadata'], 'product_fd_sha256': digest(components['PianoUEFI-product.fd']),
                     'app_payload_sha256': digest(app), 'source_files': inputs,
                     'entry_policy': 'explicit-request-only', 'request_bootarg': 'sunuefi.boot=uefi',
                     'persistent_uefi_request': uefi_request,
                     'device_passthrough_verified': False, 'sticky_route_enabled': False}
    (output / 'selector.json').write_text(json.dumps(selector_meta, indent=2) + '\n')
    result = {**manifest, 'status': 'COMBINED_ENTRY_HOST_BUILT_NOT_DEVICE_VERIFIED',
              'entry_kind': 'stock-gki-early-selector', 'android_header_version': 4,
              'stock_input': {'sha256': digest(source), **original}, 'layout': layout,
              'normal_path': 'direct stock primary_entry before EDK2; DTB/initrd unchanged; no header writes or splash',
              'recovery_path': 'original Mi Recovery unless an explicit SunUEFI request is present',
              'uefi_entry': 'unique sunuefi.boot=uefi request; never inferred from the stock recovery mode',
              'diagnostic_persistent_request': uefi_request, 'one_shot_request_verified': False,
              'independent_recovery_install_supported': False, 'boot_partition_installed': False,
              'device_boot_performed': False, 'device_passthrough_verified': False, 'files': {}}
    shutil.rmtree(work)
    for path in output.iterdir():
        if path.is_file() and path.name != '.incomplete':
            result['files'][path.name] = {'bytes': path.stat().st_size, 'sha256': digest(path.read_bytes())}
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    (output / '.incomplete').unlink()
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-boot', type=Path, required=True)
    parser.add_argument('--product', type=Path, default=ROOT / 'artifacts/product')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--uefi-request', action='store_true', help='Host diagnostic only: persistent cmdline request, NOT a verified one-shot installation')
    args = parser.parse_args()
    try:
        result = package(args.stock_boot, args.product, args.output, args.uefi_request)
        print(json.dumps({'status': result['status'], 'layout': result['layout'], 'files': result['files']}, indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
