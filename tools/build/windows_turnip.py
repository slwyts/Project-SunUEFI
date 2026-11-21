"""Prepare the pinned Windows Freedreno backend and build its Vulkan ICD."""
import configparser
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

if __package__:
    from . import windows_umd as umd
else:
    import windows_umd as umd


ICD = 'vulkan_freedreno.dll'
TARGET = 'src/freedreno/vulkan/' + ICD


def unpack(archive, parent):
    # Only regular source files are needed. In particular, Windows builders
    # should not need administrator rights to create documentation symlinks.
    with tarfile.open(archive, 'r|*') as tar:
        tar.extractall(parent, members=(m for m in tar if m.isdir() or m.isfile()), filter='data')


def directx_folder(tree, tag):
    wrap = configparser.ConfigParser(interpolation=None)
    wrap.read(tree / 'subprojects/DirectX-Headers.wrap')
    if wrap.get('wrap-git', 'revision') != tag:
        raise ValueError('The DirectX-Headers pin does not match this Mesa release')
    return tree / 'subprojects' / wrap.get('wrap-git', 'directory')


def prepare_mesa(spec, cache):
    mesa, dx = spec['mesa'], spec['directx_headers']
    archive = umd.download(cache, mesa['url'], mesa['sha256'])
    dx_archive = umd.download(cache, 'https://codeload.github.com/microsoft/DirectX-Headers/tar.gz/' + dx['commit'],
                              dx['sha256'], 'DirectX-Headers-' + dx['commit'] + '.tar.gz')
    top = 'mesa-' + mesa['version']
    tree = umd.WORK / 'sources' / (top + '-' + mesa['sha256'][:12])
    driver = umd.DRIVER / 'src'
    # Git for Windows may check out a diff with CRLF. Empty context lines in
    # this donor patch must be passed to git apply exactly as committed.
    patch_bytes = subprocess.check_output(['git', '-C', umd.DRIVER, 'cat-file', 'blob', 'HEAD:src/mesa/mesa.patch'])
    # Match the identifier supplied by the donor when it builds the Windows
    # backend, so both sides use the same shader/pipeline-cache identity.
    build_id = hashlib.sha256(mesa['commit'].encode() + patch_bytes).hexdigest()
    identity = {'mesa': mesa['sha256'], 'patch': hashlib.sha256(patch_bytes).hexdigest(),
                'directx': dx['sha256']}
    receipt = tree / '.turnip-source.json'
    if not (receipt.is_file() and json.loads(receipt.read_text()) == identity
            and (tree / 'meson.build').is_file() and (tree / 'subprojects/DirectX-Headers.wrap').is_file()
            and (directx_folder(tree, dx['tag']) / 'meson.build').is_file()):
        tree.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=tree.parent) as temp:
            stage = Path(temp)
            unpack(archive, stage)
            source = stage / top
            # This is an extracted source archive, not the repository above
            # it. Apply the patch only to the staged copy.
            env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(stage))
            for key in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR'):
                env.pop(key, None)
            subprocess.run(['git', 'apply', '--no-index', '--ignore-whitespace', '-'],
                           input=patch_bytes, cwd=source, env=env, check=True)
            unpack(dx_archive, stage)
            (stage / ('DirectX-Headers-' + dx['commit'])).rename(directx_folder(source, dx['tag']))
            umd.write_changed(source / receipt.name, json.dumps(identity, sort_keys=True))
            umd.source_receipt(source, mesa['sha256'], umd.MESA_HEADERS, umd.selected_files(source, umd.MESA_HEADERS))
            previous = stage / 'previous'
            if tree.exists():
                # The other valid form is the verified D3D-only header subset.
                if not (receipt.is_file() or (tree / '.sources.json').is_file()) or tree.is_symlink():
                    raise ValueError(f'Not a generated graphics source directory: {tree}')
                tree.rename(previous)
            try:
                source.rename(tree)
            except OSError:
                if previous.exists():
                    previous.rename(tree)
                raise

    # These files are shared by the Win32 KMD, Turnip and the Direct3D front
    # end. Restage changed files only; do not force a full Mesa rebuild.
    drm = tree / 'src/freedreno/drm'
    vulkan = tree / 'src/freedreno/vulkan'
    abi = (driver / 'qcom-gpu/wddm/abi.h').read_bytes()
    include = b'#include "../uapi.h"'
    if abi.count(include) != 1:
        raise ValueError('The layout of the Adreno KMD ABI header changed')
    for dest in (drm, vulkan):
        for name in ('a830_windows.h', 'a830_host.h', 'a830_completion.h'):
            umd.write_changed(dest / name, (driver / 'mesa' / name).read_bytes())
        umd.write_changed(dest / 'qcom_gpu_uapi.h', (driver / 'qcom-gpu/uapi.h').read_bytes())
        umd.write_changed(dest / 'a830_wddm_abi.h', abi.replace(include, b'#include "qcom_gpu_uapi.h"'))
    for dest, name in ((drm, 'fd_windows.c'), (vulkan, 'tu_knl_windows.cc')):
        umd.write_changed(dest / name, (driver / 'mesa' / name).read_bytes())

    # Let Mesa's wrap select its own zlib version and expected hashes, but
    # download over HTTPS and verify before handing any archive to Meson.
    wrap = configparser.ConfigParser(interpolation=None)
    wrap.read(tree / 'subprojects/zlib.wrap')
    zlib = wrap['wrap-file']
    for part in ('source', 'patch'):
        url = zlib[part + '_url']
        if url.startswith('http://'):
            url = 'https://' + url[len('http://'):]
        umd.download(cache, url, zlib[part + '_hash'], zlib[part + '_filename'])
    return tree, build_id, zlib['directory']


