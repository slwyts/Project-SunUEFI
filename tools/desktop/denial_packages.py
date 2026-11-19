"""Package the built Denial compositor with the signed upstream release payloads."""
from contextlib import contextmanager, nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from build import overlay_packages, overlay_release
from build.denial import FINGERPRINT, KEY
from build.denial_manifest import ENGINE, MAIN, PLUGIN, TARGETS
from build_piano_runtime_helpers import source_patches
from desktop import package_build
from desktop.debian_package import controls

ROOT = Path(__file__).resolve().parents[2]
PROGRAM = 'usr/bin/deniald'
ARCHITECTURES = {'debian': 'arm64', 'pacman': 'aarch64'}
CHECKSUMS = 'SHA256SUMS'


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def capture(args):
    return subprocess.check_output(args, text=True)


def check_binary(binary):
    elf = capture(['readelf', '-h', '-d', '-W', binary])
    if ('AArch64' not in elf or 'ELF64' not in elf or '[libseat.so.1]' not in elf or
            any(field in elf for field in ('(RPATH)', '(RUNPATH)'))):
        raise ValueError('Expected an ARM64 deniald linked to the system libseat')


def build_source(binary, receipt):
    record = json.loads(receipt.read_text())
    policy = json.loads((ROOT / 'config/denial.json').read_text())['source']
    version = policy['tag'].removeprefix('v')
    patches = source_patches(ROOT / 'patches/denial' / version / 'series.json')
    if (record.get('source') != policy or
            record.get('patches') != {patch.name: package_build.sha(patch) for patch in patches} or
            record.get('binary_sha256') != package_build.sha(binary)):
        raise ValueError('deniald does not match the build record and selected sources')
    check_binary(binary)
    return record, version


def verify_signature(keyring, signature, signed):
    """Verify the release signer and return the signature creation time."""
    result = subprocess.run(
        ['gpgv', '--homedir', keyring.parent, '--keyring', keyring,
         '--status-fd', '1', signature, signed], capture_output=True, text=True)
    valid = [line.split() for line in result.stdout.splitlines()
             if line.startswith('[GNUPG:] VALIDSIG ')]
    if result.returncode or [fields[-1] for fields in valid] != [FINGERPRINT]:
        raise ValueError(f'Invalid Denial release signature: {signed.name}')
    return int(valid[0][4])


@contextmanager
def release_keyring():
    with tempfile.TemporaryDirectory(prefix='denial-release-keys-') as home:
        keyring = Path(home) / 'denial.gpg'
        run(['gpg', '--no-options', '--homedir', home, '--batch', '--dearmor',
             '--output', keyring, KEY])
        yield keyring


def release_checksums(directory, keyring):
    checksums = directory / CHECKSUMS
    signature = checksums.with_name(checksums.name + '.sig')
    epoch = verify_signature(keyring, signature, checksums)
    digests = {}
    for line in checksums.read_text().splitlines():
        if match := re.fullmatch(r'([a-f0-9]{64}) [ *](.+)', line):
            digest, name = match.groups()
            digests.setdefault(Path(name).name, set()).add(digest)
    release = {'sha256': package_build.sha(checksums), 'signature_sha256': package_build.sha(signature),
               'signing_key': FINGERPRINT, 'source_date_epoch': epoch}
    return digests, release


def release_archives(digests, package_format, version):
    debian = package_format == 'debian'
    architecture = ARCHITECTURES[package_format]
    separator, suffix = ('_', r'_arm64\.deb') if debian else ('-', r'-aarch64\.pkg\.tar\.zst')
    archives = {}
    for name in (MAIN, PLUGIN, ENGINE):
        pattern = re.escape(name + separator) + r'(?:[0-9]+:)?'
        pattern += re.escape(version) + r'-[0-9]+(?:\.[0-9]+)?' + suffix
        filenames = [filename for filename in digests if re.fullmatch(pattern, filename)]
        if len(filenames) != 1:
            raise ValueError(f'Expected one signed {name} {architecture} release archive')
        filename, = filenames
        files = (filename, filename + '.sig')
        if any(len(digests.get(file, ())) != 1 for file in files):
            raise ValueError(f'Expected one signed digest for {filename} and its signature')
        archives[name] = {file: next(iter(digests[file])) for file in files}
    return archives


