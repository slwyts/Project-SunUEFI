#!/usr/bin/env python3
"""Build the shared official Mesa development source as native distro packages."""
import argparse
import configparser
import fcntl
from email.utils import formatdate
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import debian, overlay_release
from desktop import package_build

CONFIG = ROOT / 'config/mesa.json'
RUNTIME = {'mesa-libgallium', 'libgbm1', 'libgbm-dev', 'libegl-mesa0', 'libglx-mesa0',
           'libgl1-mesa-dri', 'mesa-vulkan-drivers', 'mesa-opencl-icd', 'mesa-teflon-delegate'}


def policy():
    return json.loads(CONFIG.read_text())


def package_version():
    return overlay_release.debian_version(policy()['debian']['package_version'])


def debian_rules(original):
    # Mesa 27 removed the AMD anti-lag layer. Keep every remaining driver and
    # layer, and use the new upstream VRAM-report layer in the same package.
    original = original.replace('VULKAN_LAYERS += anti-lag device-select intel-nullhw overlay screenshot',
                                 'VULKAN_LAYERS += device-select intel-nullhw overlay screenshot vram-report-limit')
    return original.replace('GALLIUM_DEFAULTS = softpipe', 'GALLIUM_DEFAULTS =')


def arch_provenance(output, steamos):
    data = policy()
    record = {'official_source': data['source'], 'config_sha256': package_build.sha(CONFIG),
              'producer_sha256': package_build.sha(__file__), 'native_recipe': data['arch'],
              'recipe_sha256': package_build.sha(output / 'PKGBUILD'), 'patches': []}
    if steamos:
        record['patches'] = json.loads((ROOT / 'patches/mesa/27.0/series.json').read_text())['patches']
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')


def target_identity(root):
    from desktop import debian_package as signed
    root = Path(root).resolve()
    debian.verify_sid(root)
    data = policy()
    libraries = {name: row for name, row in debian.library_versions(root).items() if name not in RUNTIME}
    names = sorted(set(debian.ABI_PACKAGES) - RUNTIME)
    rows, _ = signed.signed_records(root, 'Packages', names)
    candidates = debian.apt_candidates(root, names)
    binaries = {}
    for name in names:
        row = debian.official_candidate(name, candidates.get(name), rows, libraries)
        source, version = signed.source_identity(row)
        binaries[name] = {'version': row['Version'], 'source': source,
                          'source_version': version, 'sha256': row['SHA256']}
    return debian.TargetIdentity({'suite': 'sid', 'architecture': 'arm64',
        'sources': {'mesa': data['debian']['recipe_source_version']},
        'binaries': binaries, 'libraries': libraries}, root)


def download(url, destination, digest, size=None):
    if not destination.is_file():
        destination.parent.mkdir(parents=True, exist_ok=True)
        partial = destination.with_suffix(destination.suffix + '.partial')
        with urllib.request.urlopen(url, timeout=120) as stream, partial.open('wb') as target:
            shutil.copyfileobj(stream, target)
        if package_build.sha(partial) != digest or (size and partial.stat().st_size != size):
            raise ValueError('Mesa source or recipe checksum mismatch: ' + destination.name)
        partial.replace(destination)
    if package_build.sha(destination) != digest or (size and destination.stat().st_size != size):
        raise ValueError('Mesa source or recipe checksum mismatch: ' + destination.name)
    return destination


def copy_unchanged(source, destination):
    if not destination.is_file() or package_build.sha(source) != package_build.sha(destination):
        shutil.copy2(source, destination)


def source_archive():
    data = policy()['source']
    return download(data['url'], ROOT / 'build/source-cache/mesa-27/mesa.tar.gz',
                    data['sha256'], data['size'])


def extract_source(destination):
    source = policy()['source']
    with tarfile.open(source_archive()) as archive:
        prefix = 'mesa-' + source['commit']
        for member in archive.getmembers():
            if member.name == prefix:
                continue
            if not member.name.startswith(prefix + '/'):
                raise ValueError('Unexpected Mesa source archive layout')
            member.name = member.name[len(prefix) + 1:]
            archive.extract(member, destination, filter='data')
    if (destination / 'VERSION').read_text().strip() != source['version']:
        raise ValueError('Mesa archive version differs from the pinned official commit')