def linker_paths(libraries, work):
    if os.name == 'nt':
        return libraries
    # MSVC and SDK libraries use mixed filename casing. lld-link is a native
    # Linux process and does not share clang-cl's header overlay.
    files = {p.name.lower(): p for folder in reversed(libraries) for p in folder.iterdir() if p.suffix.lower() == '.lib'}
    key = hashlib.sha256('\n'.join(str(files[n]) for n in sorted(files)).encode()).hexdigest()
    aliases = work / ('libs-' + key[:16])
    aliases.mkdir(exist_ok=True)
    for name, target in files.items():
        link = aliases / name
        if not link.is_symlink():
            link.symlink_to(target)
    return [aliases, *libraries]


def resource_compiler(selected, headers, overlay):
    program = umd.tool(str(selected)) if selected else umd.tool('aarch64-w64-mingw32-windres', 'llvm-windres')
    if program.stem.lower() != 'llvm-windres':
        return [program.as_posix()]
    # Standalone LLVM has no implicit ARM64 sysroot (unlike llvm-mingw).
    flags = [program.as_posix(), '--target=aarch64-w64-mingw32',
             '--preprocessor-arg=--target=aarch64-pc-windows-msvc',
             *('--include-dir=' + p.as_posix() for p in headers)]
    if os.name != 'nt':
        flags += ['--preprocessor-arg=-ivfsoverlay', '--preprocessor-arg=' + overlay.as_posix()]
    return flags


def toolchain_identity(files):
    # Import libraries selected by /LIBPATH and compiler binaries are not
    # ordinary Ninja source dependencies. Detect an SDK or LLVM update in place.
    state = hashlib.sha256()
    for path in sorted(set(files)):
        stat = path.stat()
        state.update(f'{path}\0{stat.st_size}\0{stat.st_mtime_ns}\n'.encode())
    return state.hexdigest()


