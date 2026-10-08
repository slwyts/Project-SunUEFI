#!/usr/bin/env python3
"""Prepare same-core Android module inputs without a stock ROM or device access."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import build_android_module as module
from build_boot_repack import build as build_native
from package_product_trampoline import build_selector

ROOT = Path(__file__).resolve().parents[1]


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def reused_native(tool, manifest, output):
    """Reuse a real build only when its executable and recorded source bytes match."""
    data = module.read_file(tool, 32 * 1024 * 1024)
    proof = module.read_json(manifest)
    module.arm64_executable(data)
    module.checked(data, proof.get('executable'), 'Native executable')
    module.require(proof.get('arch') == 'aarch64' and proof.get('interface_version') == 1,
                   'Expected the implemented AArch64 module interface')
    sources = proof.get('sources', {})
    module.require(sources and 'android/native/piano-boot-repack.c' in sources,
                   'Native source identity is missing')
    for name, expected in sources.items():
        module.require(module.digest(module.read_file(module.child(ROOT, name))) == expected,
                       'Native input source changed: ' + name)
    output.mkdir()
    binary = output / 'piano-boot-repack'
    binary.write_bytes(data)
    binary.chmod(0o755)
    return binary, proof


def prepare(args):
    output = args.output.resolve()
    module.require(not output.exists() and not output.is_symlink(), 'Use a new module preparation directory')
    payloads, product = module.product_payload(args.product)
    output.mkdir(parents=True)
    marker = output / '.incomplete'
    marker.write_text('Android module inputs have not completed.\n')
    selector_work = output / 'selector-work'
    selector_work.mkdir()
    selector, labels, source_files = build_selector(selector_work)
    selector_path = output / 'selector.bin'
    selector_path.write_bytes(selector)
    selector_meta = {
        'schema_version': 1, 'interface_version': 1,
        'selector_sha256': module.digest(selector), 'selector_bytes': len(selector),
        'selector_memory_bytes': labels['__image_end'],
        'metadata_offset': labels['bootselect_metadata'],
        'product_fd_sha256': module.digest(payloads['fd.bin']),
        'app_payload_sha256': module.digest(payloads['app.bin']),
        'supported_boot_headers': [4], 'app_abi': 1, 'wrapper_version': 1,
        'entry_policy': 'explicit-request-only', 'request_bootarg': 'sunuefi.boot=uefi',
        'persistent_uefi_request': False, 'default_target': 'android',
        'recovery_policy': 'stock-passthrough-before-request',
        'source_files': source_files, 'stock_kernel_bundled': False,
        'device_passthrough_verified': False,
    }
    selector_manifest = output / 'selector-module.json'
    save_json(selector_manifest, selector_meta)
    native_dir = output / 'native'
    if args.native_tool or args.native_manifest:
        module.require(args.native_tool and args.native_manifest,
                       'Supply both --native-tool and --native-manifest')
        binary, native = reused_native(args.native_tool, args.native_manifest, native_dir)
    else:
        binary, native = build_native(native_dir, 'aarch64', args.cc, args.sysroot)
    # A fresh product/selector is a new proof scope. Never carry earlier device
    # claims or roundtrip results onto these hashes merely by copying a manifest.
    native = {**native, 'payload_sha256': {
        name: module.digest(data) for name, data in {**payloads, 'selector.bin': selector}.items()},
        'supported_boot_headers': [4], 'tested_boot_headers': [],
        'tests': dict.fromkeys(module.TESTS, False),
        'device_passthrough_verified': False, 'request_handling_verified': False,
        'standard_recovery_preserved': False, 'device_execution_ready': False,
        'stock_kernel_bundled': False, 'full_partition_backup': False,
        'request_policy': 'persistent-until-changed'}
    save_json(native_dir / 'manifest.json', native)
    proof_path = args.qualification or native_dir / 'manifest.json'
    options = argparse.Namespace(
        product=args.product, selector=selector_path, selector_manifest=selector_manifest,
        native_tool=binary, native_manifest=proof_path, inspect=False,
        output=None, candidate=False)
    report, _, _ = module.inspect(options)
    report.update({'input_preparation_complete': True, 'default_target': 'android',
                   'stock_boot_required_on_build_host': False,
                   'paths': {'selector': str(selector_path), 'selector_manifest': str(selector_manifest),
                             'native_tool': str(binary), 'native_manifest': str(proof_path)},
                   'product': product})
    save_json(output / 'readiness.json', report)
    marker.unlink()
    if args.package != 'none':
        options.candidate = args.package == 'candidate'
        name = 'SunUEFI-KernelSU-candidate.zip' if options.candidate else 'SunUEFI-KernelSU.zip'
        options.output = output / name
        packaged = module.package(options)
        save_json(output / 'package.json', packaged)
        report['package'] = packaged
    save_json(output / 'inputs.json', {
        'product_sha256': product['product_sha256'], 'selector': selector_meta,
        'native_executable': module.record(binary.read_bytes()),
        'package_kind': args.package, 'qualification': str(proof_path),
        'device_operation_performed': False, 'stock_kernel_bundled': False})
    return report


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--product', type=Path, default=ROOT / 'artifacts/product')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cc', help='AArch64 static C compiler, e.g. aarch64-linux-gnu-gcc')
    p.add_argument('--sysroot', type=Path)
    p.add_argument('--native-tool', type=Path, help='Reuse an unchanged real AArch64 build')
    p.add_argument('--native-manifest', type=Path)
    p.add_argument('--qualification', type=Path,
                   help='Actual same-payload native/device proof; absence keeps production packaging blocked')
    p.add_argument('--package', choices=('none', 'candidate', 'ready'), default='none')
    return p


if __name__ == '__main__':
    try:
        print(json.dumps(prepare(parser().parse_args()), indent=2))
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        print('android-module-inputs: ' + str(error), file=sys.stderr)
        sys.exit(2)
