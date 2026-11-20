"""Compile the optional ARM64 Goodix CLI using the libfprint backend sources."""
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

import build_piano_fingerprint as fingerprint
from build import overlay_release
from build_piano_runtime_helpers import tree_digest, tree_files, verify_elf
import package_bsp


ROOT = Path(__file__).resolve().parents[2]
PACKAGE = package_bsp.FINGERPRINT_TOOLS
BINARY = 'piano-fingerprint-ctl'
DOCS = f'usr/share/doc/{PACKAGE}/'
QCBOR_SOURCES = ('UsefulBuf.c', 'qcbor_encode.c', 'qcbor_decode.c', 'qcbor_err_to_str.c', 'ieee754.c')


def compile_tool(work, uapi, compiler, emulator=None):
    cli_sha = fingerprint.digest(fingerprint.CLI_SOURCE)
    prepared = fingerprint.prepare_desktop_backend(work, uapi)
    backend = work / 'fingerprint/backend'
    qcbor = backend / 'qcbor'
    flags = [compiler, '-isystem', backend / 'uapi', '-O2', '-Wall', '-Wextra',
             f'-ffile-prefix-map={work}=.']
    sources = [qcbor / 'src' / name for name in QCBOR_SOURCES]
    sources += [backend / name for name in fingerprint.CORE_SOURCES] + [fingerprint.CLI_SOURCE]
    objects = []
    for source in sources:
        obj = work / source.with_suffix('.o').name
        warnings = [] if source.is_relative_to(qcbor) else ['-Werror']
        subprocess.run([*flags, *warnings, '-fPIC', '-D_GNU_SOURCE', '-DUSE_QCBOR',
                        '-I', qcbor / 'inc', '-c', source, '-o', obj], check=True)
        objects.append(obj)
    binary = work / BINARY
    subprocess.run([*flags, '-static', *objects, '-pthread', '-lm', '-o', binary], check=True)
    proof = verify_elf(binary)
    help_output = subprocess.check_output(([emulator] if emulator else []) + [binary, '--help'],
                                          text=True, timeout=10)
    if f'Usage: {BINARY}' not in help_output:
        raise ValueError('Goodix fingerprint tool help did not run')
    if (fingerprint.source_inputs() != prepared['inputs'] or
            fingerprint.digest(fingerprint.CLI_SOURCE) != cli_sha or
            fingerprint.digest(uapi / 'include/linux/tee.h') != prepared['uapi_headers']['linux/tee.h']):
        raise ValueError('Fingerprint sources changed while building the tool')
    return binary, {**prepared, 'client_source': str(fingerprint.CLI_SOURCE.relative_to(ROOT)),
                    'client_sha256': cli_sha, 'binary': proof,
                    'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0]}


def payload(work, binary, source):
    backend = work / 'fingerprint/backend'
    licenses = {name: backend / name for name in fingerprint.LICENSES}
    licenses['QCBOR.txt'] = backend / 'qcbor' / source['qcbor']['license']
    licenses['linux-tee.h'] = backend / 'uapi/linux/tee.h'
    files = [(f'usr/bin/{BINARY}', binary.read_bytes(), 0o755)]
    files += [(DOCS + name, path.read_bytes(), 0o644) for name, path in licenses.items()]
    text = ('Source: https://github.com/slwyts/Project-SunUEFI\n'
            f"QCBOR: {source['qcbor']['url']} ({source['qcbor']['commit']})\n\n").encode()
    for name, data, _ in files[1:]:
        text += f'{Path(name).name}:\n'.encode() + data + b'\n\n'
    files.append((DOCS + 'copyright', text, 0o644))
    return [({'path': name, 'mode': f'{mode:04o}', 'type': 'file',
              'sha256': package_bsp.sha(data)}, data) for name, data, mode in files]


def prepare(args):
    package_bsp.require(args.output is not None and not args.output.exists() and not args.output.is_symlink(),
                        'Use a fresh output directory')
    policy = json.loads((ROOT / 'config/fingerprint.json').read_text())
    version = args.version or policy['tools_version']
    package_bsp.require(re.fullmatch('[0-9][A-Za-z0-9.+]*', version), 'Invalid package version')
    profile = package_bsp.read_json(package_bsp.BSP / 'profiles.json')['profiles'][args.target]
    suite = args.suite or profile['default_suite']
    package_bsp.require(suite and re.fullmatch('[A-Za-z0-9_.-]+', suite), 'Specify an explicit target suite')
    uapi = args.fingerprint_tools.resolve()
    expected = fingerprint.source_inputs()['kernel_uapi_sha256']
    if tree_digest(tree_files(uapi / 'include')) != expected:
        raise ValueError('Use the exported headers for the current release kernel')
    arm = platform.machine() in ('arm64', 'aarch64')
    compiler = shutil.which('cc' if arm else 'aarch64-linux-gnu-gcc')
    emulator = shutil.which('qemu-aarch64-static') or shutil.which('qemu-aarch64')
    if not compiler or (not arm and not emulator):
        raise ValueError('Building the Goodix tool needs an ARM64 compiler and target execution for --help')
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fingerprint-tools-', dir=ROOT / 'build') as directory:
        work = Path(directory)
        binary, source = compile_tool(work, uapi, compiler, None if arm else emulator)
        rows = payload(work, binary, policy['sources'])
    report = {
        'schema_version': 1, 'component': 'fingerprint-tools', 'package': PACKAGE,
        'version': overlay_release.debian_version(version) if args.format == 'deb' else version,
        'target': {'distro': args.target, 'suite': suite, 'arch': args.arch},
        'package_architecture': 'aarch64' if args.format == 'pacman' else 'arm64',
        'features': [], 'depends': [], 'package_ready': False, 'files': {},
        'source': {**source, 'dependencies': policy['sources']},
        'payload_files': {row['path']: {**row, 'bytes': len(data)} for row, data in rows},
        'conflict_check': (package_bsp.conflict_check(args.against, rows, PACKAGE) if args.against
                           else {'performed': False, 'required_before_install': True}),
    }
    return report, rows
