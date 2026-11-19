"""Read Denial product manifests and verify native archives for publication."""
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import re
import subprocess

from build import denial, overlay_release
from build.overlay_packages import pacman_info, sha
from build_piano_runtime_helpers import source_patches
from desktop.debian_package import controls

ROOT = Path(__file__).resolve().parents[2]
TARGETS = {
    'debian-denial': ('debian', 'apt', 'arm64'),
    'arch-denial': ('pacman', 'sunuefi', 'aarch64'),
}
MAIN = 'denial'
PLUGIN = 'denial-plugin-manager'
ENGINE = 'denial-flutter-engine'
ENGINE_ABI = ENGINE + '-abi'
SHA256 = re.compile(r'[0-9a-f]{64}')


def digest(value):
    return isinstance(value, str) and SHA256.fullmatch(value) is not None


def archive_name(name, version, package_format, architecture):
    if package_format == 'debian':
        return f'{name}_{version}_{architecture}.deb'
    return f'{name}-{version}-{architecture}.pkg.tar.zst'


def rebuilt_version(package_format, previous, generation):
    if package_format == 'debian':
        return previous + (f'+sunuefi.{generation}' if generation else '+sunuefi1')
    version, pkgrel = previous.rsplit('-', 1)
    major, _, minor = pkgrel.partition('.')
    order = overlay_release.pacman_generation(generation) if generation else ''
    return f'{version}-{major}.{int(minor or 0) + 1}{order}'


def upstream_archives(record, package_format, architecture, tag):
    from build.denial import FINGERPRINT

    release = record.get('upstream_release')
    if (not isinstance(release, dict) or release.get('signing_key') != FINGERPRINT or
            not digest(release.get('sha256')) or not digest(release.get('signature_sha256'))):
        raise ValueError('Denial manifest has no verified upstream release')
    archives = release.get('archives')
    if not isinstance(archives, dict) or archives.keys() != {MAIN, PLUGIN, ENGINE}:
        raise ValueError('Denial manifest must record the three upstream packages')

    release_version = re.escape(tag.removeprefix('v')) + r'-[1-9][0-9]*(?:\.[0-9]+)?'
    for name, row in archives.items():
        if not isinstance(row, dict):
            raise ValueError('Invalid upstream Denial package: ' + name)
        version = row.get('version')
        epoch = r'(?:[0-9]+:)?' if package_format == 'debian' or name == ENGINE else ''
        if (not isinstance(version, str) or not re.fullmatch(epoch + release_version, version) or
                not digest(row.get('sha256')) or not digest(row.get('signature_sha256'))):
            raise ValueError('Invalid upstream Denial package: ' + name)
        filename = archive_name(name, version, package_format, architecture)
        expected = (filename,) if package_format == 'debian' else (filename, filename.replace(':', '.', 1))
        if row.get('file') not in expected:
            raise ValueError('Invalid upstream Denial package filename: ' + name)
    if archives[MAIN]['version'] != archives[PLUGIN]['version']:
        raise ValueError('Upstream Denial and the plugin manager must have the same version')
    return archives


def read_manifest(path, target):
    """Read a sealed package set; image assembly needs no local archives."""
    if target not in TARGETS:
        raise ValueError('Unknown Denial package target: ' + target)
    package_format, repository, architecture = TARGETS[target]
    path = Path(path)
    if path.is_symlink():
        raise ValueError('Use a regular Denial package manifest')
    record = json.loads(path.read_text())
    identity = {'schema_version': 1, 'target': target, 'repo': repository,
                'package_format': package_format, 'architecture': architecture}
    if not isinstance(record, dict) or any(record.get(key) != value for key, value in identity.items()):
        raise ValueError('Denial manifest does not match ' + target)

    source = json.loads((ROOT / 'config/denial.json').read_text())['source']
    tag = source['tag']
    series = ROOT / 'patches/denial' / tag.removeprefix('v') / 'series.json'
    patches = {patch.name: sha(patch) for patch in source_patches(series)}
    if (record.get('source') != source or record.get('patches') != patches or
            not digest(record.get('binary_sha256'))):
        raise ValueError('Denial manifest does not match the pinned source and patches')
    release = upstream_archives(record, package_format, architecture, tag)

    generation = record.get('overlay_release')
    if generation is not None and (not isinstance(generation, str) or
                                   not re.fullmatch(overlay_release.GENERATION, generation)):
        raise ValueError('Invalid Denial publication generation')
    version = rebuilt_version(package_format, release[MAIN]['version'], generation)
    expected = {archive_name(name, version, package_format, architecture): (name, version)
                for name in (MAIN, PLUGIN)}
    if package_format == 'pacman':
        engine = release[ENGINE]
        expected[engine['file']] = (ENGINE, engine['version'])
    packages = record.get('packages')
    if not isinstance(packages, dict) or packages.keys() != expected.keys():
        raise ValueError('Denial manifest must contain the complete native package set')
    for filename, (name, version) in expected.items():
        row = packages[filename]
        if (not isinstance(row, dict) or (row.get('name'), row.get('version'), row.get('architecture')) !=
                (name, version, architecture) or not digest(row.get('sha256'))):
            raise ValueError('Invalid Denial package record: ' + filename)
        if name == ENGINE and row['sha256'] != release[ENGINE]['sha256']:
            raise ValueError('The published Engine must use the unmodified upstream archive')
    return record


