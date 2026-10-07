#!/usr/bin/env python3
"""Export exact standalone installer/launchers and optional verified build files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

from install_piano import image_info

ROOT = Path(__file__).resolve().parents[1]


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def copy_file(source, dest, expected=None, large=False):
    require(source.is_file() and not source.is_symlink(), 'Expected regular export input: ' + source.name)
    before = expected or digest(source)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if large:
        try:
            os.link(source, dest)  # CI staging, same immutable build bytes; no 8-GiB duplicate.
        except OSError:
            shutil.copy2(source, dest)
    else:
        shutil.copy2(source, dest)
    require(digest(dest) == before and digest(source) == before, 'Export input changed: ' + source.name)
    return {'sha256': before, 'bytes': dest.stat().st_size}


def bundle_inputs(base):
    require(base.is_dir() and not base.is_symlink() and not (base / '.incomplete').exists(), 'Incomplete/missing disk bundle')
    manifest_path = base / 'manifest.json'
    require(manifest_path.is_file() and not manifest_path.is_symlink() and manifest_path.stat().st_size <= 1024 * 1024, 'Missing disk-bundle manifest')
    data = json.loads(manifest_path.read_text())
    require(data.get('schema_version') == 1 and data.get('status') == 'HOST_BUILT_NOT_DEVICE_READY'
            and data.get('root_policy') == 'LABEL=PIANOROOT', 'Not a completed host disk bundle')
    files = data.get('files', {})
    require(isinstance(files, dict) and all(name in files for name in ('esp.img', 'root.ext4.img')), 'Disk export needs both ESP and root images')
    selected = []
    for name, row in files.items():
        require(isinstance(name, str) and name == Path(name).name and name not in ('.', '..') and '/' not in name and '\\' not in name, 'Unsafe disk-bundle filename')
        require(name in {'PianoUEFI-product.img', 'boot.img', 'esp.img', 'root.ext4.img', 'root.tar.zst'}, 'Unexpected disk-bundle component: ' + name)
        source = base / name
        require(source.is_file() and not source.is_symlink() and type(row.get('bytes')) is int
                and source.stat().st_size == row['bytes'] and digest(source) == row.get('sha256'), 'Disk-bundle bytes/hash differ: ' + name)
        selected.append((source, row['sha256']))
    for name, kind, label in (('esp.img', 'esp', 'SUNUEFI_ESP'), ('root.ext4.img', 'root', 'PIANOROOT')):
        require(image_info(base / name, kind)['label'] == label, 'Disk image label differs: ' + name)
    return [(manifest_path, digest(manifest_path)), *selected]


def export(output, bundle=None, product=None):
    output = Path(output).absolute()
    require(not output.exists() and not output.is_symlink(), 'Export output must be a fresh directory')
    selected = bundle_inputs(Path(bundle)) if bundle else []
    if product:
        product = Path(product)
        require(product.is_file() and not product.is_symlink(), 'Missing product image')
    output.mkdir(parents=True)
    (output / '.incomplete').write_text('Installer export has not completed.\n')
    files = {}
    sources = [(ROOT / 'tools/install_piano.py', 'install_piano.py'),
               (ROOT / 'docs/user/install-from-artifact.md', 'INSTALL.md')]
    sources += [(ROOT / 'tools/installer-launchers' / name, name)
                for name in ('install.sh', 'install.cmd', 'installer_launcher.py')]
    for source, name in sources:
        files[name] = copy_file(source, output / name)
    (output / 'install.sh').chmod(0o755)
    for source, expected in selected:
        name = 'bundle/' + source.name
        files[name] = copy_file(source, output / name, expected, large=source.suffix == '.img' or source.name.endswith('.zst'))
    if product and not bundle:
        files['PianoUEFI-product.img'] = copy_file(product, output / 'PianoUEFI-product.img', large=True)
        source_manifest = product.parent / 'manifest.json'
        if source_manifest.is_file():
            files['uefi-manifest.json'] = copy_file(source_manifest, output / 'uefi-manifest.json')
    if bundle:
        require(not (Path(bundle) / '.incomplete').exists(), 'Disk bundle became incomplete during export')
    record = {'schema_version': 1, 'status': 'HOST_EXPORTED_INSTALLER_NOT_DEVICE_TESTED',
              'kind': 'disk-bundle' if bundle else 'uefi-utility' if product else 'utility-only',
              'installer_source': 'tools/install_piano.py', 'bundle': 'bundle' if bundle else None,
              'default_action': 'help; explicit --serial uses read-only inspect', 'device_operation_performed': False,
              'fresh_partition_install_ready': False, 'recovery_install_ready': False, 'files': files}
    (output / 'installer-record.json').write_text(json.dumps(record, indent=2) + '\n')
    paths = sorted(p for p in output.rglob('*') if p.is_file() and p.name != '.incomplete')
    (output / 'SHA256SUMS').write_text(''.join(f'{digest(path)}  {path.relative_to(output).as_posix()}\n' for path in paths))
    (output / '.incomplete').unlink()
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts/installer')
    parser.add_argument('--bundle', type=Path, help='Complete host disk bundle to include unchanged')
    parser.add_argument('--product', type=Path, help='Optional UEFI-only product; never fabricates a disk manifest')
    args = parser.parse_args()
    try:
        print(json.dumps(export(args.output, args.bundle, args.product), indent=2))
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        raise SystemExit(str(error))