def build(args, spec, headers, libraries, kit_includes):
    meson, ninja = umd.tool('meson'), umd.tool('ninja')
    tree, build_id, zlib_dir = prepare_mesa(spec, args.source_cache.resolve())
    work = umd.WORK / 'turnip'
    work.mkdir(exist_ok=True)
    compiler, linker, librarian = umd.tool('clang-cl'), umd.tool('lld-link'), umd.tool('llvm-lib')
    if os.name == 'nt':
        # PATHEXT can return clang-cl.EXE. Meson checks this basename
        # case-sensitively to select the MSVC driver and linker arguments.
        compiler = compiler.with_name('clang-cl.exe')
    cc = [compiler.as_posix(), '--target=aarch64-pc-windows-msvc', '-fuse-ld=lld',
          *(f.replace('\\', '/') for f in kit_includes(headers, work, external_names=True))]
    # Generate deterministic COFF objects; lld derives the PE timestamp from
    # the image contents. Keep build and SDK paths out of __FILE__ and debug info.
    paths = [(tree, 'mesa'), (work, 'build'), *((p, 'kit/' + p.name) for p in headers)]
    compile_flags = ['/Brepro', *('/clang:-ffile-prefix-map=' + p.as_posix() + '=' + name for p, name in paths)]
    link = ['/Brepro', *('/LIBPATH:' + p.as_posix() for p in linker_paths(libraries, work))]
    windres = resource_compiler(args.windres, headers, work / 'kit-headers.json')
    # Meson machine files quote every argument separately, including paths
    # containing spaces. Drive letters and backslashes are normalized above.
    cross = work / 'arm64.ini'
    settings = (f"[binaries]\nc = {cc!r}\ncpp = {cc!r}\nar = {librarian.as_posix()!r}\n"
                f"strip = {umd.tool('llvm-strip').as_posix()!r}\nwindres = {windres!r}\n"
                "[host_machine]\nsystem = 'windows'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n"
                "[properties]\nneeds_exe_wrapper = true\n"
                f"[built-in options]\nc_args = {compile_flags!r}\ncpp_args = {compile_flags!r}\n"
                f"c_link_args = {link!r}\ncpp_link_args = {link!r}\n")
    umd.write_changed(cross, settings)
    options = ['--buildtype=release', '--wrap-mode=nodownload', '--force-fallback-for=zlib,DirectX-Headers',
               '-Dplatforms=windows', '-Dvulkan-drivers=freedreno', '-Dfreedreno-kmds=windows',
               '-Dgallium-drivers=freedreno', '-Dgallium-wgl-dll-name=a830_gl', '-Dllvm=disabled',
               '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled', '-Dgles1=disabled', '-Dgles2=disabled',
               '-Dopengl=true', '-Dshader-cache=disabled', '-Dexpat=disabled', '-Dzlib=enabled', '-Dzstd=disabled',
               '-Dbuild-tests=false', '-Dspirv-tools=disabled', '-Dtools=[]', '-Ddefault_library=static',
               '-Dtu-build-id=' + build_id]
    env = dict(os.environ, MESON_PACKAGE_CACHE_DIR=str(args.source_cache.resolve()))
    env['NINJA'] = str(ninja)
    # A changed cross file can select different compilers or library paths.
    # Meson caches compiler discovery, so replace that setup explicitly.
    objects = work / 'arm64'
    state = work / 'setup.json'
    files = [compiler, linker, librarian, meson, Path(windres[0])]
    files.extend(p for d in libraries for p in d.iterdir() if p.suffix.lower() == '.lib')
    header_map = umd.sha(work / 'kit-headers.json') if os.name != 'nt' else ''
    identity = json.dumps([settings, options, str(tree), spec['directx_headers']['sha256'],
                           toolchain_identity(files), header_map])
    initialized = (objects / 'build.ninja').is_file()
    if not initialized or not state.is_file() or state.read_text() != identity:
        subprocess.run([meson, 'setup', *(['--wipe'] if initialized else []), objects, tree,
                        '--cross-file', cross, *options], env=env, check=True)
        umd.write_changed(state, identity)
    subprocess.run([ninja, '-C', objects, '-j', str(args.jobs), TARGET], env=env, check=True)
    umd.write_changed(umd.OUTPUT / ICD, (objects / TARGET).read_bytes())
    manifest = {'file_format_version': '1.0.0', 'ICD': {'library_path': '.\\' + ICD, 'api_version': '1.4.0'}}
    umd.write_changed(umd.OUTPUT / 'freedreno_icd.json', json.dumps(manifest, indent=2) + '\n')
    # Retain component notices alongside the compiled DLL in the package.
    for source, name in ((tree / 'docs/license.rst', 'COPYING.Mesa'),
                         (directx_folder(tree, spec['directx_headers']['tag']) / 'LICENSE', 'LICENSE.DirectX-Headers'),
                         (tree / 'subprojects' / zlib_dir / 'LICENSE', 'LICENSE.zlib')):
        umd.write_changed(umd.OUTPUT / name, source.read_bytes())
    for path in (tree / 'licenses').rglob('*'):
        if path.is_file():
            umd.write_changed(umd.OUTPUT / 'licenses/Mesa' / path.relative_to(tree / 'licenses'), path.read_bytes())
    print(umd.OUTPUT / ICD)
    return tree
