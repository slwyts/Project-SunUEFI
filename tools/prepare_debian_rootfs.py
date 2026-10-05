#!/usr/bin/env python3
"""Import the official single-layer Debian ARM64 OCI rootfs into the workspace.

No device access, host installation or boot claim. Registry token is transient
and never saved. Every manifest/config/layer digest and uncompressed diff ID is
checked. Native execution and a usable kernel/init require separate validation.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile

REGISTRY = 'https://registry-1.docker.io/v2/library/debian'
ACCEPT = ','.join(('application/vnd.oci.image.index.v1+json',
    'application/vnd.docker.distribution.manifest.list.v2+json',
    'application/vnd.oci.image.manifest.v1+json',
    'application/vnd.docker.distribution.manifest.v2+json'))
MAX_BLOB = 256*1024*1024
MAX_EXPANDED = 1024*1024*1024


def sha(data):
    return 'sha256:'+hashlib.sha256(data).hexdigest()


def digest(value):
    if not isinstance(value, str) or not re.fullmatch(r'sha256:[0-9a-f]{64}', value):
        raise ValueError('Unsupported or malformed OCI digest')
    return value


def checked(data, expected):
    if sha(data) != digest(expected):
        raise ValueError('OCI content digest mismatch')
    return data


def name(value):
    while value.startswith('./'):
        value = value[2:]
    if value in ('', '.'):
        return None
    path = PurePosixPath(value)
    if len(value)>4096 or path.is_absolute() or '\\' in value or '\0' in value or any(x in ('', '.', '..') for x in value.split('/')):
        raise ValueError('Unsafe rootfs member path')
    if any(part.startswith('.wh.') for part in path.parts):
        raise ValueError('Whiteout unsupported in the required single base layer')
    return path


def parents(root, path):
    current = root
    for part in path.parts[:-1]:
        current = current/part
        if current.is_symlink() or (current.exists() and not current.is_dir()):
            raise ValueError('Rootfs write traverses a non-directory')
        current.mkdir(exist_ok=True)


def extract_layer(archive, root):
    """Delay symlinks and never follow them while materializing image files."""
    links, hardlinks, directories = [], [], []
    total = 0
    root.mkdir()
    with tarfile.open(archive, 'r:') as stream:
        for count, item in enumerate(stream, 1):
            if count>100000:
                raise ValueError('Rootfs entry budget exceeded')
            relative = name(item.name)
            if relative is None:
                continue
            parents(root, relative)
            target = root/relative
            if target.is_symlink():
                raise ValueError('Duplicate symlink destination')
            if item.isdir():
                target.mkdir(exist_ok=True)
                directories.append((target, item.mode))
            elif item.isfile():
                total += item.size
                if total > MAX_EXPANDED or item.size < 0:
                    raise ValueError('Rootfs file budget exceeded')
                with stream.extractfile(item) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(item.mode & 0o7777)
            elif item.issym():
                if not item.linkname or '\0' in item.linkname:
                    raise ValueError('Malformed guest symlink')
                links.append((target, item.linkname))
            elif item.islnk():
                source = name(item.linkname)
                if source is None:
                    raise ValueError('Malformed hardlink')
                hardlinks.append((target, root/source))
            else:
                raise ValueError('Special node unsupported in base rootfs')
    for target, source in hardlinks:
        if not source.is_file() or source.is_symlink() or not source.resolve().is_relative_to(root.resolve()):
            raise ValueError('Hardlink does not target an extracted regular file')
        os.link(source, target, follow_symlinks=False)
    for target, source in links:
        # Absolute links retain guest-root meaning. They are installed last;
        # no importer write or hash traversal follows them on the host.
        target.symlink_to(source)
    for target, mode in reversed(directories):
        target.chmod(mode & 0o7777)
    return total


def tree_digest(root):
    rows = []
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            rows.append([relative, 'link', os.readlink(path)])
        elif path.is_file():
            rows.append([relative, 'file', path.stat().st_mode & 0o7777, sha(path.read_bytes())])
        elif path.is_dir():
            rows.append([relative, 'dir', path.stat().st_mode & 0o7777])
        else:
            raise ValueError('Unexpected materialized rootfs node')
    return sha(json.dumps(rows, separators=(',', ':')).encode()), len(rows)


class Registry:
    def __init__(self):
        url = 'https://auth.docker.io/token?service=registry.docker.io&scope=repository:library/debian:pull'
        self.token = json.loads(self.fetch(url, 1024*1024))['token']

    @staticmethod
    def fetch(url, maximum, token=None):
        # curl supports this host's working TLS route. Sensitive headers use
        # stdin rather than process arguments, files or diagnostic output.
        lines = ['url = '+json.dumps(url), 'silent', 'show-error', 'fail',
                 'location', 'proto = "=https"', 'max-time = 45',
                 'max-filesize = '+str(maximum), 'header = '+json.dumps('Accept: '+ACCEPT)]
        if token:
            lines.append('header = '+json.dumps('Authorization: Bearer '+token))
        result = subprocess.run(['curl', '--config', '-'], input=('\n'.join(lines)+'\n').encode(),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=50)
        if result.returncode:
            raise OSError('Registry HTTPS request failed: '+result.stderr.decode(errors='replace').strip())
        if len(result.stdout) > maximum:
            raise ValueError('Registry object exceeds bounded size')
        return result.stdout

    def get(self, path, maximum):
        return self.fetch(REGISTRY+'/'+path, maximum, self.token)


def prepare(tag, output):
    if tag not in ('trixie-slim', 'trixie-20260918-slim') and not re.fullmatch(r'sha256:[0-9a-f]{64}', tag):
        raise ValueError('Expected an approved Debian 13 tag or exact digest')
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise ValueError('Output exists; preserve imported rootfs')
    registry = Registry()
    index_bytes = registry.get('manifests/'+tag, 2*1024*1024)
    if tag.startswith('sha256:'):
        checked(index_bytes, tag)
    index = json.loads(index_bytes)
    if index.get('schemaVersion') != 2:
        raise ValueError('Unsupported registry index schema')
    candidates = [item for item in index.get('manifests', [])
        if item.get('platform', {}).get('os') == 'linux' and item['platform'].get('architecture') == 'arm64'
        and item['platform'].get('variant', 'v8') == 'v8']
    if len(candidates) != 1:
        raise ValueError('Expected one explicit linux/arm64/v8 manifest')
    manifest_bytes = checked(registry.get('manifests/'+digest(candidates[0]['digest']), 2*1024*1024), candidates[0]['digest'])
    manifest = json.loads(manifest_bytes)
    if manifest.get('schemaVersion') != 2 or len(manifest.get('layers', [])) != 1:
        raise ValueError('Expected the official single Debian base layer')
    config_bytes = checked(registry.get('blobs/'+digest(manifest['config']['digest']), 2*1024*1024), manifest['config']['digest'])
    config = json.loads(config_bytes)
    if config.get('architecture') != 'arm64' or config.get('os') != 'linux' or config.get('rootfs', {}).get('type')!='layers' or len(config.get('rootfs', {}).get('diff_ids', [])) != 1:
        raise ValueError('OCI config does not describe one ARM64 Linux layer')
    layer = manifest['layers'][0]
    if layer.get('mediaType') not in ('application/vnd.oci.image.layer.v1.tar+gzip','application/vnd.docker.image.rootfs.diff.tar.gzip'):
        raise ValueError('Expected a gzip OCI filesystem layer')
    if not 0 < layer['size'] <= MAX_BLOB:
        raise ValueError('Layer size outside budget')
    output.mkdir(parents=True, exist_ok=False)
    try:
        (output/'index.json').write_bytes(index_bytes)
        (output/'oci-manifest.json').write_bytes(manifest_bytes)
        (output/'oci-config.json').write_bytes(config_bytes)
        compressed = checked(registry.get('blobs/'+digest(layer['digest']), MAX_BLOB), layer['digest'])
        if len(compressed) != layer['size']:
            raise ValueError('Compressed layer size changed')
        (output/'layer.tar.gz').write_bytes(compressed)
        with gzip.GzipFile(fileobj=io.BytesIO(compressed)) as stream:
            expanded = stream.read(MAX_EXPANDED+1)
        if len(expanded) > MAX_EXPANDED:
            raise ValueError('Expanded layer exceeds budget')
        checked(expanded, config['rootfs']['diff_ids'][0])
        archive = output/'layer.tar'
        archive.write_bytes(expanded)
        total = extract_layer(archive, output/'rootfs')
        tree, count = tree_digest(output/'rootfs')
        result = {'schema': 1, 'status': 'OFFICIAL_ARM64_USERSPACE_IMPORTED_NOT_BOOT_VERIFIED',
            'registry': REGISTRY, 'requested_ref': tag, 'index_digest': sha(index_bytes),
            'arm64_manifest_digest': sha(manifest_bytes), 'config_digest': sha(config_bytes),
            'layer_digest': layer['digest'], 'diff_id': config['rootfs']['diff_ids'][0],
            'rootfs_tree_digest': tree, 'rootfs_entries': count, 'rootfs_regular_bytes': total,
            'architecture': 'arm64', 'distribution': 'Debian 13', 'device_writes': False,
            'piano_boot_verified': False, 'pid1_verified': False}
        (output/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    except Exception:
        (output/'FAILED.txt').write_text('Incomplete import; do not boot.\n')
        raise
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref', default='trixie-slim')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.ref, args.output), indent=2))