def subprojects(work):
    data = policy()
    for name, pin in data.get('subprojects', {}).items():
        wrap = configparser.ConfigParser()
        wrap.read(work / 'subprojects' / (name + '.wrap'))
        destination = work / 'subprojects' / wrap['wrap-git']['directory']
        if destination.is_dir():
            continue
        archive = download(pin['url'], ROOT / 'build/source-cache/mesa-27' / pin['url'].rsplit('/', 1)[-1], pin['sha256'])
        destination.mkdir()
        with tarfile.open(archive) as stream:
            prefix = stream.getnames()[0] + '/'
            for member in stream.getmembers():
                if not member.name.startswith(prefix):
                    continue
                member.name = member.name[len(prefix):]
                if member.name:
                    stream.extract(member, destination, filter='data')
    for path in sorted((work / 'subprojects').glob('*-rs.wrap')):
        wrap = configparser.ConfigParser()
        wrap.read(path)
        if not wrap.has_section('wrap-file'):
            continue
        row = wrap['wrap-file']
        if 'source_url' not in row or (work / 'subprojects' / row['directory']).is_dir():
            continue
        archive = download(row['source_url'], ROOT / 'build/source-cache/mesa-27' / row['source_filename'], row['source_hash'])
        with tarfile.open(archive) as stream:
            stream.extractall(work / 'subprojects', filter='data')
        if row.get('patch_directory'):
            shutil.copytree(work / 'subprojects/packagefiles' / row['patch_directory'],
                            work / 'subprojects' / row['directory'], dirs_exist_ok=True)