def asset_name(filename):
    # GitHub release asset names replace the pacman epoch separator.
    return filename.replace(':', '.', 1)


def signed_archives(directory, package_format, version, keyring):
    digests, release = release_checksums(directory, keyring)
    selected = release_archives(digests, package_format, version)
    debian = package_format == 'debian'
    architecture = ARCHITECTURES[package_format]
    identity = overlay_packages.debian_identity if debian else overlay_packages.pacman_identity
    archives, inputs = {}, {}
    for name, files in selected.items():
        filename, signature = files
        archive = directory / filename
        if not archive.is_file():
            archive = directory / asset_name(filename)
        sig = archive.with_name(archive.name + '.sig')
        if (package_build.sha(archive) != files[filename] or
                package_build.sha(sig) != files[signature]):
            raise ValueError(f'Denial archive differs from signed {CHECKSUMS}: {archive.name}')
        verify_signature(keyring, sig, archive)
        pkgname, pkgver, arch = identity(archive)
        expected = (f'{name}_{pkgver}_{architecture}.deb' if debian
                    else f'{name}-{pkgver}-{architecture}.pkg.tar.zst')
        if (pkgname, arch, filename) != (name, architecture, expected):
            raise ValueError(f'Unexpected metadata in {archive.name}')
        archives[name] = archive
        inputs[name] = {'file': archive.name, 'version': pkgver, 'sha256': files[filename],
                        'signature_sha256': files[signature]}
    if inputs[MAIN]['version'] != inputs[PLUGIN]['version']:
        raise ValueError('Denial and its plugin manager must come from the same release')
    release['archives'] = inputs
    return archives, release


def replace_field(text, field, change):
    pattern = rf'(?m)^{field}:([^\n]*(?:\n[ \t][^\n]*)*)'
    result, count = re.subn(pattern, lambda match: field + ':' + change(match[1]), text)
    if count != 1:
        raise ValueError(f'Expected one Debian package control field: {field}')
    return result


def exact_dependency(name, version):
    return rf'(?<![\w.+-])({re.escape(name)}\s*\(\s*=\s*){re.escape(version)}(?=\s*\))'


def replace_dependency(field, name, previous, version):
    result, count = re.subn(exact_dependency(name, previous),
                            lambda match: match[1] + version, field)
    if count != 1:
        raise ValueError(f'Expected an exact dependency on {name} {previous}')
    return result


def install_program(payload, binary):
    target = payload / PROGRAM
    if target.is_symlink() or not target.is_file():
        raise ValueError('The original Denial package is missing its compositor')
    shutil.copyfile(binary, target)


def update_md5sums(root):
    sums = root / 'DEBIAN/md5sums'
    if sums.is_file():
        with (root / PROGRAM).open('rb') as stream:
            digest = hashlib.file_digest(stream, lambda: hashlib.md5(usedforsecurity=False)).hexdigest()
        text = re.sub(rf'(?m)^[a-f0-9]{{32}}(?= +{re.escape(PROGRAM)}$)',
                      digest, sums.read_text())
        sums.write_text(text)


def build_debian(binary, archives, inputs, output, work, epoch):
    previous = inputs[MAIN]['version']
    version = overlay_release.debian_version(previous)
    if version == previous:
        version += '+sunuefi1'
    environment = {**os.environ, 'SOURCE_DATE_EPOCH': str(epoch)}
    for name in (MAIN, PLUGIN):
        tree = work / name
        run(['dpkg-deb', '--raw-extract', archives[name], tree])
        control = tree / 'DEBIAN/control'
        text = control.read_text()
        if name == MAIN:
            depends = controls(text)[0].get('Depends', '')
            if not re.search(exact_dependency(ENGINE, inputs[ENGINE]['version']), depends):
                raise ValueError('The Denial package must retain its exact upstream Engine dependency')
            install_program(tree, binary)
            update_md5sums(tree)
        field, dependency = ('Suggests', PLUGIN) if name == MAIN else ('Depends', MAIN)
        text = replace_field(text, field, lambda value: replace_dependency(
            value, dependency, previous, version))
        size = capture(['du', '-sk', '--apparent-size', '--exclude=DEBIAN', tree]).split()[0]
        text = replace_field(text, 'Installed-Size', lambda _: ' ' + size)
        text = replace_field(text, 'Version', lambda _: ' ' + version)
        control.write_text(text)
        run(['dpkg-deb', '--root-owner-group', '-Zxz', '-z3', '--threads-max=2', '--build', tree,
             output / f'{name}_{version}_arm64.deb'], env=environment)
    return version


