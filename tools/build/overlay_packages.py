"""Collect and verify completed native packages before assembling product images."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

from build import overlay_release
from build_piano_ram_bootstrap import guest_resolve
import package_bsp


DEBIAN_SHARED = ('mesa', 'sensors', 'ffmpeg', 'pipewire', 'libwacom')
DEBIAN_GNOME = ('gnome_power', 'mutter', 'gdm')
ARCH_REPOSITORIES = {'nyarch-gnome': 'sunuefi', 'steamos': 'sunuefi-steamos'}
PACMAN_SUFFIX = r'\.pkg\.tar\.(?:zst|xz|gz)'
BSP_TARGETS = {'debian': ('debian', 'deb', 'all'), 'pacman': ('arch', 'pacman', 'any')}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def debian_identity(path):
    output = subprocess.check_output(
        ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture'], text=True)
    fields = dict(line.split(': ', 1) for line in output.splitlines())
    return fields['Package'], fields['Version'], fields['Architecture']


def bsp_package(directory, package_format, *, component='config'):
    """Read the exact common package sealed by the builder."""
    distro, archive_format, architecture = BSP_TARGETS[package_format]
    expected = {'config': package_bsp.NAME, 'audio-topology': package_bsp.TOPOLOGY_PACKAGE}[component]
    manifest = directory / 'manifest.json'
    if directory.is_symlink() or manifest.is_symlink() or (directory / '.incomplete').exists():
        raise ValueError('Device package did not complete: ' + str(directory))
    record = json.loads(manifest.read_text())
    if (record.get('schema_version') != 1 or record.get('component') != component or
            record.get('package') != expected or record.get('features', []) != [] or
            record.get('target', {}).get('distro') != distro or
            record.get('package_architecture') != architecture or
            record.get('package_ready') is not True or len(record.get('files', ())) != 1):
        raise ValueError('Unexpected device package manifest: ' + str(manifest))
    filename, info = next(iter(record['files'].items()))
    archive = directory / filename
    if (Path(filename).name != filename or info['format'] != archive_format or
            not filename.endswith('.deb' if package_format == 'debian' else '.pkg.tar.zst') or
            archive.is_symlink() or not archive.is_file() or archive.stat().st_size != info['bytes'] or
            sha(archive) != info['sha256']):
        raise ValueError('Device package archive differs: ' + filename)
    read_identity = debian_identity if package_format == 'debian' else pacman_identity
    name, version, arch = read_identity(archive)
    if (name, version, arch) != (expected, info['version'], architecture):
        raise ValueError('Unexpected device archive metadata: ' + filename)
    metadata = {'name': name, 'version': version, 'architecture': arch, 'sha256': info['sha256']}
    if component == 'audio-topology':
        from build_piano_runtime_helpers import TOPOLOGY_PATH, TOPOLOGY_PIN
        path = (Path(package_bsp.FIRMWARE_ROOT) / TOPOLOGY_PATH).as_posix()
        payload = record.get('payload_files', {})
        if (set(payload) != {path} or payload[path]['path'] != path or
                payload[path]['type'] != 'file' or payload[path]['mode'] != '0644' or
                payload[path]['sha256'] != TOPOLOGY_PIN or record.get('public_distribution') is not True):
            raise ValueError('Audio topology package differs from the release policy')
        metadata['firmware'] = payload[path]
    return archive, metadata


def build_bsp(directory, package_format, run=None, *, component='config'):
    distro, archive_format, _ = BSP_TARGETS[package_format]
    command = [package_bsp.ROOT / 'build.sh', 'bsp', 'package', '--target', distro,
               '--format', archive_format, '--output', directory]
    if component == 'audio-topology':
        command.append('--audio-topology')
    if run is None:
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    else:
        run(command)
    return bsp_package(directory, package_format, component=component)


def shared_bsp(package_format):
    return bsp_package(package_bsp.ROOT / 'artifacts/packages/bsp' / package_format,
                       package_format)


def topology_package(directory, package_format):
    return bsp_package(directory / 'audio-topology' / package_format, package_format,
                       component='audio-topology')


def shared_topology(package_format):
    return topology_package(package_bsp.ROOT / 'artifacts/packages/bsp', package_format)


def check_topology_conflicts(rootfs, metadata):
    package_bsp.conflict_check(rootfs, [(metadata['firmware'], None)], package_bsp.TOPOLOGY_PACKAGE)


def verify_topology(rootfs, package_format, metadata):
    check_topology_conflicts(rootfs, metadata)
    row = metadata['firmware']
    name = metadata['name']
    package_listing = (['dpkg-query', '-L', name] if package_format == 'debian'
                       else ['pacman', '-Qlq', name])
    paths = subprocess.check_output(['chroot', rootfs, *package_listing], text=True).splitlines()
    relative = Path(row['path'])
    path = guest_resolve(rootfs, relative.parent.as_posix()) / relative.name
    if ('/' + row['path'] not in paths or path.is_symlink() or not path.is_file() or
            stat.S_IMODE(path.stat().st_mode) != 0o644 or sha(path) != row['sha256']):
        raise ValueError('Audio topology differs from the installed package: ' + row['path'])


def verify_bsp_files(rootfs, package_format):
    """Check the final image keeps these files under native package ownership."""
    _, rows = package_bsp.payload([])
    if package_format == 'debian':
        arguments = ['dpkg-query', '-L', package_bsp.NAME]
    else:
        arguments = ['pacman', '-Qlq', package_bsp.NAME]
    paths = set(subprocess.check_output(['chroot', rootfs, *arguments], text=True).splitlines())
    for row, data in rows:
        name = row['path']
        relative = Path(name)
        path = guest_resolve(rootfs, relative.parent.as_posix()) / relative.name
        if '/' + name not in paths or not (path.exists() or path.is_symlink()):
            raise ValueError('Piano device configuration package does not own /' + name)
        is_link = path.is_symlink()
        if (row['type'] == 'symlink') != is_link or (not is_link and not path.is_file()):
            raise ValueError('Piano device configuration has the wrong file type: /' + name)
        # Package managers deliberately preserve administrator changes in /etc.
        if not name.startswith('etc/'):
            content = os.readlink(path).encode() if is_link else path.read_bytes()
            if content != data or stat.S_IMODE(path.lstat().st_mode) != int(row['mode'], 8):
                raise ValueError('Piano device configuration was overwritten: /' + name)


def debian_packages(sources):
    """Index the exact files accepted by the individual package builders."""
    packages = {}
    for component, (folder, record) in sources.items():
        for name, info in record['packages'].items():
            filename = info['file']
            path = folder / filename
            if (filename != Path(filename).name or not filename.endswith('.deb') or
                    path.is_symlink() or not path.is_file() or sha(path) != info['sha256']):
                raise ValueError('Invalid completed Debian package: ' + filename)
            identity = name, info['version'], info['architecture']
            if debian_identity(path) != identity or identity[2] not in ('arm64', 'all'):
                raise ValueError('Completed Debian package metadata differs: ' + filename)
            row = {'name': name, 'version': identity[1], 'architecture': identity[2],
                   'sha256': info['sha256'], 'component': component}
            if filename in packages:
                raise ValueError('Multiple components contain ' + filename)
            packages[filename] = (path, row)
    return packages


def verify_debian_generation(packages):
    """Only unchanged, authenticated FastRPC archives keep stock versions."""
    for filename, (_, record) in packages.items():
        if (record['component'] == 'sensors' and
                record['name'] in ('fastrpc-support', 'libfastrpc1')):
            continue
        if not overlay_release.uses_debian_generation(record['version']):
            raise ValueError('Rebuilt Debian package has no current publication revision: ' + filename)


def export(directory, target, repository, packages, components=None, *, build_inputs=None):
    """Publish no partial sets: the destination appears after copying all files."""
    if not packages or directory.exists() or directory.is_symlink():
        raise ValueError('Use a fresh package output directory and a non-empty package set')
    directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=directory.name + '-', dir=directory.parent) as temporary:
        stage = Path(temporary) / 'packages'
        stage.mkdir()
        rows = {}
        for name, (source, record) in sorted(packages.items()):
            if Path(name).name != name or source.is_symlink():
                raise ValueError('Invalid package output filename: ' + name)
            path = stage / name
            shutil.copyfile(source, path)
            if sha(path) != record['sha256']:
                raise ValueError('Package changed while collecting it: ' + name)
            path.chmod(0o644)
            rows[name] = record
        manifest = {'schema_version': 1, 'target': target, 'repo': repository, 'packages': rows}
        if components is not None:
            manifest['components'] = components
        if build_inputs is not None:
            manifest['build_inputs'] = build_inputs
        path = stage / 'manifest.json'
        path.write_text(json.dumps(manifest, indent=2) + '\n')
        if repository == 'apt':
            read_debian(path, target.removeprefix('debian-'))
        else:
            read_pacman(path, target)
        path.chmod(0o644)
        stage.chmod(0o755)
        stage.rename(directory)
    return manifest


def read_debian(path, desktop):
    manifest = json.loads(path.read_text())
    if (manifest.get('schema_version') != 1 or manifest.get('repo') != 'apt' or
            manifest.get('target') not in ('debian-gnome', 'debian-' + desktop)):
        raise ValueError('Use a completed Debian overlay package manifest for this desktop')
    components = {name: {'packages': {}} for name in DEBIAN_GNOME}
    needed = (*DEBIAN_SHARED, 'fingerprint', *(DEBIAN_GNOME if desktop == 'gnome' else ()))
    packages = {}
    names = set()
    for component in needed:
        metadata = manifest['components'].get(component)
        if not metadata or not metadata.get('packages'):
            raise ValueError('Missing Debian overlay component: ' + component)
        components[component] = metadata
        for name, info in metadata['packages'].items():
            filename = info['file']
            row = manifest['packages'].get(filename)
            expected = {'name': name, 'version': info['version'], 'architecture': info['architecture'],
                        'sha256': info['sha256'], 'component': component}
            if (row != expected or name in names or Path(filename).name != filename or
                    not filename.endswith('.deb') or not re.fullmatch(r'[a-z0-9][a-z0-9+.-]+', name) or
                    not re.fullmatch(r'[0-9][a-zA-Z0-9:.+~-]*', info['version']) or
                    not re.fullmatch(r'[0-9a-f]{64}', info['sha256']) or
                    info['architecture'] not in ('arm64', 'all')):
                raise ValueError('Invalid Debian overlay package record: ' + filename)
            packages[filename] = row
            names.add(name)
    return components, packages


def download_debian(rootfs, download, packages, run):
    """Use APT's archive authentication and compare to the producer's bytes."""
    account = run(['chroot', rootfs, 'getent', 'passwd', '_apt'],
                  capture_output=True, text=True).stdout.split(':')
    os.chown(download, int(account[2]), int(account[3]))
    guest = '/' + download.relative_to(rootfs).as_posix()
    # Restrict this download to the project source, including when the same
    # package version also exists in Debian. Dependencies use the normal list.
    run(['chroot', rootfs, 'sh', '-ec',
         'cd "$1"; shift; exec apt-get -o Dir::Etc::sourcelist=sources.list.d/sunuefi.sources '
         '-o Dir::Etc::sourceparts=- download "$@"',
         'sunuefi-package-download', guest,
         *[row['name'] + '=' + row['version'] for row in packages.values()]])
    expected = {(row['name'], row['version'], row['architecture']): row['sha256']
                for row in packages.values()}
    paths = []
    for archive in sorted(download.glob('*.deb')):
        identity = debian_identity(archive)
        if archive.is_symlink() or sha(archive) != expected.pop(identity, None):
            raise ValueError('Downloaded package differs from the published generation: ' + archive.name)
        paths.append(guest + '/' + archive.name)
    if expected:
        raise ValueError('Published APT source did not return the complete package set')
    return paths


def pacman_identity(archive):
    text = subprocess.check_output(['bsdtar', '-xOf', archive, '.PKGINFO'], text=True)
    fields = dict(line.split(' = ', 1) for line in text.splitlines() if ' = ' in line)
    return fields['pkgname'], fields['pkgver'], fields['arch']


def pacman_packages(rootfs, sources):
    """Collect exact package files from checked makepkg records, not cache globs."""
    installed = dict(line.split(maxsplit=1) for line in subprocess.check_output(
        ['chroot', rootfs, 'pacman', '-Q'], text=True).splitlines())
    result, names = {}, set()
    for component, (folder, rows) in sources.items():
        for filename, record in rows.items():
            path = folder / filename
            if (Path(filename).name != filename or not re.search(PACMAN_SUFFIX + '$', filename) or
                    path.is_symlink() or not path.is_file() or sha(path) != record['sha256']):
                raise ValueError('Invalid completed pacman package: ' + filename)
            name, version, architecture = pacman_identity(path)
            if installed.get(name) != version or architecture not in ('aarch64', 'any'):
                raise ValueError('Completed pacman package does not match the builder: ' + filename)
            if not overlay_release.uses_pacman_generation(version):
                raise ValueError('Rebuilt pacman package has no current publication revision: ' + filename)
            if filename in result or name in names:
                raise ValueError('Multiple components provide ' + name)
            result[filename] = (path, {'name': name, 'version': version, 'architecture': architecture,
                                      'sha256': record['sha256'], 'component': component})
            names.add(name)
    return result


def pacman_outputs(directory):
    record = json.loads((directory / 'manifest.json').read_text())
    if not record.get('packages') or (directory / '.incomplete').exists():
        raise ValueError('Use a completed pacman package directory: ' + str(directory))
    return directory, record['packages']


def read_pacman(path, target):
    manifest = json.loads(path.read_text())
    repo = ARCH_REPOSITORIES[target]
    if (manifest.get('schema_version') != 1 or manifest.get('target') != target or
            manifest.get('repo') != repo or not manifest.get('packages')):
        raise ValueError('Use a completed pacman overlay manifest for ' + target)
    names = set()
    for filename, row in manifest['packages'].items():
        name, version = row['name'], row['version']
        if (name in names or Path(filename).name != filename or not re.search(PACMAN_SUFFIX + '$', filename) or
                not re.fullmatch(r'[A-Za-z0-9@_+][A-Za-z0-9@_.+-]*', name) or
                not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.:+~-]*', version) or
                not re.fullmatch(r'[0-9a-f]{64}', row['sha256']) or
                row['architecture'] not in ('aarch64', 'any')):
            raise ValueError('Invalid pacman overlay package record: ' + filename)
        names.add(name)
    expected = {'libfprint', 'fprintd', 'ffmpeg', 'libwacom', 'libssc',
                'iio-sensor-proxy', 'fastrpc', 'pipewire-v4l2-source', 'gst-plugin-pipewire'}
    expected.update(('deckard-mesa-linux-aarch64', 'gamescope', 'steamos-manager', 'gtk3', 'ibus')
                    if target == 'steamos' else ('mesa', 'gdm', 'mutter', 'gnome-settings-daemon'))
    if not expected <= names:
        raise ValueError('Incomplete native overlay package set')
    return repo, manifest['packages'], manifest.get('components', {})


def download_pacman(rootfs, folder, repository, packages, guest):
    """Download from the named repository and verify every expected payload."""
    cache = '/' + folder.relative_to(rootfs).as_posix()
    targets = [repository + '/' + row['name'] + '=' + row['version'] for row in packages.values()]
    options = ['--noconfirm', '--cachedir', cache, *targets]
    guest(['pacman', '-Sw', *options])
    for name, row in packages.items():
        path = folder / name
        if path.is_symlink() or not path.is_file() or sha(path) != row['sha256']:
            raise ValueError('Downloaded pacman package differs from the published generation: ' + name)
    return options


def install_pacman(rootfs, folder, repository, packages, guest):
    """Only install from a verified cache downloaded through the named repo."""
    options = download_pacman(rootfs, folder, repository, packages, guest)
    # No sync refresh here: resolve against the same authenticated index as -Sw.
    guest(['pacman', '-S', *options])


def verify_pacman_versions(packages, listing):
    """Check the final package set after all image staging transactions."""
    installed = dict(line.split(maxsplit=1) for line in listing.splitlines())
    for row in packages.values():
        name, version = row['name'], row['version']
        if installed.get(name) != version:
            raise ValueError(f'Installed pacman package {name} must be {version}, '
                             f'found {installed.get(name, "not installed")}')


def pacman_arguments(parser):
    options = parser.add_mutually_exclusive_group()
    options.add_argument('--packages-only', type=Path, metavar='DIRECTORY',
                         help='Build and export overlay packages without assembling a product image')
    options.add_argument('--published-packages', type=Path, metavar='MANIFEST',
                         help='Install exact overlay packages from the signed SunUEFI pacman source')


def pacman_build_mode(args, target, project):
    """Keep the package builder separate from the generated product root."""
    output = args.output.resolve()
    package_output = None
    published = None
    if getattr(args, 'packages_only', None):
        package_output = args.packages_only.resolve()
        if (package_output.exists() or not package_output.is_relative_to(project / 'artifacts/packages') or
                package_output == project / 'artifacts/packages'):
            raise ValueError('Use a new project artifacts/packages directory for the overlay packages')
        if output == project / 'build/distros' / (target + '-piano'):
            output = project / 'build/distros/overlays' / target
        if output == project / 'build/distros/overlays' or not output.is_relative_to(project / 'build/distros/overlays'):
            raise ValueError('Use a separate build/distros/overlays directory for package builds')
    if getattr(args, 'published_packages', None):
        published = read_pacman(args.published_packages.resolve(), target)
    return output, package_output, published


def fingerprint_pacman_packages(rootfs, output, kernel_source, kernel_build, commit):
    from build_piano_fingerprint import install_desktop, prepare_desktop_backend
    from build_release_helpers import export_uapi

    with tempfile.TemporaryDirectory(prefix='sunuefi-fingerprint-', dir=output.parent) as temporary:
        work = Path(temporary)
        uapi = work / 'uapi'
        export_uapi(kernel_source, kernel_build, commit, uapi)
        prepare_desktop_backend(work, uapi)
        install_desktop(rootfs, work, output)
    return {name: pacman_outputs(output / name) for name in ('libfprint', 'fprintd')}


def install_arch_overlays(rootfs, kernel, target, published):
    from build_nyarch_apps import guest_environment
    from build_release_rootfs import build_resolver, target as guest_path
    from package.kernel import install
    from stage_arch_piano import install_repository

    repository, packages, components = published
    archive, bsp = shared_bsp('pacman')
    topology_archive, topology = shared_topology('pacman')
    check_topology_conflicts(rootfs, topology)
    install(rootfs, 'pacman', kernel, published=True)
    install_repository(rootfs, target == 'steamos')
    tmp = guest_path(rootfs, 'var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sunuefi-overlays-', dir=tmp) as temporary, \
            build_resolver(rootfs), guest_environment(rootfs) as guest:
        work = Path(temporary)
        work.chmod(0o755)
        guest(['pacman', '-Sy', '--noconfirm'])
        common = {archive.name: bsp, topology_archive.name: topology}
        if repository == 'sunuefi':
            packages = {**packages, **common}
        else:
            install_pacman(rootfs, work, 'sunuefi', common, guest)
        install_pacman(rootfs, work, repository, packages, guest)
    return {**components, 'device_configuration': bsp, 'audio_topology_package': topology}


def install_local_arch_bsp(rootfs):
    from build_nyarch_apps import guest_environment
    from build_release_rootfs import build_resolver, target as guest_path

    tmp = guest_path(rootfs, 'var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sunuefi-bsp-', dir=tmp) as temporary:
        work = Path(temporary)
        archive, bsp = build_bsp(work / 'package', 'pacman')
        topology_archive, topology = build_bsp(work / 'topology', 'pacman', component='audio-topology')
        check_topology_conflicts(rootfs, topology)
        with build_resolver(rootfs), guest_environment(rootfs) as guest:
            guest(['pacman', '-U', '--noconfirm', '/' + archive.relative_to(rootfs).as_posix(),
                   '/' + topology_archive.relative_to(rootfs).as_posix()])
    return bsp, topology