def package_identity(path):
    return dict(line.split(': ', 1) for line in debian.capture(
        ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture']).splitlines())


def packaging_source(root, output, log):
    from desktop import debian_package as signed
    version = policy()['debian']['recipe_source_version']
    cache = output / 'archives/mesa'
    cache.mkdir(parents=True, exist_ok=True)
    checksums = debian.source_checksums(root, {'mesa': version})['mesa']
    if not all((cache / name).is_file() and package_build.sha(cache / name) == row['sha256']
               for name, row in checksums.items()):
        debian.command(root, {}, cache, ['apt-get', 'source', '--download-only', '--only-source',
                                        'mesa=' + version], log)
    origin = signed.verify_source(root, cache, 'mesa', version)
    archive = next(row['file'] for row in origin['archives'] if '.debian.tar.' in row['file'])
    with tarfile.open(cache / archive) as source:
        recipe = {name: source.extractfile(name).read() for name in ('debian/rules', 'debian/control')}
    origin['archives'] = [origin.pop('dsc'), *origin['archives']]
    origin['recipe'] = {name: hashlib.sha256(data).hexdigest() for name, data in recipe.items()}
    return origin, recipe


def reusable(output, target=None):
    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        if (record.get('package_version') != package_version() or
                record.get('official_source') != policy()['source'] or record.get('patches') != [] or
                record.get('producer_sha256') != package_build.sha(__file__) or
                record.get('config_sha256') != package_build.sha(CONFIG) or
                record.get('build_recipe_version') != 1 or
                record.get('compiler_environment_sha256') != package_build.sha(package_build.__file__) or
                set(record.get('packages', {})) != RUNTIME or (output / '.incomplete').exists()):
            return None
        if target is not None:
            if any(record['target'][name] != target[name] for name in ('suite', 'architecture', 'sources', 'binaries')):
                return None
            for name in record['runtime_dependencies']:
                if name in RUNTIME or name not in target['libraries']:
                    continue
                built_version = record['build_packages'].get(name)
                # Target-only ABI anchors are already checked in binaries above.
                if built_version is None and name in debian.ABI_PACKAGES:
                    continue
                if built_version != target['libraries'][name]['version']:
                    return None
        root = getattr(target, 'root', output / 'builder/rootfs')
        source_checksums = debian.source_checksums(root, {'mesa': policy()['debian']['recipe_source_version']})
        expected = {row['file']: {'sha256': row['sha256'], 'size': row['size']}
                    for row in record['source']['archives']}
        if expected != source_checksums['mesa']:
            return None
        signed_packaging = next(row for row in record['source']['archives'] if '.debian.tar.' in row['file'])
        if record.get('packaging_archive_sha256') != signed_packaging['sha256']:
            return None
        if Path(record['changes']['file']).name != record['changes']['file']:
            return None
        receipt = output / record['changes']['file']
        sums = debian.checksum_rows(next(row for row in debian.controls(receipt.read_text()) if row.get('Source') == 'mesa'))
        if package_build.sha(receipt) != record['changes']['sha256']:
            return None
        for name, row in record['packages'].items():
            path = output / 'runtime' / row['file']
            if Path(row['file']).name != row['file'] or package_build.sha(path) != row['sha256']:
                return None
            fields = package_identity(path)
            if fields['Package'] != name or fields['Version'] != package_version() or fields['Architecture'] != 'arm64':
                return None
            if sums.get(path.name) != {'sha256': row['sha256'], 'size': path.stat().st_size}:
                return None
        return record
    except (ValueError, OSError, KeyError, StopIteration):
        return None


def tree_hashes(work):
    return {str(path.relative_to(work)): package_build.sha(path)
            for path in work.rglob('*') if path.is_file() and not path.is_symlink() and
            path.relative_to(work).parts[0] not in ('debian', 'build') and
            path.name != 'VERSION' and path.suffix not in ('.pyc',)}


def verify_official_source(work, proof):
    if proof['official_source'] != policy()['source']:
        raise ValueError('Existing Mesa source differs from the configured official commit')
    for name, digest in proof['files'].items():
        if name.startswith('subprojects/'):
            continue  # Debian's standard cargo-registry preparation rewrites wraps.
        if package_build.sha(work / name) != digest:
            raise ValueError('Official Mesa source changed after extraction: ' + name)


def build(output, sysroot, jobs, resume=False):
    output = output.resolve()
    if jobs < 1 or not output.is_relative_to(ROOT / 'build'):
        raise ValueError('Use a project build output and at least one build job')
    output.mkdir(parents=True, exist_ok=True)
    # Desktop builds share this cache. Hold the lock before validation as
    # another build may be replacing its manifest or cleaning its objects.
    with (output / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return build_package(output, sysroot, jobs, resume)


def build_package(output, sysroot, jobs, resume):
    data = policy()
    target = target_identity(sysroot or Path('/'))
    if cached := reusable(output, target):
        return cached
    with debian.builder(output, sysroot) as (root, env), (output / 'build.log').open('a') as log:
        (output / '.incomplete').write_text('Mesa package build has not completed.\n')
        debian.dependencies(root, log, resume)
        origin, original_recipe = packaging_source(root, output, log)
        work = output / 'mesa-27/source'
        if not work.exists():
            work.mkdir(parents=True)
            extract_source(work)
            (work.parent / 'SOURCE.json').write_text(json.dumps({
                'official_source': data['source'], 'files': tree_hashes(work)}, indent=2) + '\n')
        proof = json.loads((work.parent / 'SOURCE.json').read_text())
        verify_official_source(work, proof)
        if not (work / 'debian/changelog').exists() or (work / 'debian/changelog').read_text().split('(', 1)[1].split(')', 1)[0] != package_version():
            packaging = next(row['file'] for row in origin['archives'] if '.debian.tar.' in row['file'])
            if (work / 'debian').exists():
                shutil.rmtree(work / 'debian')
            with tarfile.open(output / 'archives/mesa' / packaging) as archive:
                archive.extractall(work, filter='data')
            patch_folder = work / 'debian/patches'
            if patch_folder.exists():
                shutil.rmtree(patch_folder)
            changelog = work / 'debian/changelog'
            changelog.write_text(f'mesa ({package_version()}) sid; urgency=medium\n\n'
                '  * Package the shared official Mesa development source without source patches.\n\n'
                ' -- Project SunUEFI <sunuefi@users.noreply.github.com>  ' + formatdate(localtime=False) +
                '\n\n' + changelog.read_text())
        generated_version = package_version().split('-', 1)[0].replace('~', '-')
        if (work / 'VERSION').read_text().strip() not in (
                data['source']['version'], package_version(), generated_version):
            raise ValueError('Existing Mesa source differs; preserve it and use a fresh output')
        for name in ('rules', 'control'):
            expected = original_recipe['debian/' + name].decode()
            if name == 'rules':
                expected = debian_rules(expected)
                if (work / 'debian' / name).read_text() != expected:
                    (work / 'debian' / name).write_text(expected)
            if (work / 'debian' / name).read_text() != expected:
                raise ValueError('Original Debian Mesa recipe changed: ' + name)
        for manifest, additions in {
            'mesa-vulkan-drivers.install': ['usr/share/drirc.d/01-radv-performance.conf'],
            'mesa-drm-shim.install': ['usr/bin/amdgpu_list_devices'],
        }.items():
            path = work / 'debian' / manifest
            contents = path.read_text().splitlines()
            changed = [line for line in additions if line not in contents]
            if changed:
                path.write_text('\n'.join(contents + changed) + '\n')
        llvm = data['debian']['llvm_version']
        subprojects(work)
        if env.get('CC') == 'clang':
            env = dict(env, CC=package_build.GUEST_TOOLS + '/bin/clang',
                CXX=package_build.GUEST_TOOLS + '/bin/clang++', LIBCLANG_PATH=f'/usr/lib/llvm-{llvm}/lib',
                BINDGEN_EXTRA_CLANG_ARGS=f'-resource-dir=/usr/lib/llvm-{llvm}/lib/clang/{llvm}')
        changes = work.parent / f'mesa_{package_version()}_arm64.changes'
        if not resume or not changes.is_file():
            debian.build(root, env, work, jobs, log, architecture_only=True,
                          resume=resume and (work / 'build/meson-private/coredata.dat').is_file())
        packages = debian.collect(output, work.parent, RUNTIME, package_version())
        verify_official_source(work, proof)
        wanted = {row['file'] for row in packages.values()}
        for stale in (output / 'runtime').glob('*.deb'):
            if stale.name not in wanted:
                stale.unlink()
        receipt = output / changes.name
        shutil.copy2(changes, receipt)
        record = {'official_source': data['source'], 'source': origin, 'target': dict(target),
            'official_source_verification': data['source_verification'],
            'package_version': package_version(), 'packages_built': True, 'patches': [],
            'packages': packages, 'config_sha256': package_build.sha(CONFIG),
            'producer_sha256': package_build.sha(__file__),
            'build_packages': debian.build_packages(root),
            'runtime_dependencies': debian.runtime_dependencies(output, packages),
            'build_command': ['dpkg-buildpackage', '-B', '-us', '-uc', '-j' + str(jobs)],
            'debian_rules_sha256': package_build.sha(work / 'debian/rules'),
            'install_manifests': {path.name: package_build.sha(path) for path in (work / 'debian').glob('*.install')},
            'compiler_environment_sha256': package_build.sha(package_build.__file__),
            'build_recipe_version': 1,
            'subprojects': data.get('subprojects', {}),
            'source_files_sha256': hashlib.sha256(json.dumps(proof['files'], sort_keys=True).encode()).hexdigest(),
            'source_patch_series': [],
            'changes': {'file': receipt.name, 'sha256': package_build.sha(receipt)},
            'packaging_archive_sha256': next(row['sha256'] for row in origin['archives'] if '.debian.tar.' in row['file']),
            'recipe': 'original Debian rules and control with official unpatched Mesa source'}
        debian.write_manifest(output, record)
        (output / '.incomplete').unlink()
        return record


def arch_recipe(output, steamos=False):
    data = policy()
    output.mkdir(parents=True, exist_ok=True)
    archive = source_archive()
    copy_unchanged(archive, output / 'mesa.tar.gz')
    recipe = download(data['arch']['recipe_url'], ROOT / 'build/source-cache/mesa-27/PKGBUILD.arch',
                      data['arch']['recipe_sha256']).read_text()
    recipe = re.sub(r'^pkgver=.*$', 'pkgver=' + data['arch']['package_version'], recipe, flags=re.M)
    recipe = re.sub(r'^pkgrel=.*$', 'pkgrel=' + data['arch']['package_release'], recipe, flags=re.M)
    recipe = re.sub(r'^arch=\([^\n]*\)$', 'arch=(aarch64)', recipe, flags=re.M)
    recipe = recipe.replace('device-select,intel-nullhw,overlay,screenshot,anti-lag,vram-report-limit',
                             'device-select,intel-nullhw,overlay,screenshot,vram-report-limit')
    recipe = recipe.replace('    _pick vkdevice $libdir/libVkLayer_MESA_anti_lag.so\n', '')
    recipe += '\nsource=(mesa.tar.gz)\nsha256sums=(' + data['source']['sha256'] + ')\n'
    recipe += 'b2sums=(' + hashlib.blake2b(archive.read_bytes()).hexdigest() + ')\n'
    with tarfile.open(archive) as source:
        for member in source.getmembers():
            if not member.name.endswith('-rs.wrap'):
                continue
            wrap = configparser.ConfigParser()
            wrap.read_string(source.extractfile(member).read().decode())
            if not wrap.has_section('wrap-file'):
                continue
            row = wrap['wrap-file']
            dependency = download(row['source_url'], output / row['source_filename'], row['source_hash'])
            recipe += '\nsource+=("' + dependency.name + '")\n'
            recipe += 'sha256sums+=(' + row['source_hash'] + ')\n'
            recipe += 'b2sums+=(' + hashlib.blake2b(dependency.read_bytes()).hexdigest() + ')\n'
            recipe += 'noextract+=("' + dependency.name + '")\n'
    prepare = 'prepare() {\n  mv "mesa-' + data['source']['commit'] + '" "mesa-$_pkgver"\n'
    for name, pin in data.get('subprojects', {}).items():
        with tarfile.open(archive) as source:
            wrap_text = source.extractfile('mesa-' + data['source']['commit'] + '/subprojects/' + name + '.wrap').read().decode()
        wrap_config = configparser.ConfigParser()
        wrap_config.read_string(wrap_text)
        subarchive = download(pin['url'], output / pin['url'].rsplit('/', 1)[-1], pin['sha256'])
        with tarfile.open(subarchive) as stream:
            root_name = stream.getnames()[0]
        recipe += '\nsource+=("' + subarchive.name + '")\n'
        recipe += 'sha256sums+=(' + pin['sha256'] + ')\n'
        recipe += 'b2sums+=(' + hashlib.blake2b(subarchive.read_bytes()).hexdigest() + ')\n'
        prepare += '  mv "' + root_name + '" "mesa-$_pkgver/subprojects/' + wrap_config['wrap-git']['directory'] + '"\n'
    if not steamos:
        (output / 'PKGBUILD').write_text(recipe + '\n' + prepare + '}\n')
        arch_provenance(output, False)
        return output
    with tarfile.open(archive) as source:
        text = source.extractfile('mesa-' + data['source']['commit'] + '/subprojects/wayland-protocols.wrap').read().decode()
    protocols = configparser.ConfigParser()
    protocols.read_string(text)
    wrap = protocols['wrap-file']
    download(wrap['source_url'], output / wrap['source_filename'], wrap['source_hash'])
    patches = json.loads((ROOT / 'patches/mesa/27.0/series.json').read_text())['patches']
    for row in patches:
        patch = ROOT / 'patches/mesa/27.0' / row['file']
        if package_build.sha(patch) != row['sha256']:
            raise ValueError('SteamOS Mesa patch checksum mismatch')
        shutil.copy2(patch, output / row['file'])
        recipe += "\nsource+=(" + row['file'] + ")\nsha256sums+=(" + row['sha256'] + ")\n"
        recipe += 'b2sums+=(' + hashlib.blake2b((output / row['file']).read_bytes()).hexdigest() + ')\n'
        prepare += '  patch --batch --fuzz=0 -Np1 -d "mesa-$_pkgver" < "' + row['file'] + '"\n'
    recipe += '\nsource+=("' + wrap['source_filename'] + '")\n'
    recipe += 'sha256sums+=(' + wrap['source_hash'] + ')\n'
    recipe += 'b2sums+=(' + hashlib.blake2b((output / wrap['source_filename']).read_bytes()).hexdigest() + ')\n'
    recipe += 'noextract+=("' + wrap['source_filename'] + '")\n'
    prepare += '  mkdir -p "mesa-$_pkgver/subprojects/packagecache"\n'
    prepare += '  cp "' + wrap['source_filename'] + '" "mesa-$_pkgver/subprojects/packagecache/"\n}\n'
    recipe += '\n' + prepare
    recipe += '\npkgname=(deckard-mesa-linux-aarch64)\n'
    recipe += 'pkgbase=deckard-mesa-linux-aarch64\n'
    recipe += 'pkgdesc="Official Mesa development drivers for the SteamOS native runtime"\n'
    recipe += 'provides=("mesa=$epoch:$pkgver-$pkgrel" "mesa-libgl=$epoch:$pkgver-$pkgrel" '
    recipe += 'opengl-driver vulkan-driver opencl-driver)\n'
    recipe += 'conflicts=(mesa mesa-libgl deckard-mesa-linux-aarch64-assert)\n'
    recipe += 'depends=(expat glibc libdrm libelf gcc-libs libglvnd libx11 libxcb libxext '
    recipe += 'libxshmfence libxxf86vm llvm-libs lm_sensors spirv-tools wayland zlib zstd '
    recipe += 'libdisplay-info systemd-libs vulkan-icd-loader xcb-util-keysyms libclc clang)\n'
    recipe += 'makedepends=(${makedepends[@]/libgcc/gcc-libs})\n'
    recipe += 'makedepends=(${makedepends[@]/libstdc++/gcc-libs})\n'
    recipe += 'options+=(!debug)\n'
    recipe += '\n# Replace Vulkan/GL together, retaining Valve\'s native package identity.\n'
    recipe += '\npackage() {\n  meson install -C build --destdir "$pkgdir" --no-rebuild --skip-subprojects\n'
    recipe += '  install -Dm644 "mesa-$_pkgver/docs/license.rst" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"\n}\n'
    (output / 'PKGBUILD').write_text(recipe)
    arch_provenance(output, True)
    return output


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?', default=ROOT / 'build/mesa')
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.sysroot and not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private', sys.executable,
                               __file__, *argv, '--worker']).returncode
    try:
        print(json.dumps(build(args.output, args.sysroot, args.jobs, args.resume), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
