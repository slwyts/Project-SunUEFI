"""Build the ARM64 Direct3D 10/11 user-mode driver for the Adreno WDDM stack."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'upstream/woa_oneplus_ace5_pro'
OUTPUT = ROOT / 'artifacts/windows/drivers/adreno/Release'
WORK = ROOT / 'build/windows/graphics-umd'
DLL = 'a830_umd_arm64.dll'


def write_changed(path, contents):
    if isinstance(contents, str):
        contents = contents.encode('utf-8')
    if not path.is_file() or path.read_bytes() != contents:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def sources(name, spec, cache, keep):
    """Verify even cached archives and restore the selected source files from them."""
    filename = spec['url'].rsplit('/', 1)[1]
    archive = cache / filename
    if not archive.is_file():
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=cache) as tmp:
            part = Path(tmp) / filename
            with urllib.request.urlopen(spec['url'], timeout=120) as response, part.open('wb') as stream:
                shutil.copyfileobj(response, stream)
            if sha(part) != spec['sha256']:
                raise ValueError(f'SHA-256 mismatch downloading {filename}')
            part.replace(archive)
    elif sha(archive) != spec['sha256']:
        raise ValueError(f'SHA-256 mismatch in {archive}')

    top = f"{name}-{spec['version']}"
    output = WORK / 'sources' / f"{top}-{spec['sha256'][:12]}"
    receipt = output / '.sources.json'
    selection = sorted(map(str, keep))
    if receipt.is_file():
        try:
            previous = json.loads(receipt.read_text())
        except ValueError:
            previous = {}
        if previous.get('sha256') == spec['sha256'] and previous.get('selection') == selection:
            files = {p.relative_to(output).as_posix(): sha(p) for p in output.rglob('*')
                     if p != receipt and p.is_file() and not p.is_symlink()}
            if files and files == previous.get('files'):
                return output, files
    files = {}
    with tarfile.open(archive, 'r|xz') as tar:
        for entry in tar:
            path = PurePosixPath(entry.name)
            if not entry.isfile() or not path.parts or path.parts[0] != top or '..' in path.parts:
                continue
            relative = path.relative_to(top)
            if any(relative == p or p in relative.parents for p in keep):
                with tar.extractfile(entry) as stream:
                    contents = stream.read()
                write_changed(output / relative, contents)
                files[relative.as_posix()] = hashlib.sha256(contents).hexdigest()
    write_changed(receipt, json.dumps({'sha256': spec['sha256'], 'selection': selection, 'files': files}, sort_keys=True))
    return output, files


def tool(*names):
    for name in names:
        if executable := shutil.which(name):
            # clang-cl and lld-link select their personality from argv[0].
            return Path(executable).absolute()
    raise ValueError('Required Windows graphics build tool: ' + ' or '.join(names))


def kits(args):
    vc = args.vc_tools or os.environ.get('VCToolsInstallDir')
    if not vc or not (Path(vc) / 'include/vcruntime.h').is_file():
        raise ValueError('Provide --vc-tools <VC/Tools/MSVC/version> or use a Visual Studio developer shell')
    vc = Path(vc).resolve()
    include = [p.resolve() for p in (args.kit_include or [])]
    if not include and os.environ.get('WindowsSdkDir') and os.environ.get('WindowsSDKVersion'):
        include = [Path(os.environ['WindowsSdkDir']) / 'Include' / os.environ['WindowsSDKVersion'].strip('/\\')]
    headers = [vc / 'include', *(p / part for part in ('ucrt', 'shared', 'um', 'winrt')
                                  for p in include if (p / part).is_dir())]
    if not any((p / 'd3d10umddi.h').is_file() for p in headers):
        raise ValueError('Provide --kit-include for the WDK with um/d3d10umddi.h and the matching Windows SDK')
    libraries = [vc / 'lib/arm64', *(p.resolve() for p in (args.kit_lib or []))]
    for p in include:
        libraries.extend(p.parent.parent / 'Lib' / p.name / part / 'arm64' for part in ('um', 'ucrt'))
    return headers, list(dict.fromkeys(p for p in libraries if p.is_dir()))


def ninja_path(path):
    return Path(path).as_posix().replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def shell_tool(executable):
    text = str(executable)
    return (subprocess.list2cmdline([text]) if os.name == 'nt' else shlex.quote(text)).replace('$', '$$')


def build(args):
    # build.sh invokes windows.py both as a script and as an imported module.
    if __package__:
        from .windows import clang_kit_includes
    else:
        from windows import clang_kit_includes
    if args.jobs < 1:
        raise ValueError('--jobs must be positive')
    if not (DRIVER / 'src/mesa/umd/driver.cpp').is_file():
        raise ValueError('Fetch the Windows driver sources first: ./build.sh sources --group windows')
    pin = json.loads((ROOT / 'sources.lock.json').read_text())['woa_oneplus_ace5_pro']['commit']
    if subprocess.check_output(['git', '-C', DRIVER, 'rev-parse', 'HEAD'], text=True).strip() != pin:
        raise ValueError('The Adreno sources differ from sources.lock.json; run ./build.sh sources --group windows')
    cc, ld, ninja = tool('clang-cl'), tool('lld-link'), tool('ninja')
    bison, flex = tool('bison', 'win_bison'), tool('flex', 'win_flex')
    headers, libraries = kits(args)
    spec = json.loads((ROOT / 'config/windows-graphics.json').read_text())
    mesa, _ = sources('mesa', spec['mesa'], args.source_cache, (PurePosixPath('include'),
                   PurePosixPath('src/compiler/spirv/spirv.h'), PurePosixPath('src/compiler/spirv/GLSL.std.450.h')))
    vkd3d, vkd3d_files = sources('vkd3d', spec['vkd3d'], args.source_cache, tuple(map(PurePosixPath, (
        'include', 'libs/vkd3d-shader', 'libs/vkd3d-common', 'COPYING', 'LICENSE'))))
    shader = vkd3d / 'libs/vkd3d-shader'
    frontend = DRIVER / 'src/mesa/umd'
    version = spec['vkd3d']['version']
    macros = ['HAVE_BUILTIN_CLZ', 'HAVE_BUILTIN_POPCOUNT', 'HAVE_BUILTIN_ADD_OVERFLOW',
              'HAVE_ATOMIC_EXCHANGE_N', 'HAVE_SYNC_ADD_AND_FETCH',
              'HAVE_SYNC_BOOL_COMPARE_AND_SWAP', 'HAVE_SPIRV_UNIFIED1_SPIRV_H',
              'HAVE_SPIRV_UNIFIED1_GLSL_STD_450_H', 'HAVE_STDINT_H', 'HAVE_INTTYPES_H']
    write_changed(WORK / 'config.h', f'#define PACKAGE_VERSION "{version}"\n#define PACKAGE_STRING "vkd3d {version}"\n'
                  + ''.join(f'#define {macro} 1\n' for macro in macros))
    write_changed(WORK / 'unistd.h', '#pragma once\n#include <io.h>\n#include <process.h>\n'
                  '#define isatty _isatty\n#define fileno _fileno\n#define getpid _getpid\n')
    write_changed(WORK / 'vkd3d_version.h', f'#define VKD3D_VCS_ID "{version}"\n')
    for name in ('spirv.h', 'GLSL.std.450.h'):
        write_changed(WORK / 'spirv/unified1' / name, (mesa / 'src/compiler/spirv' / name).read_bytes())
    write_changed(WORK / 'a830_umd.def', 'LIBRARY a830_umd\nEXPORTS\nOpenAdapter10\nOpenAdapter10_2\n')

    includes = [WORK, vkd3d / 'include', vkd3d / 'include/private', shader, mesa / 'include',
                frontend.parent, DRIVER / 'src/qcom-gpu/wddm', frontend]
    flags = ['--target=aarch64-pc-windows-msvc', '/nologo', '/O2', '/MD', '/Brepro', '/DNDEBUG',
             '/D_CRT_SECURE_NO_WARNINGS', '/D_USE_MATH_DEFINES', '/DLIBVKD3D_SHADER_SOURCE',
             '/DVKD3D_NO_TRACE_MESSAGES', '/DVKD3D_NO_DEBUG_MESSAGES', '/DUSE_GCC_ATOMIC_BUILTINS',
             *clang_kit_includes(headers, WORK, external_names=True), *('/I' + str(p) for p in includes)]
    for name, extra in [('c', ['/std:c11', '/w']), ('cxx', ['/std:c++17', '/EHa', '/W4', '/WX',
                        '/clang:-Wno-unused-parameter', '/clang:-Wno-missing-field-initializers'])]:
        write_changed(WORK / f'{name}.rsp', subprocess.list2cmdline(flags + extra))
    # New entries in the Linux case-insensitive header overlay may shadow an
    # old include even when no already-recorded dependency has changed.
    overlay = 'kit-headers.json ' if os.name != 'nt' else ''
    available = {lib.name.lower(): lib for folder in reversed(libraries) for lib in folder.iterdir()
                 if lib.suffix.lower() == '.lib'}
    names = ('kernel32', 'user32', 'gdi32', 'advapi32', 'oldnames', 'uuid', 'msvcrt', 'msvcprt', 'vcruntime', 'ucrt')
    missing = [name for name in names if name + '.lib' not in available]
    if missing:
        raise ValueError('Missing ARM64 import libraries: ' + ', '.join(missing) + '. Check --vc-tools and --kit-lib.')
    libs = [available[name + '.lib'] for name in names]
    write_changed(WORK / 'link.rsp', subprocess.list2cmdline(
        ['/DLL', '/MACHINE:ARM64', '/Brepro', '/DYNAMICBASE', '/NXCOMPAT', '/DEF:a830_umd.def',
         *('/LIBPATH:' + str(p) for p in libraries), *map(str, libs)]))

    rules = []
    for language in ('c', 'cxx'):
        rules += [f'rule {language}',
                  f'  command = {shell_tool(cc)} @{language}.rsp /clang:-MD /clang:-MF /clang:$out.d '
                  '/clang:-MT /clang:$out /c $in /Fo$out',
                  '  depfile = $out.d', '  deps = gcc', '  description = Compile $in']
    rules += ['rule parser', f'  command = {shell_tool(bison)} -d -o $parser $in',
              'rule lexer', f'  command = {shell_tool(flex)} -o $out $in',
              'rule link', f'  command = {shell_tool(ld)} @link.rsp /OUT:$out $in',
              '  description = Link $out']
    sources_c = [vkd3d / p for p in sorted(vkd3d_files) if PurePosixPath(p).parent == PurePosixPath('libs/vkd3d-shader')
                 and p.endswith('.c')]
    sources_c += [vkd3d / 'libs/vkd3d-common' / (name + '.c') for name in ('debug', 'memory', 'error', 'utf8')]
    for name in ('hlsl', 'preproc'):
        rules += [f'build {name}.tab.c {name}.tab.h: parser {ninja_path(shader / (name+".y"))} | {ninja_path(bison)}',
                  f'  parser = {name}.tab.c',
                  f'build {name}.yy.c: lexer {ninja_path(shader / (name+".l"))} | {ninja_path(flex)}']
        sources_c += [Path(name + '.tab.c'), Path(name + '.yy.c')]
    sources_cxx = [frontend / (name + '.cpp') for name in ('core', 'driver', 'pipeline', 'query', 'resource', 'shader', 'state')]
    objects = []
    for src in sources_c + sources_cxx:
        obj = f'obj/{src.name}.obj'
        lang = 'cxx' if src.suffix == '.cpp' else 'c'
        rules.append(f'build {obj}: {lang} {ninja_path(src)} | {lang}.rsp {overlay}{ninja_path(cc)}'
                     ' || hlsl.tab.h preproc.tab.h')
        objects.append(obj)
    rules += [f'build {DLL}: link {" ".join(objects)} | a830_umd.def link.rsp {ninja_path(ld)} '
              + ' '.join(map(ninja_path, libs)), f'default {DLL}']
    (WORK / 'obj').mkdir(exist_ok=True)
    write_changed(WORK / 'build.ninja', '\n'.join(rules) + '\n')
    subprocess.run([ninja, '-C', WORK, '-j', str(args.jobs)], check=True)
    write_changed(OUTPUT / DLL, (WORK / DLL).read_bytes())
    for name in ('COPYING', 'LICENSE'):
        write_changed(OUTPUT / f'{name}.vkd3d', (vkd3d / name).read_bytes())
    print(OUTPUT / DLL)


def add_parser(subparsers):
    cmd = subparsers.add_parser('graphics-umd', help='Build the ARM64 Adreno Direct3D user-mode front end')
    cmd.add_argument('--vc-tools', type=Path, help='VC/Tools/MSVC/<version> with ARM64 C++ libraries (default: VCToolsInstallDir)')
    cmd.add_argument('--kit-include', type=Path, action='append', help='Windows SDK or WDK Include/<version> (repeat for split kits)')
    cmd.add_argument('--kit-lib', type=Path, action='append', help='Separate ARM64 SDK library directory (repeat for um and ucrt)')
    cmd.add_argument('--source-cache', type=Path, default=ROOT / 'build/source-cache/windows-graphics',
                     help='Directory for the checksum-verified Mesa and vkd3d source archives')
    cmd.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 4), help='Number of parallel C/C++ compilations')
    cmd.set_defaults(run=build)
