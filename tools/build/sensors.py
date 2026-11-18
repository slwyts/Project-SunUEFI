#!/usr/bin/env python3
"""Build the Debian sensor packages or the shared Arch device package."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import debian, overlay_release
from desktop import package_build

RUNTIME = {'fastrpc-support', 'libfastrpc1', 'libssc2', 'libssc-bin',
           'iio-sensor-proxy', 'piano-sensors'}
POLICY = ROOT / 'patches/piano-sensors/5/build.json'
ARCH_PACKAGE = 'piano-sensors'
SSC_DEPENDENCY = 'iio-sensor-proxy-ssc>=3.9'
ARCH_RUNTIME = {
    'usr/lib/piano-sensors/piano-sensors-import',
    'usr/lib/piano-sensors/piano-sensors-lp-table',
    'usr/lib/piano-sensors/piano-sensors-wait',
    'usr/lib/systemd/system/adsprpcd-sensorspd.service',
    'usr/lib/systemd/system/adsprpcd.service.d/60-piano.conf',
    'usr/lib/systemd/system/adsprpcd_audiopd.service.d/60-piano.conf',
    'usr/lib/systemd/system/iio-sensor-proxy.service.d/piano.conf',
    'usr/lib/systemd/system/piano-sensors-import.service',
    'usr/lib/udev/rules.d/81-piano-sensors.rules',
    'usr/share/doc/piano-sensors/source.json',
    'usr/share/licenses/piano-sensors/LICENSE',
}


def package_versions(policy):
    selected = policy['source_versions']
    libssc = overlay_release.debian_version(selected['libssc'] + '+' + policy['local_revisions']['libssc'])
    sensor_proxy = overlay_release.debian_version(selected['iio-sensor-proxy'] + '+' +
                                                  policy['local_revisions']['iio-sensor-proxy'])
    return {'piano-sensors': overlay_release.debian_version(policy['native_version']),
            'libssc2': libssc, 'libssc-bin': libssc, 'iio-sensor-proxy': sensor_proxy,
            # Both FastRPC archives are downloaded unchanged from signed sid.
            'fastrpc-support': selected['fastrpc'], 'libfastrpc1': selected['fastrpc']}


def source_workspace(workspace, config):
    source = ROOT / config['source']
    head = debian.capture(['git', '-C', source, 'rev-parse', 'HEAD'])
    dirty = debian.capture(['git', '-C', source, 'status', '--porcelain'])
    if head != config['commit'] or dirty:
        raise ValueError('Sensors source HEAD/cleanliness mismatch; run ./build.sh sources')
    patches = [{'path': name, 'sha256': package_build.sha(ROOT / name)} for name in config['patches']]
    record = {'source_url': config['source_url'], 'source_commit': head, 'patches': patches}
    if workspace.exists():
        if json.loads((workspace / 'SOURCE').read_text()) != record:
            raise ValueError('Sensors source changed; use a fresh output directory')
    else:
        shutil.copytree(source, workspace, ignore=shutil.ignore_patterns('.git'))
        for patch in patches:
            debian.run(['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', ROOT / patch['path']],
                       cwd=workspace)
        (workspace / 'SOURCE').write_text(json.dumps(record, indent=2) + '\n')
    return workspace, record


def build_pacman(output):
    from package_bsp import makepkg

    output = output.resolve()
    if output.exists():
        raise ValueError('Use a fresh directory for the shared Arch sensor package')
    config = json.loads((ROOT / 'config/release.json').read_text())['sensors']
    version = json.loads(POLICY.read_text())['native_version']
    if not re.fullmatch(r'[0-9][a-zA-Z0-9.+_]*', version):
        raise ValueError('The shared sensor package has an invalid version')
    with tempfile.TemporaryDirectory(prefix='sunuefi-sensors-') as folder:
        stage = Path(folder)
        source, provenance = source_workspace(stage / 'source', config)
        work = stage / 'recipe'
        work.mkdir()
        archive = work / 'piano-sensors.tar'
        debian.run(['tar', '--sort=name', '--numeric-owner', '--owner=0', '--group=0',
                    '--mtime=@0', '--format=ustar', '-C', source, '-cf', archive,
                    'LICENSE', 'SOURCE', 'piano-sensors'])
        recipe = (ROOT / 'linux/bsp/arch/piano-sensors/PKGBUILD').read_text()
        recipe = recipe.replace('@VERSION@', version).replace('@SSC_DEPENDENCY@', SSC_DEPENDENCY)
        recipe = recipe.replace('@SOURCE_SHA256@', package_build.sha(archive))
        (work / 'PKGBUILD').write_text(overlay_release.pacman_recipe(recipe))
        # The recipe directory must be reachable by the unprivileged builder.
        stage.chmod(0o755)
        makepkg(work)
        release = version + '-' + overlay_release.pacman_release('1')
        filename = f'{ARCH_PACKAGE}-{release}-any.pkg.tar.zst'
        package = work / filename
        record = {'schema_version': 1, 'source': provenance, 'filename': filename,
                  'name': ARCH_PACKAGE, 'version': release, 'architecture': 'any',
                  'sha256': package_build.sha(package)}
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='sunuefi-sensors-', dir=output.parent) as temporary:
            candidate = Path(temporary) / 'package'
            candidate.mkdir()
            shutil.copyfile(package, candidate / filename)
            (candidate / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
            pacman_package(candidate)
            candidate.rename(output)
    return record


def pacman_package(directory):
    """Verify the shared package and its runtime paths before use."""
    from build.overlay_packages import pacman_info

    manifest = directory / 'manifest.json'
    if directory.is_symlink() or manifest.is_symlink():
        raise ValueError('Invalid shared Arch sensor package directory')
    record = json.loads(manifest.read_text())
    filename, version = record['filename'], record['version']
    archive = directory / filename
    if (record['schema_version'] != 1 or record['name'] != ARCH_PACKAGE or
            record['architecture'] != 'any' or
            not re.fullmatch(r'[0-9][a-zA-Z0-9_.+]*-[0-9.]+', version) or
            filename != f'{ARCH_PACKAGE}-{version}-any.pkg.tar.zst' or
            archive.is_symlink() or not archive.is_file() or
            record['sha256'] != package_build.sha(archive)):
        raise ValueError('Shared Arch sensor package differs from its build record')
    info = pacman_info(archive)
    if ((info.get('pkgname'), info.get('pkgver'), info.get('arch')) !=
            ([ARCH_PACKAGE], [version], ['any']) or SSC_DEPENDENCY not in info.get('depend', ())):
        raise ValueError('Shared Arch sensor package has incorrect package metadata')
    paths = subprocess.check_output(['bsdtar', '-tf', archive], text=True).splitlines()
    payload = {name for name in paths if not name.startswith('.') and not name.endswith('/')}
    if payload != ARCH_RUNTIME:
        raise ValueError('Shared Arch sensor package has an unexpected file list')
    return archive, {name: record[name] for name in ('name', 'version', 'architecture', 'sha256')}


def check_ssc_provider(archive):
    from build.overlay_packages import pacman_info

    info = pacman_info(archive)
    capability, required = SSC_DEPENDENCY.split('>=')
    if info.get('pkgname') != ['iio-sensor-proxy'] or info.get('arch') != ['aarch64']:
        raise ValueError(f'{archive.name} is not the ARM64 SensorProxy package')
    for value in info.get('provides', ()):
        if value.startswith(capability + '='):
            provided = value.split('=', 1)[1]
            if int(subprocess.check_output(['vercmp', provided, required], text=True)) >= 0:
                return
    raise ValueError(f'{archive.name} must provide {SSC_DEPENDENCY}')


def verify_arch_files(rootfs):
    from build_piano_ram_bootstrap import guest_resolve

    listed = subprocess.check_output(
        ['chroot', rootfs, 'pacman', '-Qlq', ARCH_PACKAGE], text=True).splitlines()
    if not {'/' + path for path in ARCH_RUNTIME} <= set(listed):
        raise ValueError('The native sensor package does not own all Piano runtime files')
    for path in sorted(ARCH_RUNTIME):
        if not guest_resolve(rootfs, '/' + path).is_file():
            raise ValueError('The installed sensor package is missing /' + path)


def build(output, sysroot=None, jobs=None, libssc_only=False, resume=False):
    output = output.resolve()
    jobs = package_build.default_jobs() if jobs is None else jobs
    if jobs < 1:
        raise ValueError('Use at least one build job')
    config = json.loads((ROOT / 'config/release.json').read_text())['sensors']
    policy = json.loads(POLICY.read_text())
    versions = policy['source_versions']
    runtime_versions = package_versions(policy)
    native_version = runtime_versions['piano-sensors']
    runtime_names = {'libssc2', 'libssc-bin'} if libssc_only else RUNTIME
    patches = [{'path': name, 'sha256': package_build.sha(ROOT / name)} for name in config['patches']]
    selected = {'libssc': versions['libssc']}
    anchors = {'libssc': 'libssc2'}
    if not libssc_only:
        selected.update({'iio-sensor-proxy': versions['iio-sensor-proxy'], 'fastrpc': versions['fastrpc']})
        anchors['iio-sensor-proxy'] = 'iio-sensor-proxy'
    target = debian.target_identity(sysroot or Path('/'), selected, anchors)
    cached = debian.reusable(output, native_version, patches, runtime_names, target)
    if cached and cached.get('policy_sha256') == package_build.sha(POLICY):
        return cached
    if resume:
        debian.validate_resume(output, target, patches, native_version)
    with debian.builder(output, sysroot) as (root, env), (output / 'build.log').open('a') as log:
        (output / '.incomplete').write_text('Sensors package build has not completed.\n')
        workspace, provenance = source_workspace(output / 'upstream', config)
        debian.dependencies(root, log, resume)
        target['source_checksums'] = debian.source_checksums(root, selected)
        packages, sources = {}, {}
        for name in ['libssc'] + ([] if libssc_only else ['iio-sensor-proxy']):
            work, origin = debian.source(root, output, name, versions[name], log)
            version = runtime_versions['libssc2' if name == 'libssc' else name]
            patches = sorted((workspace / 'patches' / name).glob('*.patch'))
            debian.add_patches(work, patches, name, version,
                               'Preserve SSC property types, raw reports and sensor claim ordering.')
            if name == 'libssc':
                install = work / 'debian/libssc-bin.install'
                if 'usr/bin/piano-ssc-vector' not in install.read_text().splitlines():
                    install.write_text(install.read_text().rstrip() + '\nusr/bin/piano-ssc-vector\n')
                symbols = work / 'debian/libssc2.symbols'
                if 'ssc_sensor_decode_vector_report@' not in symbols.read_text():
                    symbols.write_text(symbols.read_text().rstrip() +
                                        '\n ssc_sensor_decode_vector_report@LIBSSC_0.4.0 ' + version + '\n')
            marker = work / 'SOURCE.json'
            previous = json.loads(marker.read_text())
            identity = {'target': target, 'package_version': version,
                        'patches': [{'file': p.name, 'sha256': package_build.sha(p)} for p in patches]}
            if previous.get('build_identity') not in (None, identity):
                raise ValueError('Resume requires the same sensor source, patches and target ABI')
            previous['build_identity'] = identity
            marker.write_text(json.dumps(previous, indent=2) + '\n')
            changes = work.parent / f'{name}_{version}_arm64.changes'
            if not resume or not changes.is_file():
                debian.build(root, env, work, jobs, log, resume=resume)
            names = {'libssc2', 'libssc-bin'} if name == 'libssc' else {'iio-sensor-proxy'}
            packages.update(debian.collect(output, work.parent, names, version))
            sources[name] = origin
            if name == 'libssc' and not libssc_only:
                development = [p for p in work.parent.glob('*.deb')
                               if p.name.startswith(('libssc-dev_', 'libssc2_', 'gir1.2-ssc-2_'))]
                debian.guest(root, ['apt-get', 'install', '-y', '--no-remove', '--no-install-recommends',
                                   *[debian.inside(root, p) for p in development]],
                             stdout=log, stderr=subprocess.STDOUT)
        if not libssc_only:
            native = output / 'sources/piano-sensors'
            if not native.exists():
                shutil.copytree(workspace / 'piano-sensors', native)
            version = native_version
            debian.add_patches(native, [], 'piano-sensors', version,
                               'Build the sensor integration for Debian sid.')
            if not resume or not (native.parent / f'piano-sensors_{version}_arm64.changes').is_file():
                debian.build(root, env, native, jobs, log, resume=resume)
            packages.update(debian.collect(output, native.parent, {'piano-sensors'}, version))
            cache = output / 'fastrpc'
            provenance['fastrpc'] = debian.binaries(
                root, cache, ['fastrpc-support', 'libfastrpc1'], versions['fastrpc'], log)
            packages.update(debian.collect(output, cache, {'fastrpc-support', 'libfastrpc1'},
                                           versions['fastrpc'], built=False))
        if packages.keys() != runtime_names:
            raise ValueError('Incomplete sensors runtime package set')
        provenance.update(suite='sid', target=target, sources=sources, packages=packages, packages_built=True,
                          package_version=native_version,
                          recipe='original Debian debian/rules and control',
                          producer=str(Path(__file__).relative_to(ROOT)),
                          producer_sha256=package_build.sha(__file__),
                          builder_sha256=package_build.sha(debian.__file__),
                          build_packages=debian.build_packages(root),
                          runtime_dependencies=debian.runtime_dependencies(output, packages),
                          compiler_environment_sha256=package_build.sha(package_build.__file__),
                          verifier_sha256=debian.verifier_sha(),
                          policy_sha256=package_build.sha(POLICY))
        debian.write_manifest(output, provenance)
        (output / 'SOURCE').write_text(json.dumps(provenance, indent=2) + '\n')
        (output / '.incomplete').unlink()
        return provenance


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?', default=ROOT / 'build/sensors')
    parser.add_argument('--format', choices=('debian', 'pacman'), default='debian')
    parser.add_argument('--sysroot', type=Path, help='Clone a signed sid ARM64 sysroot below build/')
    parser.add_argument('--libssc-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--resume', action='store_true', help='Reuse completed source builds and existing objects')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.format == 'pacman' and (args.sysroot or args.libssc_only or args.resume):
        parser.error('The shared Arch package does not use --sysroot, --libssc-only or --resume')
    if args.sysroot and not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private', sys.executable,
                               __file__, *argv, '--worker']).returncode
    try:
        result = (build_pacman(args.output) if args.format == 'pacman' else
                  build(args.output, args.sysroot, args.jobs, args.libssc_only, args.resume))
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
