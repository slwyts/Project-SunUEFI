#!/usr/bin/env python3
"""Build the sensor packages against the target Debian sid ABI."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import debian
from desktop import package_build

RUNTIME = {'fastrpc-support', 'libfastrpc1', 'libssc2', 'libssc-bin',
           'iio-sensor-proxy', 'piano-sensors'}
POLICY = ROOT / 'patches/piano-sensors/sid/build.json'


def source_workspace(output, config):
    source = ROOT / config['source']
    head = debian.capture(['git', '-C', source, 'rev-parse', 'HEAD'])
    dirty = debian.capture(['git', '-C', source, 'status', '--porcelain'])
    if head != config['commit'] or dirty:
        raise ValueError('Sensors source HEAD/cleanliness mismatch; run ./build.sh sources')
    workspace = output / 'upstream'
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


def build(output, sysroot=None, jobs=None, libssc_only=False):
    output = output.resolve()
    jobs = package_build.default_jobs() if jobs is None else jobs
    if jobs < 1:
        raise ValueError('Use at least one build job')
    config = json.loads((ROOT / 'config/release.json').read_text())['sensors']
    policy = json.loads(POLICY.read_text())
    versions = policy['source_versions']
    runtime_names = {'libssc2', 'libssc-bin'} if libssc_only else RUNTIME
    patches = [{'path': name, 'sha256': package_build.sha(ROOT / name)} for name in config['patches']]
    cached = debian.reusable(output, policy['native_version'], patches, runtime_names)
    if cached and cached.get('policy_sha256') == package_build.sha(POLICY):
        return cached
    with debian.builder(output, sysroot) as (root, env), (output / 'build.log').open('a') as log:
        (output / '.incomplete').write_text('Sensors package build has not completed.\n')
        workspace, provenance = source_workspace(output, config)
        debian.dependencies(root, log)
        packages, sources = {}, {}
        for name in ['libssc'] + ([] if libssc_only else ['iio-sensor-proxy']):
            work, origin = debian.source(root, output, name, versions[name], log)
            version = versions[name] + '+' + policy['local_revisions'][name]
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
            debian.build(root, env, work, jobs, log)
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
            version = policy['native_version']
            debian.add_patches(native, [], 'piano-sensors', version,
                               'Build the sensor integration for Debian sid.')
            debian.build(root, env, native, jobs, log)
            packages.update(debian.collect(output, native.parent, {'piano-sensors'}, version))
            cache = output / 'fastrpc'
            cache.mkdir(exist_ok=True)
            debian.command(root, {}, cache, ['apt-get', 'download',
                                            'fastrpc-support=' + versions['fastrpc'],
                                            'libfastrpc1=' + versions['fastrpc']], log)
            # apt-get download authenticates these binaries against signed sid Packages.
            packages.update(debian.collect(output, cache, {'fastrpc-support', 'libfastrpc1'},
                                           versions['fastrpc'], built=False))
        if packages.keys() != runtime_names:
            raise ValueError('Incomplete sensors runtime package set')
        provenance.update(suite='sid', sources=sources, packages=packages, packages_built=True,
                          package_version=policy['native_version'],
                          recipe='original Debian debian/rules and control',
                          producer=str(Path(__file__).relative_to(ROOT)),
                          producer_sha256=package_build.sha(__file__),
                          builder_sha256=package_build.sha(debian.__file__),
                          policy_sha256=package_build.sha(POLICY))
        debian.write_manifest(output, provenance)
        (output / 'SOURCE').write_text(json.dumps(provenance, indent=2) + '\n')
        (output / '.incomplete').unlink()
        return provenance


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?', default=ROOT / 'build/sensors')
    parser.add_argument('--sysroot', type=Path, help='Clone a signed sid ARM64 sysroot below build/')
    parser.add_argument('--libssc-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.sysroot and not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private', sys.executable,
                               __file__, *argv, '--worker']).returncode
    try:
        print(json.dumps(build(args.output, args.sysroot, args.jobs, args.libssc_only), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