def run_makepkg(guest, stage, folder, scratch, recipe, settings):
    recipe = f'{folder}/packaging/{recipe}'
    command = ['env', '-i', *package_build.arguments(settings), 'bash', '-ec',
               'cd "$1"; shift; exec makepkg --noconfirm --nocheck --nodeps "$@"',
               'makepkg', recipe]
    # Concurrent makepkg runs must not share fakeroot's SysV queues.
    if guest is not None:
        guest(['chown', 'nobody:nobody', f'{folder}/{scratch}'])
        guest(['unshare', '--ipc', '--', 'runuser', '-u', 'nobody', '--', *command])
    else:
        # As in the kernel and BSP builders, makepkg only assembles the
        # finished ARM64 payload; no host library is linked into it.
        from package_bsp import pacman_builder_account

        config = stage / 'makepkg.conf'
        config.write_text('. /etc/makepkg.conf\nCARCH=aarch64\n')
        command.extend(('--config', str(config)))
        if account := pacman_builder_account():
            os.chown(stage / scratch, account.pw_uid, account.pw_gid)
            command = ['unshare', '--ipc', '--', 'runuser', '-u', account.pw_name, '--', *command]
        run(command)


def build_pacman(binary, archives, inputs, output, version, source, root, policy):
    from build_nyarch_apps import guest_environment
    from desktop.build_denial import verify_source

    if root is not None and (os.geteuid() or root == Path('/') or not (root / 'usr/bin/makepkg').is_file()):
        raise ValueError('Use root and a prepared ARM64 makepkg build root')
    # GnuPG uses Unix sockets whose paths cannot be as long as package outputs.
    with tempfile.TemporaryDirectory(prefix='denial-keys-') as key_home:
        verify_source(source, policy, Path(key_home))
    previous = inputs[MAIN]['version']
    pkgver, pkgrel = previous.rsplit('-', 1)
    if pkgver != version:
        raise ValueError('The pacman package version must match the signed Denial tag')
    release = overlay_release.pacman_release(pkgrel)
    if release == pkgrel:
        major, _, minor = pkgrel.partition('.')
        release = f'{major}.{int(minor or 0) + 1}'
    pkgversion = pkgver + '-' + release
    epoch = capture(['git', '-C', source, 'log', '-1', '--format=%ct', policy['commit']]).strip()
    staging_dir = root / 'var/tmp' if root is not None else '/tmp'
    with tempfile.TemporaryDirectory(prefix='sunuefi-denial-packages-', dir=staging_dir) as stage:
        stage = Path(stage)
        stage.chmod(0o755)
        folder = '/' + stage.relative_to(root).as_posix() if root is not None else str(stage)
        recipes = subprocess.check_output(
            ['git', '-C', source, 'archive', policy['commit'], 'packaging'])
        run(['bsdtar', '-xf', '-', '-C', stage], input=recipes)
        for name in (MAIN, PLUGIN):
            payload = stage / name
            payload.mkdir()
            run(['bsdtar', '-xpf', archives[name], '-C', payload,
                 '--exclude=.PKGINFO', '--exclude=.MTREE', '--exclude=.BUILDINFO',
                 '--exclude=.INSTALL', '--exclude=.CHANGELOG'])
            if name == MAIN:
                install_program(payload, binary)
        with (guest_environment(root) if root is not None else nullcontext()) as guest:
            for name, recipe in ((MAIN, 'arch'), (PLUGIN, 'arch/plugin-manager')):
                scratch = 'makepkg-' + name
                (stage / scratch).mkdir()
                settings = {
                    'PATH': '/usr/bin:/bin', 'LC_ALL': 'C', 'CARCH': 'aarch64',
                    'HOME': f'{folder}/{scratch}', 'SOURCE_DATE_EPOCH': epoch,
                    'DENIAL_PACKAGE_VERSION': pkgver, 'DENIAL_PACKAGE_RELEASE': release,
                    'DENIAL_PACKAGE_PAYLOAD_ROOT': f'{folder}/{name}',
                    'PKGDEST': f'{folder}/{scratch}', 'BUILDDIR': f'{folder}/{scratch}',
                    'SRCDEST': f'{folder}/{scratch}', 'PKGEXT': '.pkg.tar.zst',
                }
                run_makepkg(guest, stage, folder, scratch, recipe, settings)
                basename = f'{name}-{pkgversion}-aarch64.pkg.tar.zst'
                archive = stage / scratch / basename
                before = overlay_packages.pacman_info(archives[name])
                after = overlay_packages.pacman_info(archive)
                depends = before.get('depend', [])
                if name == PLUGIN:
                    if f'{MAIN}={previous}' not in depends:
                        raise ValueError('The upstream plugin manager has no exact Denial dependency')
                    depends = [f'{MAIN}={pkgversion}' if dep == f'{MAIN}={previous}' else dep
                               for dep in depends]
                if after.get('depend', []) != depends or after.get('backup') != before.get('backup'):
                    raise ValueError(f'The native package changed {name} dependencies or conffiles')
                shutil.copyfile(archive, output / basename)
    return pkgversion