def debian_fields(archive, row):
    fields, = controls(subprocess.check_output(['dpkg-deb', '-f', archive], text=True))
    if tuple(fields.get(key) for key in ('Package', 'Version', 'Architecture')) != (
            row['name'], row['version'], row['architecture']):
        raise ValueError('Unexpected Debian metadata in ' + archive.name)
    return fields


def exact_debian_dependency(value, name, version):
    relation = re.escape(name) + r'\s*\(\s*=\s*' + re.escape(version) + r'\s*\)'
    return any(re.fullmatch(relation, dependency.strip()) for dependency in value.split(','))


def verify_debian(fields, version, engine_version):
    for package, control, dependency, required in (
            (MAIN, 'Depends', ENGINE, engine_version),
            (MAIN, 'Suggests', PLUGIN, version),
            (PLUGIN, 'Depends', MAIN, version)):
        if not exact_debian_dependency(fields[package].get(control, ''), dependency, required):
            raise ValueError(f'{package} must declare {control}: {dependency} (= {required})')


def pacman_fields(archive, row):
    fields = pacman_info(archive)
    if tuple(fields.get(key) for key in ('pkgname', 'pkgver', 'arch')) != (
            [row['name']], [row['version']], [row['architecture']]):
        raise ValueError('Unexpected pacman metadata in ' + archive.name)
    return fields


def verify_pacman(fields, version):
    provided = [value for value in fields[ENGINE].get('provides', ())
                if value.startswith(ENGINE_ABI + '=')]
    depends = fields[MAIN].get('depend', ())
    if (len(provided) != 1 or not provided[0].partition('=')[2] or
            provided[0] not in depends or ENGINE not in depends):
        raise ValueError('Denial and the upstream Engine must use the same Flutter Engine ABI')
    if f'{MAIN}={version}' not in fields[PLUGIN].get('depend', ()):
        raise ValueError(f'{PLUGIN} must depend on {MAIN}={version}')


def verify_engine(directory, engine):
    from desktop.denial_packages import release_keyring, verify_signature

    archive = directory / engine['file']
    signature = directory / (archive.name + '.sig')
    if signature.is_symlink() or not signature.is_file() or sha(signature) != engine['signature_sha256']:
        raise ValueError('Denial Engine signature differs from the upstream release')
    with release_keyring() as keyring:
        verify_signature(keyring, signature, archive)


def compositor_digest(archive, debian):
    """Hash the packaged program without extracting the desktop payload."""
    path = denial.COMPOSITOR.lstrip('/')
    with ExitStack() as stack:
        if debian:
            data = stack.enter_context(subprocess.Popen(
                ['dpkg-deb', '--fsys-tarfile', archive], stdout=subprocess.PIPE))
            command, source = ['tar', '-xOf', '-', './' + path], data.stdout
        else:
            command, source = ['bsdtar', '-xOf', archive, path], None
        program = stack.enter_context(subprocess.Popen(command, stdin=source, stdout=subprocess.PIPE))
        if source is not None:
            source.close()
        digest = hashlib.file_digest(program.stdout, 'sha256').hexdigest()
        if program.wait() or (debian and data.wait()):
            raise ValueError('Cannot read the Denial compositor in ' + archive.name)
    return digest


def verify_archives(directory, target):
    """Verify the complete publication input, including original Engine bytes."""
    directory = Path(directory)
    if directory.is_symlink() or (directory / '.incomplete').exists():
        raise ValueError('Denial package generation has not completed: ' + str(directory))
    record = read_manifest(directory / 'manifest.json', target)
    engine = record['upstream_release']['archives'][ENGINE]
    packages = record['packages']
    debian = record['package_format'] == 'debian'
    expected = {*packages, 'manifest.json'}
    if not debian:
        expected.add(engine['file'] + '.sig')
    if {path.name for path in directory.iterdir()} != expected:
        raise ValueError('Denial directory does not contain exactly the recorded packages and signatures')

    fields, paths = {}, {}
    read_fields = debian_fields if debian else pacman_fields
    for filename, row in packages.items():
        archive = directory / filename
        if archive.is_symlink() or not archive.is_file() or sha(archive) != row['sha256']:
            raise ValueError('Denial archive differs from the package manifest: ' + filename)
        fields[row['name']] = read_fields(archive, row)
        if row['name'] == MAIN and compositor_digest(archive, debian) != record['binary_sha256']:
            raise ValueError('Packaged Denial compositor differs from the build record')
        paths[filename] = (archive, row['sha256'])
    if debian:
        verify_debian(fields, fields[MAIN]['Version'], engine['version'])
    else:
        verify_engine(directory, engine)
        verify_pacman(fields, fields[MAIN]['pkgver'][0])
    return paths
