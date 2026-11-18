"""Check completed package sets against the current distribution build inputs."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from build import overlay_packages, shared_products
from build_piano_ram_bootstrap import guest_resolve

RECIPE_PATHS = ('build.sh', 'requirements-build.txt', 'sources.lock.json',
                'tools', 'config', 'containers', 'patches', 'linux',
                ':(exclude,literal)tools/check_host.py',
                ':(exclude,literal)tools/prepare_nv_runtime_guard.py')


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def recipe_identity(project):
    return digest(shared_products.source_identity(project, RECIPE_PATHS))


def pacman_state(root):
    """Read libalpm's actual config and sync files, including Valve's DBPath."""
    def query(*command):
        return subprocess.check_output(
            ['chroot', root, 'env', 'LC_ALL=C', *command], text=True).strip()

    config = query('pacman-conf')
    names = query('pacman-conf', '--repo-list').splitlines()
    db_path = query('pacman-conf', 'DBPath')
    if (not names or len(set(names)) != len(names) or not db_path.startswith('/') or
            any(not re.fullmatch(r'[A-Za-z0-9_-]+', name) for name in names)):
        raise ValueError('Cannot identify the pacman repositories used to build overlays')
    files = {}
    for name in names:
        for suffix in ('.db', '.db.sig'):
            filename = name + suffix
            path = guest_resolve(root, str(Path(db_path) / 'sync' / filename))
            if suffix == '.db' or path.exists():
                files[filename] = overlay_packages.sha(path)
    return {'config': digest(config), 'databases': files}


def pacman_inputs(project, root, kernel_manifest, base, extra=None):
    """The package manager has already refreshed and authenticated this root."""
    image = os.environ.get('SUNUEFI_BUILD_ENVIRONMENT_ID')
    if not image:
        return None
    kernel = json.loads(kernel_manifest.read_text())
    installed = subprocess.check_output(
        ['chroot', root, 'env', 'LC_ALL=C', 'pacman', '-Q'], text=True)
    return {
        'builder_image': image,
        'recipes': recipe_identity(project),
        'base': base,
        'kernel': {name: kernel[name] for name in
                   ('source_tree', 'config_sha256', 'module_symvers_sha256')},
        'upstream': extra,
        'repositories': pacman_state(root),
        'installed': digest(sorted(installed.splitlines())),
    }


def check_pacman_inputs(project, root, inputs):
    if inputs is None:
        return
    if (inputs['repositories'] != pacman_state(root) or
            inputs['recipes'] != recipe_identity(project)):
        raise ValueError('Pacman overlay inputs changed during the build; retry with fresh inputs')


def restore_pacman(project, directory, target, inputs):
    """A cache hit requires fresh inputs and matching bytes for every archive."""
    if inputs is None:
        return None
    candidate = project / 'build/overlay-cache' / target
    manifest = candidate / 'manifest.json'
    if candidate.is_symlink() or manifest.is_symlink() or not manifest.is_file():
        return None
    try:
        record = json.loads(manifest.read_text())
        if record.get('build_inputs') != inputs or record.get('target') != target:
            return None
        repo, packages, components = overlay_packages.read_pacman(manifest, target)
        if {path.name for path in candidate.iterdir()} != {'manifest.json', *packages}:
            raise ValueError('Cached overlay package set is incomplete')
        paths = {name: (candidate / name, row) for name, row in packages.items()}
        for path, row in paths.values():
            identity = row['name'], row['version'], row['architecture']
            if (path.is_symlink() or not path.is_file() or overlay_packages.sha(path) != row['sha256'] or
                    overlay_packages.pacman_identity(path) != identity):
                raise ValueError('Cached package differs: ' + path.name)
    except (OSError, ValueError, TypeError, KeyError, subprocess.SubprocessError) as error:
        print(f'Ignoring the cached {target} packages: {error}', flush=True)
        return None
    print(f'Reusing {len(paths)} verified {target} packages with unchanged build inputs', flush=True)
    return overlay_packages.export(directory, target, repo, paths, components, build_inputs=inputs)


def debian_recipe_identity(project):
    inputs = (*RECIPE_PATHS, 'vendor/firmware/hyperos4-fingerprint/manifest.json')
    return digest(shared_products.source_identity(project, inputs))


def debian_inputs(project, root, uapi_sha256):
    """Bind the completed Debian set to a fresh base and the current sid archive."""
    from desktop import debian_package

    def query(*arguments):
        return debian_package.capture(['chroot', root, 'env', 'LC_ALL=C', *arguments])

    installed = query('dpkg-query', '-W',
                      '-f=${binary:Package}\t${Version}\t${Architecture}\t${Status}\n')
    return {
        'builder_image': os.environ['SUNUEFI_BUILD_ENVIRONMENT_ID'],
        'recipes': debian_recipe_identity(project),
        'kernel_uapi_sha256': uapi_sha256,
        'archive': debian_package.archive_inputs(root),
        'apt_policy': digest([query('apt-config', 'dump'),
                              query('apt-cache', '-o', 'Dir::State::status=/dev/null', 'policy')]),
        'installed': digest(sorted(installed.splitlines())),
    }


def restore_debian(project, directory, target, inputs):
    """Re-export only the complete Debian package set for this exact build."""
    candidate = project / 'build/overlay-cache' / target
    manifest = candidate / 'manifest.json'
    if candidate.is_symlink() or manifest.is_symlink() or not manifest.is_file():
        return None
    try:
        record = json.loads(manifest.read_text())
        if record.get('build_inputs') != inputs or record.get('target') != target:
            return None
        components, packages = overlay_packages.read_debian(manifest, target.removeprefix('debian-'))
        if (record['packages'] != packages or
                {path.name for path in candidate.iterdir()} != {'manifest.json', *packages}):
            raise ValueError('Cached overlay package set is incomplete')
        sources = {component: (candidate, metadata) for component, metadata in components.items()}
        paths = overlay_packages.debian_packages(sources)
    except (OSError, ValueError, TypeError, KeyError, subprocess.CalledProcessError) as error:
        print(f'Ignoring the cached {target} packages: {error}', flush=True)
        return None
    print(f'Reusing {len(paths)} verified {target} packages with unchanged build inputs', flush=True)
    return overlay_packages.export(directory, target, 'apt', paths, components, build_inputs=inputs)