def add_engine(runtime, archive, metadata):
    sig = archive.with_name(archive.name + '.sig')
    for path, digest in ((archive, metadata['sha256']), (sig, metadata['signature_sha256'])):
        staged = runtime / path.name
        shutil.copyfile(path, staged)
        if package_build.sha(staged) != digest:
            raise ValueError(f'The original Engine archive or signature changed: {path.name}')
    name, version, arch = overlay_packages.pacman_identity(runtime / archive.name)
    return {'name': name, 'version': version, 'architecture': arch, 'sha256': metadata['sha256']}


def build(binary, receipt, upstream, output, package_format, *, source=None, arch_root=None, target=None):
    """Build the native Denial package set without installing it."""
    if package_format not in ARCHITECTURES:
        raise ValueError('Choose the debian or pacman package format')
    if target is not None and TARGETS.get(target, (None,))[0] != package_format:
        raise ValueError('The Denial overlay target and package format differ')
    binary, receipt, upstream, output = map(lambda path: Path(path).resolve(),
                                            (binary, receipt, upstream, output))
    if package_format == 'pacman' and source is None:
        raise ValueError('Pacman packaging requires the signed Denial source checkout')
    if output.exists():
        raise FileExistsError('Use a new package output directory: ' + str(output))
    record, version = build_source(binary, receipt)
    output.parent.mkdir(parents=True, exist_ok=True)
    with (release_keyring() as keyring,
          tempfile.TemporaryDirectory(prefix='denial-packages-', dir=output.parent) as work):
        work = Path(work)
        archives, release = signed_archives(upstream, package_format, version, keyring)
        runtime = work / 'runtime'
        runtime.mkdir()
        if package_format == 'debian':
            revision = build_debian(binary, archives, release['archives'], runtime, work,
                                    release['source_date_epoch'])
            identity = overlay_packages.debian_identity
        else:
            revision = build_pacman(binary, archives, release['archives'], runtime, version,
                                    Path(source).resolve(), Path(arch_root).resolve() if arch_root else None,
                                    record['source'])
            identity = overlay_packages.pacman_identity
        packages, names = {}, set()
        architecture = ARCHITECTURES[package_format]
        for archive in sorted(runtime.iterdir()):
            name, version, arch = identity(archive)
            if name in names or name not in (MAIN, PLUGIN) or (version, arch) != (revision, architecture):
                raise ValueError(f'Unexpected native Denial package: {archive.name}')
            names.add(name)
            packages[archive.name] = {'name': name, 'version': version, 'architecture': arch,
                                      'sha256': package_build.sha(archive)}
        if names != {MAIN, PLUGIN}:
            raise ValueError('Expected both the Denial and plugin manager archives')
        if target == 'arch-denial':
            engine = archives[ENGINE]
            packages[engine.name] = add_engine(runtime, engine, release['archives'][ENGINE])
        record.update(schema_version=1, package_format=package_format, architecture=architecture,
                      overlay_release=overlay_release.generation(), upstream_release=release,
                      packages=packages)
        if target is not None:
            record.update(target=target, repo=TARGETS[target][1])
        (runtime / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
        runtime.rename(output)
    return record
