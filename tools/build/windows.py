#!/usr/bin/env python3
"""Prepare Windows ARM64 deployment files on the build host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
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


def driver_infs(directory):
    if not directory.is_dir():
        raise ValueError(f'Windows driver directory not found: {directory}')
    infs = []
    for path in directory.rglob('*'):
        if path.is_symlink():
            raise ValueError(f'Use regular files in a Windows driver package, not a symlink: {path}')
        if path.is_file() and path.suffix.lower() == '.inf':
            infs.append(path.relative_to(directory).as_posix())
    if not infs:
        raise ValueError(f'No INF driver packages found in {directory}')
    return sorted(infs)


def customize_winpe(wimtool, wim, index, work, probe, drivers):
    commands = []

    def add_text(name, destination, text=None):
        # cmd.exe seeks to labels by byte offset. Do not depend on Git line endings.
        if text is None:
            text = (TEMPLATES / name).read_text(encoding='utf-8')
        (work / name).write_bytes(text.replace('\n', '\r\n').encode('utf-8'))
        commands.append(f'add {name} {destination}')

    if drivers is not None:
        # Capture directly from the selected directory. Keep relative INF/CAT/payload paths
        # intact and never copy this package into the output being captured.
        subprocess.run([wimtool, 'update', wim, str(index)], cwd=drivers,
                       input='add . /SunUEFI/Drivers\n', text=True, check=True)
        add_text('drivers.cmd', '/SunUEFI/drivers.cmd')
    if probe:
        add_text('winpeshl.ini', '/Windows/System32/winpeshl.ini')
        add_text('probe.cmd', '/Windows/System32/sunuefi-probe.cmd')
    elif drivers is not None:
        add_text('winpeshl.ini', '/Windows/System32/winpeshl.ini',
                 '[LaunchApps]\n%SYSTEMROOT%\\System32\\cmd.exe, "/d /c %SYSTEMDRIVE%\\SunUEFI\\drivers.cmd setup"\n')
    if commands:
        subprocess.run([wimtool, 'update', wim, str(index)], cwd=work,
                       input='\n'.join(commands) + '\n', text=True, check=True)


def media(iso, output, probe=False, drivers=None):
    seven = required_tool('7z')
    wimtool = required_tool('wimlib-imagex')
    if not iso.is_file():
        raise ValueError(f'ISO not found: {iso}')
    if drivers is not None:
        drivers = drivers.resolve()
        if output.parent.resolve().is_relative_to(drivers):
            raise ValueError('Choose a Windows driver directory separate from the media build directory')
        infs = driver_infs(drivers)
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
        customize_winpe(wimtool, wim, boot_index, work, probe, drivers)
        if probe:
            marker = esp / 'SunUEFI/WinPE/probe.tag'
            marker.parent.mkdir(parents=True)
            marker.touch()
        images = image_info(wimtool, wim)
        if not any(image['index'] == boot_index for image in images):
            raise ValueError('WinPE boot index is not an ARM64 image')
        result = {'format': 'sunuefi-windows-media', 'loader': LOADER.as_posix(),
                  'boot': {'index': boot_index, 'images': images}, 'probe': probe}
        if drivers is not None:
            result['drivers'] = infs
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
    result = media(args.iso, output.absolute(), args.probe, args.drivers)
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


def kmdf_headers(config, kits):
    major = config.findtext('{*}KMDF_VERSION_MAJOR')
    minor = config.findtext('{*}KMDF_VERSION_MINOR')
    if not major or not minor:
        raise ValueError('Specify the KMDF major and minor versions in the WDK project')
    version = f'{major}.{minor}'
    for kit in kits:
        directory = kit.parent / 'wdf/kmdf' / version
        if (directory / 'wdf.h').is_file():
            return directory
    raise ValueError(f'Missing KMDF {version} headers in the supplied Windows kits')


def wdk_driver_options(config, description, kits):
    def locate(options, purpose):
        match = next((path for path in options if path.exists()), None)
        if not match:
            raise ValueError(f'Missing {purpose} in the supplied Windows kits')
        return match

    kernel_dirs = [kit.parent.parent / 'Lib' / kit.name / 'km/ARM64' for kit in kits]
    framework = config.findtext('{*}DriverType')
    headers, libraries = [], []
    dependencies = ['bufferoverflowfastfailk.lib', 'ntoskrnl.lib']
    if framework == 'KMDF':
        headers.append(kmdf_headers(config, kits))
        wdf_dirs = [kit.parent.parent / 'Lib/wdf/kmdf/ARM64' / headers[0].name for kit in kits]
        libraries.extend(locate((path / name for path in wdf_dirs), name)
                         for name in ('wdfdriverentry.lib', 'wdfldr.lib'))
        dependencies.extend(('hal.lib', 'wmilib.lib'))
        entry = 'FxDriverEntry'
    elif framework == 'WDM':
        entry = 'GsDriverEntry'
    else:
        raise ValueError(f'Build {framework} drivers with the full MSBuild/WDK toolset')
    for node in description.findall('.//{*}Link/{*}AdditionalDependencies'):
        for dep in (node.text or '').split(';'):
            dep = dep.strip()
            if dep and dep != '%(AdditionalDependencies)':
                dependencies.append(dep.removeprefix('$(DDK_LIB_PATH)').lower())
    libraries += [locate((path / name for path in kernel_dirs), name) for name in dict.fromkeys(dependencies)]
    return headers, libraries, entry


def clang_kit_includes(directories, work, *, external_names=False):
    flags = [f'/imsvc{directory}' for directory in directories]
    if os.name != 'nt':
        # WDK headers use Windows filename casing, including for quoted
        # includes in their own directory. Leave all original kits untouched.
        roots = []
        for directory in directories:
            if any(parent in directories for parent in directory.parents):
                continue
            for base, _, files in os.walk(directory):
                entries = [{'type': 'file', 'name': name, 'external-contents': str(Path(base) / name)}
                           for name in files]
                if entries:
                    roots.append({'type': 'directory', 'name': base, 'contents': entries})
        overlay = work / 'kit-headers.json'
        # Incremental builds need real filenames in Clang's dependency files.
        contents = json.dumps({'version': 0, 'case-sensitive': False, 'use-external-names': external_names,
                               'roots': roots}).encode()
        if not overlay.is_file() or overlay.read_bytes() != contents:
            overlay.write_bytes(contents)
        flags.extend(['/clang:-ivfsoverlay', f'/clang:{overlay}'])
    return flags


def msvc_arm64_linker():
    # Git for Windows also ships a link.exe; resolve the target tool from VS.
    roots = []
    if configured := os.environ.get('VCToolsInstallDir'):
        roots.append(Path(configured))
    locator = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if locator.is_file():
        vs = subprocess.check_output([locator, '-latest', '-products', '*', '-requires',
                                      'Microsoft.VisualStudio.Component.VC.Tools.ARM64',
                                      '-property', 'installationPath'], text=True).strip()
        if vs:
            vc = Path(vs) / 'VC'
            default_version = vc / 'Auxiliary/Build/Microsoft.VCToolsVersion.default.txt'
            if default_version.is_file():
                roots.append(vc / 'Tools/MSVC' / default_version.read_text().strip())
    hosts = ['Hostx64', 'Hostx86']
    if platform.machine().lower() in ('arm64', 'aarch64'):
        hosts.insert(0, 'Hostarm64')
    for root in roots:
        for host in hosts:
            path = root / 'bin' / host / 'arm64/link.exe'
            if path.is_file():
                return path
    raise ValueError('Install the Visual Studio ARM64 C++ tools or set VCToolsInstallDir to their version directory')


def llvm_driver(project, kits):
    work = ROOT / 'build/windows' / project.stem / 'Release'
    output = ROOT / 'artifacts/windows/drivers' / project.stem / 'Release'
    work.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    description = ET.parse(project)
    config = description.find('.//{*}PropertyGroup[@Label="Configuration"]')
    kind = config.findtext('{*}ConfigurationType')
    directories, libraries = [], []
    if kind == 'Driver':
        directories, libraries, entry = wdk_driver_options(config, description, kits)
    elif kind == 'StaticLibrary' and config.findtext('{*}DriverType') == 'KMDF':
        directories.append(kmdf_headers(config, kits))
    elif kind != 'StaticLibrary':
        raise ValueError(f'{project.name}: build this driver type with the full MSBuild/WDK toolset')
    # Kernel CRT headers must win over the user CRT, including with split kits.
    directories.extend(path for part in ('km', 'km/crt', 'shared', 'ucrt')
                       for kit in kits if (path := kit / part).is_dir())
    compile_flags = clang_kit_includes(directories, work)
    compiler = required_tool('clang-cl')
    objects = []
    for unit in description.findall('.//{*}ClCompile[@Include]'):
        source = project.parent / unit.attrib['Include']
        obj = work / (source.stem + '.obj')
        subprocess.run([compiler, '--target=aarch64-pc-windows-msvc', '/nologo', '/kernel', '/O2', '/Brepro',
                        '/std:c17', '/W4', '/WX', '/c', '/D_ARM64_=1', *compile_flags, f'/Fo{obj}', source],
                       check=True)
        objects.append(obj)
    if kind == 'StaticLibrary':
        library = output / (project.stem + '.lib')
        subprocess.run([required_tool('llvm-lib'), f'/out:{library}', *(obj.name for obj in objects)],
                       cwd=work, check=True)
        return library
    target = description.findtext('.//{*}TargetName') or project.stem
    driver = output / (target + '.sys')
    if os.name == 'nt':
        linker = [msvc_arm64_linker(), '/kernel']
    else:
        # Unlike LINK /KERNEL, LLD /DRIVER does not mark ordinary kernel
        # code and data resident. Leave deliberately pageable sections alone.
        linker = [required_tool('lld-link'), '/section:.text,erp', '/section:.rdata,rp',
                  '/section:.data,rwp', '/section:.pdata,rp', '/section:INIT,erd', '/tsaware:no']
    subprocess.run([*linker, '/nologo', '/driver', '/machine:arm64', '/subsystem:native,10.0',
                    f'/entry:{entry}', '/nodefaultlib', '/opt:ref', '/opt:icf', '/integritycheck', '/dynamicbase',
                    '/nxcompat', '/release', '/Brepro', f'/out:{driver}', *objects, *libraries], check=True)
    for item in description.findall('.//{*}Inf[@Include]'):
        shutil.copy2(project.parent / item.attrib['Include'], output)
    return driver


def make_driver_catalogs(projects, kits):
    # Inf2Cat is a host tool. The WDK ships the x86 program for use on
    # Arm64 Windows as well; /os selects the architecture of the package.
    candidates = [kit.parent.parent / 'bin' / kit.name / 'x86/Inf2Cat.exe' for kit in kits]
    sdk = Path(os.environ.get('WindowsSdkDir') or Path(os.environ['ProgramFiles(x86)']) / 'Windows Kits/10')
    installed = sdk / 'bin'
    if installed.is_dir():
        versions = (p for p in installed.iterdir() if all(n.isdigit() for n in p.name.split('.')))
        candidates.extend(p / 'x86/Inf2Cat.exe' for p in
                          sorted(versions, key=lambda p: tuple(map(int, p.name.split('.'))), reverse=True))
    tool = next((p for p in candidates if p.is_file()), None) or shutil.which('Inf2Cat.exe')
    if not tool:
        raise ValueError('Install the Windows Driver Kit or provide its Include/<version> with --kit-include')
    for project in projects:
        sources = ET.parse(project).findall('.//{*}Inf[@Include]')
        if not sources:
            continue  # Kernel components without a PnP package have no catalog.
        package = ROOT / 'artifacts/windows/drivers' / project.stem / 'Release'
        for item in sources:
            name = Path(item.attrib['Include']).with_suffix('.inf').name
            if not (package / name).is_file():
                raise ValueError(f'Missing built driver INF: {package / name}')
        subprocess.run([tool, f'/driver:{package}', '/os:10_GE_ARM64'], check=True)


def drivers(args):
    projects = sorted((ROOT / 'windows/drivers').glob('*/*.vcxproj'))
    if not projects:
        raise ValueError('No Windows driver projects found')
    if args.catalogs and os.name != 'nt':
        raise ValueError('Generating Windows driver catalogs requires the WDK on a Windows host')
    if args.kit_include:
        kits = [path.resolve() for path in args.kit_include]
        for project in projects:
            print(llvm_driver(project, kits))
        if args.catalogs:
            make_driver_catalogs(projects, kits)
        return
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
    # These StampInf development overrides take precedence over the project's
    # literal DriverVer. Leave the developer shell unchanged.
    env = {name: value for name, value in os.environ.items()
           if name.upper() not in ('PRIVATE_DRIVER_PACKAGE', 'STAMPINF_DATE', 'STAMPINF_VERSION')}
    for project in projects:
        package = ROOT / 'artifacts/windows/drivers' / project.stem / 'Release'
        subprocess.run([msbuild, project, '/m', '/p:Platform=ARM64', '/p:Configuration=Release',
                        f'/p:PackageDir={package}', '/p:EnableInf2cat=false', '/p:SignMode=Off'],
                       env=env, check=True)
    if args.catalogs:
        make_driver_catalogs(projects, [])


def main(argv=None):
    if __package__:
        from . import windows_umd
    else:
        import windows_umd
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    build = commands.add_parser('media', help='Prepare WinPE on FAT32 and Windows Setup files on NTFS')
    build.add_argument('--iso', required=True, type=Path, help='Windows ARM64 installation ISO')
    build.add_argument('--output', type=Path, help='Generated files (default: artifacts/windows/media or winpe)')
    build.add_argument('--probe', action='store_true', help='Boot a diagnostic WinPE, collect local logs and reboot')
    build.add_argument('--drivers', type=Path,
                       help='Include signed ARM64 INF packages from this directory in WinPE and the installed OS')
    build.set_defaults(run=run_media)
    graphics = commands.add_parser('graphics-test', help='Build the ARM64 Direct3D hardware readback test')
    graphics.set_defaults(run=graphics_test)
    windows_umd.add_parser(commands)
    driver = commands.add_parser('drivers', help='Build the ARM64 WDK driver components')
    driver.add_argument('--kit-include', type=Path, action='append',
                        help='Build with LLVM using this SDK/WDK Include/<version>, its WDF headers and ARM64 libraries '
                             '(repeat for split kits)')
    driver.add_argument('--catalogs', action='store_true',
                        help='Use the Windows WDK to validate each INF package and generate an unsigned Arm64 catalog')
    driver.set_defaults(run=drivers)
    args = parser.parse_args(argv)
    try:
        args.run(args)
    except (OSError, ValueError, ET.ParseError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
