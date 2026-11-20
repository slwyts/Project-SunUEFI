#!/usr/bin/env python3
"""Prepare Windows ARM64 deployment files on the build host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from tempfile import TemporaryDirectory
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = ROOT / 'windows/deployment'
LOADER = Path('EFI/Microsoft/Boot/bootmgfw.efi')
INSTALL_IMAGE = ('install.wim', 'install.esd', 'install.swm')


def required_tool(name):
    tool = shutil.which(name)
    if tool is None:
        raise ValueError(f'Missing Windows media build tool: {name}')
    return tool


def member(directory, name):
    """Resolve names on case-insensitive installation media extracted on Linux."""
    matches = [p for p in directory.iterdir() if p.name.casefold() == name.casefold()]
    if len(matches) != 1:
        raise ValueError(f'Expected one {name} in {directory.name}')
    return matches[0]


def check_loader(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError('Missing ARM64 EFI application in the ISO')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        pe = stream.read(94)
        if (len(pe) != 94 or pe[:4] != b'PE\0\0' or
                struct.unpack_from('<H', pe, 4)[0] != 0xaa64 or
                struct.unpack_from('<H', pe, 24)[0] != 0x20b or
                struct.unpack_from('<H', pe, 92)[0] != 10):
            raise ValueError('The installation bootloader is not an ARM64 EFI application')


def check_ramdisk(bcd, sdi, wim):
    store = bcd.read_bytes()
    paths = [s.encode('utf-16le') for s in (r'\boot\boot.sdi', r'\sources\boot.wim')]
    if not store.startswith(b'regf') or not all(path in store.lower() for path in paths):
        raise ValueError('The media BCD does not reference boot/boot.sdi and sources/boot.wim')
    with sdi.open('rb') as stream:
        if stream.read(8) != b'$SDI0001':
            raise ValueError('Invalid WinPE SDI image')
    with wim.open('rb') as stream:
        header = stream.read(124)
    if len(header) != 124 or header[:8] != b'MSWIM\0\0\0':
        raise ValueError('Invalid WinPE boot.wim')
    return struct.unpack_from('<I', header, 120)[0]


def image_info(wimtool, path):
    # Validate the image resources, not just the XML directory or filename.
    reference = ['--ref=' + str(path.parent / '*.[sS][wW][mM]')] if path.suffix.lower() == '.swm' else []
    subprocess.run([wimtool, 'verify', path, *reference, '--quiet'], check=True)
    xml = subprocess.check_output([wimtool, 'info', path, '--xml'])
    images = []
    for image in ET.fromstring(xml).findall('IMAGE'):
        windows = image.find('WINDOWS')
        if windows is None or windows.findtext('ARCH') != '12':
            continue
        version = [windows.findtext('VERSION/' + key) for key in ('MAJOR', 'MINOR', 'BUILD', 'SPBUILD')]
        images.append({'index': int(image.attrib['INDEX']), 'name': image.findtext('NAME'),
                       'edition': windows.findtext('EDITIONID'),
                       'version': '.'.join(n for n in version if n is not None)})
    if not images:
        raise ValueError(f'{path.name} contains no ARM64 Windows images')
    return images


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def publish(tree, output, previous):
    if output.exists() or output.is_symlink():
        # Do not retire unrelated files if --output points at a user's directory.
        receipt = output / 'media.json'
        if output.is_symlink() or not receipt.is_file():
            raise ValueError(f'Not a generated Windows media directory: {output}')
        actual = {p.relative_to(output).as_posix() for p in output.rglob('*')
                  if p.is_symlink() or not p.is_dir()}
        old = json.loads(receipt.read_text())
        if (not isinstance(old, dict) or old.get('format') != 'sunuefi-windows-media' or
                not isinstance(old.get('files'), dict) or
                actual - {'media.json'} != set(old['files'])):
            raise ValueError(f'Output contains files outside its media manifest: {output}')
        output.rename(previous)
    try:
        tree.rename(output)
    except OSError:
        if previous.exists():
            previous.rename(output)
        raise


def media(iso, output, probe=False):
    seven = required_tool('7z')
    wimtool = required_tool('wimlib-imagex')
    if not iso.is_file():
        raise ValueError(f'ISO not found: {iso}')
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=output.parent, prefix='.winpe-') as temp:
        work = Path(temp)
        raw = work / 'iso'
        tree = work / 'media'
        esp = tree / 'esp'
        ntfs = tree / 'ntfs'
        boot = esp / 'EFI/Microsoft/Boot'
        boot.parent.mkdir(parents=True)
        files = ['efi/boot/bootaa64.efi', 'efi/microsoft/boot/*', 'boot/boot.sdi',
                 'sources/boot.wim' if probe else 'sources/*']
        if not probe:
            files.append('setup.exe')
        subprocess.run([seven, 'x', '-y', '-bso0', '-bsp0', '-ssc-', f'-o{raw}',
                        '--', iso.resolve(), *files], check=True)
        efi = member(raw, 'efi')
        member(member(efi, 'microsoft'), 'boot').rename(boot)
        # Stock installation ISOs usually carry Boot Manager at the removable
        # media path. On the shared ESP, leave that path to the existing OS.
        loader = next((p for p in boot.iterdir() if p.name.casefold() == 'bootmgfw.efi'), None)
        if loader is None:
            loader = member(member(efi, 'boot'), 'bootaa64.efi')
        check_loader(loader)
        loader.replace(esp / LOADER)
        for path in boot.iterdir():
            if path.name.casefold() in ('cdboot.efi', 'cdboot_noprompt.efi',
                                        'efisys.bin', 'efisys_noprompt.bin'):
                path.unlink()  # Optical media support, not used on an ESP.
        (esp / 'boot').mkdir()
        sdi = esp / 'boot/boot.sdi'
        member(member(raw, 'boot'), 'boot.sdi').rename(sdi)
        (esp / 'sources').mkdir()
        wim = esp / 'sources/boot.wim'
        sources = member(raw, 'sources')
        member(sources, 'boot.wim').rename(wim)
        boot_index = check_ramdisk(member(boot, 'bcd'), sdi, wim)
        if probe:
            subprocess.run([wimtool, 'update', wim, str(boot_index)], cwd=TEMPLATES, text=True,
                           input='add winpeshl.ini /Windows/System32/winpeshl.ini\n'
                                 'add probe.cmd /Windows/System32/sunuefi-probe.cmd\n', check=True)
            marker = esp / 'SunUEFI/WinPE/probe.tag'
            marker.parent.mkdir(parents=True)
            marker.touch()
        images = image_info(wimtool, wim)
        if not any(image['index'] == boot_index for image in images):
            raise ValueError('WinPE boot index is not an ARM64 image')
        result = {'format': 'sunuefi-windows-media', 'loader': LOADER.as_posix(),
                  'boot': {'index': boot_index, 'images': images}, 'probe': probe}
        if not probe:
            names = {p.name.casefold(): p for p in sources.iterdir()}
            available = [names[n] for n in INSTALL_IMAGE if n in names]
            if len(available) != 1:
                raise ValueError('Expected one Windows installation image set under sources/')
            install = available[0]
            result['install'] = {'path': f'sources/{install.name}', 'images': image_info(wimtool, install)}
            ntfs.mkdir()
            sources.rename(ntfs / 'sources')
            member(raw, 'setup.exe').rename(ntfs / 'setup.exe')
        for path in esp.rglob('*'):
            if path.is_file() and path.stat().st_size >= 1 << 32:
                raise ValueError(f'{path.relative_to(esp)} cannot fit in a FAT32 file')
        result['files'] = {p.relative_to(tree).as_posix(): digest(p)
                           for p in sorted(tree.rglob('*')) if p.is_file()}
        (tree / 'media.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
        publish(tree, output, work / 'previous')
    return result


def run_media(args):
    output = args.output or ROOT / 'artifacts/windows' / ('winpe' if args.probe else 'media')
    result = media(args.iso, output.absolute(), args.probe)
    print(f'WinPE: {output}/esp/{result["loader"]}')
    if 'install' in result:
        print(f'Windows: {output}/ntfs/{result["install"]["path"]}')


def graphics_test(_args):
    cmake = required_tool('cmake')
    output = ROOT / 'artifacts/windows/tests'
    build = ROOT / 'build/windows/graphics-readback'
    if os.name == 'nt':
        options = ['-A', 'ARM64']
    else:
        options = ['-G', 'Ninja', '-DCMAKE_SYSTEM_NAME=Windows',
                   '-DCMAKE_SYSTEM_PROCESSOR=ARM64',
                   '-DCMAKE_CXX_COMPILER=' + required_tool('aarch64-w64-mingw32-clang++'),
                   '-DCMAKE_RC_COMPILER=' + required_tool('aarch64-w64-mingw32-windres')]
    subprocess.run([cmake, '-S', ROOT / 'windows/drivers/adreno/tests', '-B', build,
                    '-DCMAKE_BUILD_TYPE=Release',
                    f'-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={output}',
                    f'-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE={output}', *options], check=True)
    subprocess.run([cmake, '--build', build, '--config', 'Release'], check=True)
    print(output / 'graphics-readback.exe')


def drivers(args):
    project = ROOT / 'windows/drivers/adreno/adreno.vcxproj'
    if not args.kit_include:
        msbuild = shutil.which('msbuild')
        if not msbuild and os.name == 'nt':
            locator = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
            if locator.is_file():
                hits = subprocess.check_output([locator, '-latest', '-products', '*', '-requires',
                                                'Microsoft.Component.MSBuild', '-find',
                                                r'MSBuild\**\Bin\MSBuild.exe'], text=True).splitlines()
                msbuild = next(iter(hits), None)
        if not msbuild:
            raise ValueError('Use a Visual Studio/WDK developer shell, or pass --kit-include for LLVM.')
        subprocess.run([msbuild, project, '/m', '/p:Platform=ARM64', '/p:Configuration=Release'], check=True)
        return

    compiler, librarian = required_tool('clang-cl'), required_tool('llvm-lib')
    work = ROOT / 'build/windows/adreno/Release'
    output = ROOT / 'artifacts/windows/drivers/adreno/Release'
    work.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    # Kernel CRT headers must win over the user CRT, including with split kits.
    includes = [f'/imsvc{path}' for name in ('km', 'km/crt', 'shared', 'ucrt')
                for root in args.kit_include
                if (path := root.resolve() / name).is_dir()]
    units = ET.parse(project).findall('.//{*}ClCompile[@Include]')
    objects = []
    for unit in units:
        source = project.parent / unit.attrib['Include']
        obj = work / (source.stem + '.obj')
        subprocess.run([compiler, '--target=aarch64-pc-windows-msvc', '/nologo', '/kernel',
                        '/std:c17', '/W4', '/WX', '/c', '/D_ARM64_=1',
                        *includes, f'/Fo{obj}', source], check=True)
        objects.append(obj)
    library = output / (project.stem + '.lib')
    subprocess.run([librarian, f'/out:{library}', *(obj.name for obj in objects)],
                   cwd=work, check=True)
    print(library)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    build = commands.add_parser('media', help='Prepare WinPE on FAT32 and Windows Setup files on NTFS')
    build.add_argument('--iso', required=True, type=Path, help='Windows ARM64 installation ISO')
    build.add_argument('--output', type=Path, help='Generated files (default: artifacts/windows/media or winpe)')
    build.add_argument('--probe', action='store_true', help='Boot a diagnostic WinPE, collect local logs and reboot')
    build.set_defaults(run=run_media)
    graphics = commands.add_parser('graphics-test', help='Build the ARM64 Direct3D hardware readback test')
    graphics.set_defaults(run=graphics_test)
    driver = commands.add_parser('drivers', help='Build the ARM64 WDK graphics driver components')
    driver.add_argument('--kit-include', type=Path, action='append',
                        help='Use LLVM with this versioned SDK/WDK Include directory (repeat for split kits)')
    driver.set_defaults(run=drivers)
    args = parser.parse_args(argv)
    try:
        args.run(args)
    except (OSError, ValueError, ET.ParseError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
