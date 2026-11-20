"""Build the Debian libfprint package set with the Piano Goodix backend."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

from build import debian, overlay_release
from build_piano_fingerprint import LICENSES, backend_sources, patches, source, source_inputs
from desktop import debian_package as signed, package_build

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / 'config/fingerprint.json'
PACKAGE = 'libfprint'
LIBRARY = 'libfprint-2-2'
RUNTIME = {
    LIBRARY: 'arm64',
    'gir1.2-fprint-2.0': 'arm64',
    'libfprint-2-dev': 'arm64',
    'libfprint-2-doc': 'all',
    'libfprint-2-tests': 'arm64',
}
TRANSFER_PACKAGES = frozenset(('gir1.2-fprint-2.0', 'libfprint-2-dev'))


def policy():
    return json.loads(CONFIG.read_text())


def base_package_version(data):
    # Keep the local revision in the upstream portion, after the old +sunuefi2.
    cfg = data['debian']
    epoch = cfg['recipe_source_version'].split(':', 1)[0]
    return f"{epoch}:{data['desktop'][PACKAGE]['version']}+{cfg['package_revision']}"


def verify_debian_packages(record):
    version = record.get('package_version', '')
    packages = record.get('packages', {})
    if (overlay_release.base_debian_version(version) != base_package_version(policy()) or
            set(packages) != set(RUNTIME) or
            any(row['version'] != version or row['architecture'] != RUNTIME[name]
                for name, row in packages.items())):
        raise ValueError('Debian libfprint needs all five native packages at the same version')


def backend_patch(directory, backend):
    """Record the vendor source addition outside the original tar."""
    empty = directory / 'empty'
    empty.mkdir()
    # Git control metadata is not part of the backend source package.
    (backend / 'qcbor/.gitignore').unlink(missing_ok=True)
    result = subprocess.run(['diff', '-ruN', empty, backend], capture_output=True, text=True)
    if result.returncode != 1 or result.stderr:
        raise ValueError('Cannot generate the Piano Goodix source patch')
    patch = directory / 'piano-goodix-backend.patch'
    patch.write_text(
        'Description: Add the Piano Goodix fingerprint backend\n'
        'Origin: vendor, https://github.com/slwyts/Project-SunUEFI\n'
        'Forwarded: no\n\n')
    prefix = str(backend) + '/'
    with patch.open('a') as stream:
        headers = 0
        for line in result.stdout.splitlines(keepends=True):
            if line.startswith('diff '):
                headers = 2
                continue
            if headers == 2:
                if not line.startswith('--- '):
                    raise ValueError('Unexpected Piano Goodix source patch header')
                line = '--- /dev/null\n'
            elif headers == 1:
                if not line.startswith('+++ ' + prefix):
                    raise ValueError('Unexpected Piano Goodix source path')
                relative = line.split('\t', 1)[0].removeprefix('+++ ' + prefix)
                line = '+++ b/libfprint/drivers/piano-goodix/' + relative + '\n'
            headers = max(headers - 1, 0)
            stream.write(line)
    return patch


def add_file_transfers(control, version):
    """Order the transfer of typelib and development files from the old runtime."""
    relation = f'{LIBRARY} (<< {version})'
    sections = control.read_text().split('\n\n')
    moved = set()
    for index, section in enumerate(sections):
        fields = debian.controls(section)
        if not fields or fields[0].get('Package') not in TRANSFER_PACKAGES:
            continue
        if any(field in fields[0] for field in ('Breaks', 'Replaces')) or '\nDescription:' not in section:
            raise ValueError('Check the updated Debian libfprint file transfer relationships')
        sections[index] = section.replace('\nDescription:',
                                          f'\nBreaks: {relation}\nReplaces: {relation}\nDescription:', 1)
        moved.add(fields[0]['Package'])
    if moved != TRANSFER_PACKAGES:
        raise ValueError('The Debian libfprint recipe no longer has the expected file owners')
    control.write_text('\n\n'.join(sections))


def prepare_source(directory, original, backend, cfg, local_patches, version, transfer_version):
    upstream = source(PACKAGE, cfg)
    upstream_version = version.split(':', 1)[1].rsplit('-', 1)[0]
    work = directory / f'{PACKAGE}-{upstream_version}'
    archive = directory / f'{PACKAGE}_{upstream_version}.orig.tar.gz'
    debian.run(['git', '-C', upstream, 'archive', '--format=tar.gz',
                '--prefix=' + work.name + '/', '-o', archive, cfg['commit']])
    with tarfile.open(archive) as tar:
        tar.extractall(directory, filter='data')
    packaging = work / 'debian'
    shutil.copytree(original / 'debian', packaging)
    add_file_transfers(packaging / 'control', transfer_version)
    staged = directory / 'backend'
    staged.mkdir()
    with tarfile.open(backend) as tar:
        tar.extractall(staged, filter='data')
    licenses = {name: name for name in LICENSES}
    licenses.update({'QCBOR.txt': 'qcbor/LICENSE', 'linux-tee.h': 'uapi/linux/tee.h'})
    manifest = packaging / (LIBRARY + '.docs')
    with manifest.open('a') as docs:
        for name, path in licenses.items():
            filename = 'piano-goodix-' + name
            shutil.copy2(staged / path, packaging / filename)
            docs.write('debian/' + filename + '\n')
    debian.add_patches(work, [*local_patches, backend_patch(directory, staged)], PACKAGE,
                      version, 'Build the Piano Goodix backend in the Debian libfprint package set.')
    return work


def reusable(output, target, patch_proof, version, inputs):
    record = debian.reusable(output, version, patch_proof, RUNTIME, target)
    if not record or record.get('inputs') != inputs:
        return None
    try:
        verify_debian_packages(record)
    except (KeyError, TypeError, ValueError):
        return None
    return record


def build(output, sysroot, runtime, jobs=None):
    output, sysroot = Path(output).resolve(), Path(sysroot).resolve()
    jobs = package_build.default_jobs() if jobs is None else jobs
    if (os.geteuid() or not output.is_relative_to(ROOT / 'build') or
            not sysroot.is_relative_to(ROOT / 'build') or jobs < 1 or
            output.is_relative_to(sysroot) or sysroot.is_relative_to(output)):
        raise ValueError('Use root, separate project build directories, and at least one build job')
    data = policy()
    cfg = data['desktop'][PACKAGE]
    recipe_version = data['debian']['recipe_source_version']
    transfer_version = data['debian']['file_transfer_version']
    version = overlay_release.debian_version(base_package_version(data))
    debian.run(['dpkg', '--compare-versions', version, 'gt', recipe_version])
    debian.run(['dpkg', '--compare-versions', version, 'ge', transfer_version])
    local_patches = patches(cfg)
    patch_proof = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': sha}
                   for path, sha in local_patches]
    input_generation = source_inputs()
    backend = backend_sources(Path(runtime))
    inputs = {'backend_sources_sha256': signed.sha(backend), 'fingerprint': input_generation}
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        signed.verify_build_archive(sysroot)
        target = debian.target_identity(sysroot, {PACKAGE: recipe_version}, {PACKAGE: LIBRARY})
        if record := reusable(output, target, patch_proof, version, inputs):
            print('Reusing Debian libfprint packages with unchanged sources and ABI', flush=True)
            return record
        archive = signed.archive_inputs(sysroot)
        return build_packages(output, sysroot, cfg, recipe_version, transfer_version,
                              backend, local_patches, version,
                              patch_proof, target, inputs, archive, jobs)


def build_packages(output, sysroot, cfg, recipe_version, transfer_version, backend, patches, version,
                   patch_proof, target, inputs, archive, jobs):
    incomplete = output / '.incomplete'
    incomplete.touch()
    with (signed.use_build_archive(archive), debian.builder(output, sysroot, overlay=True) as (root, env),
          (output / 'build.log').open('a') as log):
        signed.verify_build_archive(root)
        signed.chroot(root, ['apt-get', 'install', '-y', '--no-install-recommends',
                            'dpkg-dev', 'build-essential', 'ccache'],
                      stdout=log, stderr=subprocess.STDOUT)
        signed.install_build_dependencies(root, PACKAGE, recipe_version, log)
        original, origin = debian.source(root, output, PACKAGE, recipe_version, log)
        with tempfile.TemporaryDirectory(prefix='package-', dir=output) as temporary:
            directory = Path(temporary)
            work = prepare_source(directory, original, backend, cfg, [path for path, _ in patches],
                                  version, transfer_version)
            debian.command(root, {}, work, ['dpkg-source', '-b', '.'], log)
            debian.build(root, env, work, jobs, log)
            packages = debian.collect(output, directory, RUNTIME, version)
            receipts = output / 'source'
            receipts.mkdir(exist_ok=True)
            for path in directory.iterdir():
                if path.is_file() and (path.suffix in ('.dsc', '.changes', '.buildinfo') or '.tar.' in path.name):
                    shutil.copy2(path, receipts / path.name)
        runtime_dir = output / 'runtime'
        files = {row['file'] for row in packages.values()}
        for path in runtime_dir.glob('*.deb'):
            if path.name not in files:
                path.unlink()
        record = {'package_version': version, 'packages_built': True, 'packages': packages,
                  'target': dict(target), 'inputs': inputs, 'patches': patch_proof, 'source': origin,
                  'producer': Path(__file__).relative_to(ROOT).as_posix(),
                  'producer_sha256': signed.sha(__file__),
                  'compiler_environment_sha256': signed.sha(package_build.__file__),
                  'recipe': 'original Debian debian/rules and control',
                  'build_packages': debian.build_packages(root),
                  'runtime_dependencies': debian.runtime_dependencies(output, packages)}
        if source_inputs() != inputs['fingerprint'] or signed.sha(backend) != inputs['backend_sources_sha256']:
            raise ValueError('Fingerprint sources changed during the package build')
        verify_debian_packages(record)
    debian.write_manifest(output, record)
    incomplete.unlink()
    return record
