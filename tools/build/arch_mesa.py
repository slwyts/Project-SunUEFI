#!/usr/bin/env python3
"""Build complete native Mesa pacman packages without changing the Debian producer."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import debian, mesa
from desktop import package_build


def recipe(output, steamos=False):
    output.mkdir(parents=True, exist_ok=True)
    cache = ROOT / 'build/source-cache/mesa-27'
    with tarfile.open(mesa.source_archive()) as archive:
        for member in archive:
            if not member.name.endswith('-rs.wrap'):
                continue
            wrap = configparser.ConfigParser()
            wrap.read_string(archive.extractfile(member).read().decode())
            if not wrap.has_section('wrap-file'):
                continue
            wrap = wrap['wrap-file']
            existing = output / wrap['source_filename']
            if existing.is_file() and package_build.sha(existing) == wrap['source_hash']:
                mesa.copy_unchanged(existing, cache / existing.name)
            dependency = mesa.download(wrap['source_url'], cache / wrap['source_filename'], wrap['source_hash'])
            mesa.copy_unchanged(dependency, output / dependency.name)
    mesa.arch_recipe(output, steamos)
    text = (output / 'PKGBUILD').read_text()
    text = text.replace('-D amdgpu-virtio=true', '-D amd-options=use-llvm,virtio')
    text = text.replace('  meson compile -C build\n',
                        '  meson compile -C build ${MAKEFLAGS:-}\n')
    # Clang's builtin fwrite prototype can hide size_t from bindgen.
    text = text.replace('  export MESON_PACKAGE_CACHE_DIR="$srcdir"\n',
                        '  export MESON_PACKAGE_CACHE_DIR="$srcdir"\n'
                        '  export BINDGEN_EXTRA_CLANG_ARGS="${BINDGEN_EXTRA_CLANG_ARGS:-} -fno-builtin-fwrite"\n')
    text = text.replace('    _pick vkradeon usr/share/drirc.d/00-radv-defaults.conf\n',
                        '    _pick vkradeon usr/share/drirc.d/00-radv-defaults.conf\n'
                        '    _pick vkradeon usr/share/drirc.d/01-radv-performance.conf\n')
    if steamos:
        text = text.replace('libdisplay-info systemd-libs vulkan-icd-loader xcb-util-keysyms libclc clang)',
                            'libdisplay-info systemd-libs vulkan-icd-loader xcb-util-keysyms '
                            'libclc clang libpng python spirv-llvm-translator)')
    if not steamos:
        source = mesa.policy()['source']
        with tarfile.open(mesa.source_archive()) as archive:
            wrap = configparser.ConfigParser()
            wrap.read_string(archive.extractfile(
                'mesa-' + source['commit'] + '/subprojects/wayland-protocols.wrap').read().decode())
        wrap = wrap['wrap-file']
        dependency = mesa.download(wrap['source_url'], output / wrap['source_filename'], wrap['source_hash'])
        before, prepare = text.rsplit('\nprepare() {\n', 1)
        body, closing = prepare.rsplit('\n}', 1)
        text = before + '\nsource+=("' + dependency.name + '")\nsha256sums+=(' + wrap['source_hash'] + ')\n'
        text += 'b2sums+=(' + hashlib.blake2b(dependency.read_bytes()).hexdigest() + ')\n'
        text += 'noextract+=("' + dependency.name + '")\n'
        text += ('\nprepare() {\n' + body + '\n  mkdir -p "mesa-$_pkgver/subprojects/packagecache"\n'
                 '  cp "' + dependency.name + '" "mesa-$_pkgver/subprojects/packagecache/"\n}' + closing)
    (output / 'PKGBUILD').write_text(text)
    record = json.loads((output / 'SOURCE.json').read_text())
    record.update(producer_sha256=package_build.sha(__file__),
                  source_preparation_sha256=package_build.sha(mesa.__file__),
                  recipe_sha256=package_build.sha(output / 'PKGBUILD'))
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
    return output


def build(output, root, jobs, resume=False, steamos=False):
    import build_arch_package
    output, root = output.resolve(), root.resolve()
    if (not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root) or
            root.is_relative_to(output) or jobs < 1):
        raise ValueError('Use separate project build output and ARM64 rootfs directories')
    output.mkdir(parents=True, exist_ok=True)
    (output / '.incomplete').write_text('Native Mesa package build has not completed.\n')
    selected = recipe(output / 'recipe', steamos)
    build_arch_package.build(root, selected, output / 'runtime', resume=resume, jobs=jobs,
                             validate=validate)
    record = json.loads((selected / 'SOURCE.json').read_text())
    record.update(packages_built=True,
                  packages=json.loads((output / 'runtime/manifest.json').read_text())['packages'],
                  validation=json.loads((output / 'runtime/VALIDATION.json').read_text()))
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
    (output / '.incomplete').unlink()
    return record


def validate(root, work, output):
    """Inspect actual package ELF and configured features before removing objects."""
    source = mesa.policy()['source']
    candidates = [path for path in (work / 'src').glob('mesa-*') if path.is_dir()]
    if len(candidates) != 1:
        raise ValueError('Native Mesa build has an ambiguous prepared source tree')
    prepared = candidates[0]
    selected = debian.capture(['chroot', root, 'bash', '-c',
        'source /var/tmp/sunuefi-package-build/PKGBUILD; printf "%s\\n" "${pkgname[@]}"']).splitlines()
    patches = json.loads((work / 'SOURCE.json').read_text())['patches']
    changed = {}
    if patches:
        with tarfile.open(mesa.source_archive()) as archive:
            name = 'src/gallium/drivers/freedreno/freedreno_resource.c'
            original = archive.extractfile('mesa-' + source['commit'] + '/' + name).read()
        patch = ROOT / 'patches/mesa/27.0' / patches[0]['file']
        with tempfile.NamedTemporaryFile(prefix='mesa-source-', dir=output) as temporary:
            temporary.write(original)
            temporary.flush()
            result = subprocess.run(['patch', '--batch', '--fuzz=0', '--output=-', temporary.name, patch],
                                    capture_output=True, check=True)
        changed[name] = hashlib.sha256(result.stdout).hexdigest()
    source_files = 0
    with tarfile.open(mesa.source_archive()) as archive:
        prefix = 'mesa-' + source['commit'] + '/'
        for member in archive:
            if not member.isfile():
                continue
            name = member.name.removeprefix(prefix)
            expected = changed.get(name)
            if expected is None:
                expected = hashlib.sha256(archive.extractfile(member).read()).hexdigest()
            if package_build.sha(prepared / name) != expected:
                raise ValueError('Prepared native Mesa source differs from its official archive: ' + name)
            source_files += 1
    options = json.loads((work / 'src/build/meson-info/intro-buildoptions.json').read_text())
    options = {row['name']: row['value'] for row in options}
    expected = {
        'gallium-drivers': 'all', 'gallium-rusticl': True, 'teflon': True,
        'video-codecs': ['all'], 'html-docs': 'enabled',
        'vulkan-drivers': ['amd', 'intel', 'intel_hasvk', 'swrast', 'freedreno', 'panfrost',
                           'virtio', 'broadcom', 'imagination', 'microsoft-experimental',
                           'nouveau', 'asahi', 'gfxstream'],
        'vulkan-layers': ['device-select', 'intel-nullhw', 'overlay', 'screenshot', 'vram-report-limit'],
    }
    for name, value in expected.items():
        if options.get(name) != value and not (name == 'gallium-drivers' and options.get(name) == ['all']):
            raise ValueError('Native Mesa package lost a configured feature: ' + name)
    packages = {}
    for archive in sorted(path for path in output.glob('*.pkg.tar.*')
                          if path.suffix in ('.xz', '.zst', '.gz')):
        metadata = debian.capture(['bsdtar', '-xOf', archive, '.PKGINFO'])
        fields = {}
        for line in metadata.splitlines():
            if ' = ' in line:
                name, value = line.split(' = ', 1)
                fields.setdefault(name, []).append(value)
        if (fields.get('arch') not in (['aarch64'], ['any']) or
                fields.get('pkgver') != ['1:' + mesa.policy()['arch']['package_version'] + '-' +
                                         mesa.policy()['arch']['package_release']]):
            raise ValueError('Native Mesa package identity differs: ' + archive.name)
        elf = {}
        process = subprocess.Popen(['bsdtar', '-cf', '-', '--format=gnutar', '@' + str(archive)],
                                   stdout=subprocess.PIPE)
        try:
            with tarfile.open(fileobj=process.stdout, mode='r|') as contents:
                for member in contents:
                    if not member.isfile():
                        continue
                    name = Path(member.name)
                    if name.is_absolute() or '..' in name.parts:
                        raise ValueError('Unsafe native package filename')
                    with contents.extractfile(member) as stream:
                        header = stream.read(20)
                        if not header.startswith(b'\x7fELF'):
                            continue
                        if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\xb7\x00':
                            raise ValueError('Native Mesa package contains a non-ARM64 ELF: ' + str(name))
                        elf['/' + name.as_posix()] = hashlib.sha256(header + stream.read()).hexdigest()
        finally:
            process.stdout.close()
            status = process.wait()
        if status:
            raise ValueError('Cannot inspect native Mesa archive: ' + archive.name)
        if elf:
            actual = debian.capture(['chroot', root, 'sha256sum', '--', *sorted(elf)])
            actual = {name: digest for digest, name in
                      (line.split('  ', 1) for line in actual.splitlines())}
            if actual != elf:
                raise ValueError('Installed native Mesa ELF differs from its package: ' + archive.name)
            for name in elf:
                result = subprocess.run(['chroot', root, 'ldd', '-r', name],
                                        capture_output=True, text=True)
                if result.returncode or any(word in result.stdout + result.stderr
                                             for word in ('not found', 'undefined symbol')):
                    raise ValueError('Installed native Mesa ELF has unresolved dependencies: ' + name +
                                     '\n' + result.stdout + result.stderr)
        if fields.get('depend'):
            missing = subprocess.run(['chroot', root, 'pacman', '-T', *fields['depend']],
                                     capture_output=True, text=True)
            if missing.returncode:
                raise ValueError('Installed native Mesa package dependencies are unsatisfied: ' +
                                 archive.name + '\n' + missing.stdout + missing.stderr)
        buildinfo = debian.capture(['bsdtar', '-xOf', archive, '.BUILDINFO'])
        if ('pkgbuild_sha256sum = ' + package_build.sha(work / 'PKGBUILD')) not in buildinfo.splitlines():
            raise ValueError('Native Mesa build receipt differs from its recipe: ' + archive.name)
        (output / (archive.name + '.BUILDINFO')).write_text(buildinfo + '\n')
        packages[archive.name] = {'control': fields, 'elf_sha256': elf}
    if not packages or not any(row['elf_sha256'] for row in packages.values()):
        raise ValueError('Native Mesa build produced no ARM64 runtime ELF files')
    if {row['control']['pkgname'][0] for row in packages.values()} != set(selected):
        raise ValueError('Native Mesa build did not produce the complete selected package set')
    (output / 'builder-packages.tsv').write_text(debian.capture(['chroot', root, 'pacman', '-Q']) + '\n')
    (output / 'VALIDATION.json').write_text(json.dumps({
        'official_source_files_checked': source_files,
        'build_options': options, 'packages': packages}, indent=2) + '\n')


def main(argv):
    dispatch = argparse.ArgumentParser(add_help=False)
    dispatch.add_argument('--format', choices=('debian', 'pacman'), default='debian')
    selected, remaining = dispatch.parse_known_args(argv)
    if selected.format == 'debian':
        return mesa.main(remaining)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rootfs', type=Path, required=True,
                        help='Isolated ARM64 pacman root; installs built packages')
    parser.add_argument('--steamos', action='store_true', help='Keep Valve package identity and height patch')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(remaining)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private', sys.executable,
                               __file__, *argv, '--worker']).returncode
    try:
        print(json.dumps(build(args.output, args.rootfs, args.jobs, args.resume, args.steamos), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
