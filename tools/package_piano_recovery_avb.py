#!/usr/bin/env python3
"""Wrap the ONE manifest-pinned product in a recovery-only AVB install container.

No device access or product/profile changes. The pinned AOSP avbtool copy in
upstream/debian-piano-current (a75f8c5d5fa099d65c171ac839c2e3bb6c63ec45)
is version 1.3.0. Generate unsigned metadata with flags zero and a recovery hash;
then copy its footer to the 100 MiB partition end, preserving the small image,
as AOSP fastboot's copy_avb_footer does. This verifies structure and host hashes,
not the vendor bootloader's acceptance or boot-partition compatibility.
Sources: https://android.googlesource.com/platform/external/avb/
https://android.googlesource.com/platform/system/core/+/refs/heads/main/fastboot/fastboot.cpp#1206
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
AVBTOOL = ROOT / 'upstream/debian-piano-current/avb/avbtool.py'
AVBTOOL_SHA256 = 'e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b'
AVBTOOL_REVISION = 'a75f8c5d5fa099d65c171ac839c2e3bb6c63ec45'
PARTITION_BYTES = 100 * 1024 * 1024


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def audit_container(path, canonical):
    """Independently check the footer, unsigned header and sole hash descriptor."""
    if path.stat().st_size != PARTITION_BYTES:
        raise ValueError('Recovery container must be exactly 100 MiB')
    with path.open('rb') as stream:
        if stream.read(len(canonical)) != canonical:
            raise ValueError('Canonical prefix was changed')
        stream.seek(-64, 2)
        magic, major, minor, original, offset, size, reserved = struct.unpack('>4sIIQQQ28s', stream.read(64))
        if (magic, major, minor, original) != (b'AVBf', 1, 0, len(canonical)) or any(reserved):
            raise ValueError('Invalid AVB footer or original image size')
        if offset != (len(canonical) + 4095) // 4096 * 4096 or not 256 <= size <= 65536 or offset + size > PARTITION_BYTES - 64:
            raise ValueError('Invalid vbmeta extent')
        stream.seek(offset)
        meta = stream.read(size)
    if meta[:4] != b'AVB0' or struct.unpack_from('>II', meta, 4) != (1, 0):
        raise ValueError('Invalid AVB0 header/version')
    auth, auxiliary = struct.unpack_from('>QQ', meta, 12)
    algorithm = struct.unpack_from('>I', meta, 28)[0]
    desc_offset, desc_bytes, rollback = struct.unpack_from('>QQQ', meta, 96)
    flags, rollback_location = struct.unpack_from('>II', meta, 120)
    if auth or algorithm or flags or rollback != 1 or rollback_location or size != 256 + auxiliary:
        raise ValueError('Expected unsigned NONE, flags 0, rollback 1 metadata')
    hash_offset, hash_size, signature_offset, signature_size, key_offset, key_size, key_meta_offset, key_meta_size = struct.unpack_from('>8Q', meta, 32)
    if (hash_offset or hash_size or signature_offset or signature_size or key_size or key_meta_size or
            key_offset > auxiliary or key_meta_offset > auxiliary or desc_offset + desc_bytes > auxiliary):
        raise ValueError('Unexpected authentication/key fields or descriptor extent')
    desc = meta[256 + desc_offset:256 + desc_offset + desc_bytes]
    if len(desc) < 132:
        raise ValueError('Missing recovery hash descriptor')
    tag, following, image_size, hash_alg, name_size, salt_size, digest_size, desc_flags, desc_reserved = struct.unpack_from('>QQQ32sIIII60s', desc)
    if tag != 2 or following + 16 != len(desc) or image_size != len(canonical) or hash_alg.rstrip(b'\0') != b'sha256':
        raise ValueError('Expected exactly one SHA256 hash descriptor')
    if name_size != 8 or salt_size or digest_size != 32 or desc_flags or any(desc_reserved):
        raise ValueError('Unexpected hash descriptor policy')
    if desc[132:140] != b'recovery' or desc[140:172] != hashlib.sha256(canonical).digest() or any(desc[172:]):
        raise ValueError('Recovery name/hash/padding mismatch')
    return {'original_image_bytes': original, 'vbmeta_offset': offset,
            'vbmeta_bytes': size, 'algorithm': 'NONE', 'flags': flags,
            'rollback_index': rollback, 'hash_partition_names': ['recovery'],
            'hash_algorithm': 'sha256', 'salt_hex': '', 'host_hash_verified': True}


def package(image, product_manifest, output_dir):
    image, product_manifest, output_dir = image.resolve(), product_manifest.resolve(), output_dir.resolve()
    manifest = json.loads(product_manifest.read_text())
    record = manifest.get('files', {}).get('PianoUEFI-product.img', {})
    if image.name != 'PianoUEFI-product.img' or manifest.get('target') != 'product' or manifest.get('artifact') != image.name:
        raise ValueError('Input must be the unique canonical product and its manifest')
    canonical = image.read_bytes()
    canonical_sha = hashlib.sha256(canonical).hexdigest()
    if record != {'bytes': len(canonical), 'sha256': canonical_sha}:
        raise ValueError('Canonical product does not match manifest size/SHA256')
    if len(canonical) < 4096 or len(canonical) > PARTITION_BYTES - 69632 or canonical[:8] != b'ANDROID!' or struct.unpack_from('<I', canonical, 40)[0] not in (2, 3, 4):
        raise ValueError('Expected a bounded Android v2/v3/v4 canonical product')
    if struct.unpack_from('<I', canonical, 40)[0] == 2:
        if struct.unpack_from('<I', canonical, 36)[0] != 4096 or struct.unpack_from('<I', canonical, 1644)[0] != 1660:
            raise ValueError('Expected Android v2/4096 with a complete header')
    if canonical[-64:-60] == b'AVBf':
        raise ValueError('Canonical product already has an AVB footer')
    if sha(AVBTOOL) != AVBTOOL_SHA256:
        raise ValueError('Workspace avbtool source pin mismatch')
    if not output_dir.is_relative_to(ROOT / 'private/provisioning'):
        raise ValueError('Output must be a new directory inside private/provisioning')
    if output_dir.exists():
        raise ValueError('Output directory already exists; refusing to replace it')
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(AVBTOOL)]
    version = subprocess.check_output(command + ['version'], text=True).strip()
    if version != 'avbtool 1.3.0':
        raise ValueError('Pinned avbtool version mismatch')
    with tempfile.TemporaryDirectory(prefix='.piano-recovery-avb-', dir=output_dir.parent) as temporary:
        work = Path(temporary)
        small = work / 'small.img'
        small.write_bytes(canonical)
        subprocess.run(command + ['add_hash_footer', '--image', str(small),
                                  '--dynamic_partition_size', '--partition_name', 'recovery',
                                  '--algorithm', 'NONE', '--flags', '0', '--rollback_index', '1',
                                  '--hash_algorithm', 'sha256', '--salt', ''], check=True)
        with small.open('rb') as stream:
            stream.seek(-64, 2)
            footer = stream.read(64)
        # AOSP fastboot retains the small footer too; vbmeta offsets stay fixed.
        verify_alias = work / 'recovery.bin'
        with verify_alias.open('xb') as stream, small.open('rb') as source:
            shutil.copyfileobj(source, stream)
            stream.seek(PARTITION_BYTES - 64)
            stream.write(footer)
        avb = audit_container(verify_alias, canonical)
        info = subprocess.check_output(command + ['info_image', '--image', str(verify_alias)], text=True)
        verification = subprocess.check_output(command + ['verify_image', '--image', str(verify_alias)], text=True)
        if sha(image) != canonical_sha or json.loads(product_manifest.read_text()) != manifest:
            raise ValueError('Canonical input/manifest changed during packaging')
        container_sha = sha(verify_alias)
        result = {'status': 'RECOVERY_ONLY_INSTALL_CONTAINER',
                  'canonical_image': str(image), 'canonical_manifest': str(product_manifest),
                  'canonical_product_sha256': canonical_sha, 'canonical_product_bytes': len(canonical),
                  'canonical_image_unchanged': True, 'canonical_prefix_byte_equal': True,
                  'core_application_sha256': manifest.get('core_application_sha256'),
                  'core_application_unchanged': True, 'target_partition': 'recovery',
                  'container': 'install-container.bin', 'container_bytes': PARTITION_BYTES,
                  'container_sha256': container_sha, 'avb': avb,
                  'avbtool': {'path': str(AVBTOOL), 'sha256': AVBTOOL_SHA256,
                              'workspace_revision': AVBTOOL_REVISION, 'version': version,
                              'origin': 'https://android.googlesource.com/platform/external/avb/'},
                  'host_info_image_passed': True, 'host_verify_image_passed': True,
                  'signed': False, 'device_boot_performed': False, 'device_writes_performed': False,
                  'other_partitions_modified': False,
                  'entry_compatibility': {'recovery': 'HOST_STRUCTURE_AND_HASH_VERIFIED_DEVICE_PENDING',
                                          'boot_partition': 'NOT_CLAIMED',
                                          'fastboot_boot_install_container': 'NOT_CLAIMED'},
                  'verification_note': 'Host verification proves structure and payload hash; unsigned metadata requires unlocked vendor acceptance, not yet tested.'}
        staging = work / 'result'
        staging.mkdir()
        os.rename(verify_alias, staging / 'install-container.bin')
        (staging / 'avb-info.txt').write_text(info)
        (staging / 'avb-verify.txt').write_text(verification)
        (staging / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        if output_dir.exists():
            raise ValueError('Output directory appeared during packaging')
        os.rename(staging, output_dir)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=ROOT / 'artifacts/product/PianoUEFI-product.img')
    parser.add_argument('--manifest', type=Path, default=ROOT / 'artifacts/product/manifest.json')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    result = package(args.image, args.manifest, args.output_dir)
    print(json.dumps({'status': result['status'], 'output_dir': str(args.output_dir.resolve()),
                      'canonical_product_sha256': result['canonical_product_sha256'],
                      'container_sha256': result['container_sha256'],
                      'container_bytes': result['container_bytes'], 'device_boot_performed': False}, indent=2))


if __name__ == '__main__':
    main()
