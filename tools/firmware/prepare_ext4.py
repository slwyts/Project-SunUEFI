#!/usr/bin/env python3
"""Stage the fixed lwext4 filesystem library in the disposable product tree."""
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import urllib.request


def prepare(root, target):
    root, target = Path(root), Path(target)
    config = json.loads((root / 'config/uefi-ext4.json').read_text())
    cache = root / 'build/source-cache' / ('lwext4-' + config['commit'] + '.tar.gz')
    if not cache.exists():
        cache.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(config['url'], timeout=60) as response:
            data = response.read(2 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != config['sha256']:
            raise ValueError('lwext4 source digest mismatch')
        cache.write_bytes(data)
    if hashlib.sha256(cache.read_bytes()).hexdigest() != config['sha256']:
        raise ValueError('lwext4 source cache digest mismatch')
    destination = target / 'Drivers/PianoExt4Dxe'
    shutil.copytree(root / 'uefi/components/ext4', destination)
    library = destination / 'lwext4'
    library.mkdir()
    with tarfile.open(cache, 'r:gz') as archive:
        prefix = 'lwext4-' + config['commit'] + '/'
        for entry in archive:
            relative = entry.name.removeprefix(prefix)
            if not entry.isfile() or not relative.startswith(('include/', 'src/')):
                continue
            path = Path(relative)
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Unexpected lwext4 archive path')
            path = library / path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.extractfile(entry).read())
    # Do not compile formatting/partitioning utilities into the firmware.
    sources = sorted(path.relative_to(destination).as_posix() for path in
                     (library / 'src').glob('*.c') if path.name not in ('ext4_mkfs.c', 'ext4_mbr.c'))
    # lwext4's high-level fwrite overwrites an earlier IO error with the inode
    # release result. Preserve errors; the device backend also latches failures.
    api=library/'src/ext4.c'
    text=api.read_text()
    old='Finish:\n\tr = ext4_fs_put_inode_ref(&ref);\n\n\tif (r != EOK)\n\t\text4_trans_abort(file->mp);\n\telse\n\t\text4_trans_stop(file->mp);'
    new='Finish:\n\t{ int release = ext4_fs_put_inode_ref(&ref); if (r == EOK) r = release; }\n\n\tif (r != EOK)\n\t\text4_trans_abort(file->mp);\n\telse\n\t\tr = ext4_trans_stop(file->mp);'
    if text.count(old)!=1:raise ValueError('lwext4 fwrite error boundary changed')
    api.write_text(text.replace(old,new,1))
    inf = destination / 'PianoExt4Dxe.inf'
    # BaseTools expands MODULE_DIR to a package-relative path when it emits a
    # compiler response file, unlike the absolute make variable. Use the
    # prepared module's absolute include root in both compilation modes.
    text = inf.read_text().replace('$(MODULE_DIR)', destination.resolve().as_posix())
    inf.write_text(text.replace('  # LWEXT4_SOURCES', ''.join('  ' + name + '\n' for name in sources)))
    (destination / 'SOURCE.json').write_text(json.dumps(config, indent=2) + '\n')
    return {'module': 'pianoProductPkg/Drivers/PianoExt4Dxe/PianoExt4Dxe.inf',
            'source': config, 'formatting_included': False}
