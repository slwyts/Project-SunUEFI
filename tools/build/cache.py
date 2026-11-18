"""Hash build inputs and atomically record verified cache contents."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_identity(root, names):
    output = subprocess.check_output(
        ['git', '-C', root, 'ls-files', '--cached', '--others', '--exclude-standard',
         '-z', '--', *names])
    rows = {}
    for raw in output.split(b'\0'):
        if not raw:
            continue
        name = os.fsdecode(raw)
        path = root / name
        if path.is_dir():
            # Source pins are in sources.lock.json and checked by build.sh.
            continue
        if path.suffix.lower() in ('.md', '.rst') or '__pycache__' in path.parts:
            continue
        if path.is_symlink():
            rows[name] = {'link': os.readlink(path)}
        else:
            rows[name] = {'sha256': sha(path), 'mode': path.stat().st_mode & 0o777}
    # Include a new helper during its first local invocation, before git add.
    for name in names:
        path = root / name
        if path.is_file() and name not in rows:
            rows[name] = {'sha256': sha(path), 'mode': path.stat().st_mode & 0o777}
    if not rows:
        raise ValueError('No tracked build inputs found')
    return rows


def atomic_json(path, record):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', prefix=path.name + '.',
                                     dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(record, stream, indent=2)
        stream.write('\n')
        stream.flush()
        # The runner must be able to save records written inside Docker.
        os.fchmod(stream.fileno(), 0o644)
        os.fsync(stream.fileno())
    temporary.replace(path)
